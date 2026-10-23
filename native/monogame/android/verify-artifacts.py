#!/usr/bin/env python3
"""Verify pinned Android native outputs and (optionally) an APK's native payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections import Counter


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
SOURCE_ROOT = SCRIPT_DIR.parents[2]
TOOLCHAIN_PATH = SCRIPT_DIR / "toolchain.json"
SYSTEM_LIBRARIES = {
    "libaaudio.so", "libamidi.so", "libandroid.so", "libbinder_ndk.so", "libc.so",
    "libcamera2ndk.so", "libdl.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so",
    "libjnigraphics.so", "liblog.so", "libm.so", "libmediandk.so", "libnativewindow.so",
    "libOpenMAXAL.so", "libOpenSLES.so", "libvulkan.so", "libz.so",
}
PACKAGED_LIBRARIES = {"libSDL2.so", "libc++_shared.so", "libmgruntime.so"}
MACHINE_BY_ABI = {"x86_64": "Advanced Micro Devices X86-64", "arm64-v8a": "AArch64"}
RID_BY_ABI = {"x86_64": "android-x64", "arm64-v8a": "android-arm64"}
DECLARATION_EXCEPTION = {"MGP_Platform_StartRunLoop"}


class VerificationFailure(Exception):
    def __init__(self, reason: str, detail: str):
        super().__init__(detail)
        self.reason = reason
        self.detail = detail


def fail(reason: str, detail: str) -> None:
    raise VerificationFailure(reason, detail)


def digest(path: pathlib.Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def run(*args: str | pathlib.Path, cwd: pathlib.Path | None = None) -> str:
    try:
        result = subprocess.run([str(arg) for arg in args], cwd=cwd, check=True,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as error:
        output = getattr(error, "stdout", "") or ""
        fail("tool-execution-failed", f"Command failed: {args[0]} ({error}) {output.strip()}")
    return result.stdout


def require(condition: bool, reason: str, detail: str) -> None:
    if not condition:
        fail(reason, detail)


def source_fingerprint(repo: pathlib.Path) -> tuple[str, str]:
    commit = run("git", "-C", repo, "rev-parse", "HEAD").strip()
    names = subprocess.check_output(["git", "-C", str(repo), "ls-files", "--recurse-submodules", "-z"])
    hasher = hashlib.sha256()
    for raw_name in sorted(filter(None, names.split(b"\0"))):
        name = raw_name.decode()
        path = repo / name
        hasher.update(raw_name + b"\0")
        hasher.update(path.read_bytes() if path.is_file() else b"<missing>")
        hasher.update(b"\0")
    return commit, hasher.hexdigest()


def source_identities(manifest: dict) -> dict:
    sources = manifest.get("sources")
    expected_paths = {
        "monoGame": SOURCE_ROOT,
        "sdl2Wrapper": SOURCE_ROOT / "native/monogame/external/sdl2",
        "sdl2": SOURCE_ROOT / "native/monogame/external/sdl2/sdl",
        "faudio": SOURCE_ROOT / "native/monogame/external/faudio",
    }
    require(isinstance(sources, dict) and set(sources) == set(expected_paths),
            "invalid-build-manifest", "expected all four registered MonoGame/SDL/FAudio source identities")
    result = {}
    for name, record in sources.items():
        require(isinstance(record, dict) and record.get("path") and record.get("commit") and
                record.get("trackedContentSha256"), "invalid-build-manifest",
                f"source identity is incomplete: {name}")
        repo = pathlib.Path(record["path"]).resolve()
        expected_repo = expected_paths[name].resolve()
        require(repo == expected_repo, "wrong-source-path",
                f"{name} source path is {repo}; expected checked-out source {expected_repo}")
        require(repo.is_dir(), "source-missing", f"source checkout is unavailable: {repo}")
        try:
            current_commit, current_hash = source_fingerprint(repo)
        except (OSError, subprocess.CalledProcessError) as error:
            fail("source-identity-unavailable", f"cannot fingerprint {name}: {error}")
        require(current_commit == record["commit"] and current_hash == record["trackedContentSha256"],
                "stale-source", f"{name} source identity differs from the build manifest")
        result[name] = {"commit": current_commit, "trackedContentSha256": current_hash}
    return result


def expected_api_exports(configuration: str = "Debug") -> set[str]:
    pattern = re.compile(r"MG_EXPORT\s+[^;{]*?\b((?:MGP|MGG|MGA|MGI|MGM|MG_Asset)_\w+)\s*\(")
    expected = set()
    for header in (SOURCE_ROOT / "native/monogame/include").glob("api_*.h"):
        if header.name == "api_MGG_AndroidTrace.h" and configuration != "Debug":
            continue
        expected.update(pattern.findall(header.read_text(encoding="utf-8")))
    return expected - DECLARATION_EXCEPTION


def expected_startup_exports(manifest: dict) -> set[str]:
    exports = manifest.get("exports", {})
    declarations = set(exports.get("expected", [])) if isinstance(exports, dict) else set()
    declared = set()
    for key in ("androidStartupExports", "startupExports", "entryPointExports"):
        values = manifest.get(key)
        if isinstance(values, list):
            declared.update(item for item in values if isinstance(item, str))
    if isinstance(exports, dict):
        for key in ("androidStartup", "startup", "entryPoints"):
            values = exports.get(key)
            if isinstance(values, list):
                declared.update(item for item in values if isinstance(item, str))
    # Future T07 build helpers may register Android entry symbols in expected.
    declared.update(item for item in declarations
                    if item == "SDL_main" or item.startswith(("MGP_Android_", "MG_Android_", "MGN_")))
    return declared


def expected_sdl_exports() -> set[str]:
    return {
        "JNI_OnLoad",
        "Java_org_libsdl_app_SDLActivity_nativeSetupJNI",
        "Java_org_libsdl_app_SDLActivity_nativeRunMain",
        "Java_org_libsdl_app_SDLAudioManager_nativeSetupJNI",
        "Java_org_libsdl_app_SDLControllerManager_nativeSetupJNI",
    }


def inspect_elf(path: pathlib.Path, readelf: pathlib.Path, abi: str,
                expected_symbols: set[str], packaged_names: set[str]) -> dict:
    header = run(readelf, "--file-header", path)
    match = re.search(r"Machine:\s+(.+)", header)
    require(match is not None, "invalid-elf", f"ELF machine is unreadable: {path.name}")
    machine = match.group(1).strip()
    require(machine == MACHINE_BY_ABI[abi], "wrong-architecture",
            f"{path.name} has machine {machine}; expected {MACHINE_BY_ABI[abi]}")

    dynamic = run(readelf, "--dynamic", "--wide", path)
    needed = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", dynamic)
    missing = sorted(set(needed) - packaged_names - SYSTEM_LIBRARIES)
    require(not missing, "missing-dependency", f"{path.name} needs unavailable libraries: {missing}")

    program_headers = run(readelf, "--program-headers", "--wide", path)
    alignments = [int(line.split()[-1], 16) for line in program_headers.splitlines()
                  if line.strip().startswith("LOAD ")]
    require(bool(alignments) and all(value >= 16384 for value in alignments),
            "invalid-elf-alignment", f"{path.name} LOAD alignments are below 16 KB: {alignments}")

    symbols = set()
    if expected_symbols:
        for line in run(readelf, "--dyn-syms", "--wide", path).splitlines():
            fields = line.split()
            if len(fields) >= 8 and fields[4] == "GLOBAL" and fields[6] != "UND":
                symbols.add(fields[7].split("@", 1)[0])
    missing_symbols = sorted(expected_symbols - symbols)
    require(not missing_symbols, "missing-export", f"{path.name} is missing required exports: {missing_symbols}")
    return {"machine": machine, "needed": needed, "loadSegmentAlignments": alignments,
            "exportCount": len(expected_symbols), "sha256": digest(path), "bytes": path.stat().st_size}


def validate_build_inputs(manifest: dict) -> dict:
    records = manifest.get("buildInputs")
    require(isinstance(records, dict), "invalid-build-manifest", "buildInputs object is missing")
    required = ("CMakeLists.txt", "build.sh", "toolchain.json", "generate-shaders.sh",
                "validate-output-paths.py", "verify-runtime.py", "verify-artifacts.py")
    require(set(records) == set(required), "incomplete-build-manifest",
            "buildInputs must fingerprint all build and verification scripts")
    for name in required:
        record = records.get(name)
        require(isinstance(record, dict) and record.get("path") and record.get("sha256"),
                "incomplete-build-manifest", f"build input fingerprint is missing: {name}")
        path = pathlib.Path(record["path"])
        require(path.resolve() == (SCRIPT_DIR / name).resolve() and path.is_file() and
                digest(path) == record["sha256"], "stale-build-input",
                f"build input changed since native output was produced: {name}")
    return {name: records[name]["sha256"] for name in required}


def validate_native(native_root: pathlib.Path, abi: str, configuration: str) -> tuple[dict, pathlib.Path]:
    toolchain = json.loads(TOOLCHAIN_PATH.read_text(encoding="utf-8"))
    require(toolchain.get("schemaVersion") == 1 and abi in toolchain.get("abis", []) and
            configuration in toolchain.get("configurations", []), "invalid-toolchain-pin",
            "toolchain.json does not permit this ABI/configuration")
    artifact_dir = native_root / "artifacts" / abi / configuration
    manifest_path = artifact_dir / "build-manifest.json"
    require(manifest_path.is_file(), "missing-build-manifest", f"missing {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail("invalid-build-manifest", str(error))
    require(manifest.get("schemaVersion") == 1 and manifest.get("phase") == "runtime" and
            manifest.get("result") == "ok", "invalid-build-manifest",
            "expected a completed runtime build manifest with result=ok")
    require(manifest.get("abi") == abi, "wrong-abi", f"manifest ABI is {manifest.get('abi')}, requested {abi}")
    require(manifest.get("configuration") == configuration, "wrong-configuration",
            f"manifest configuration is {manifest.get('configuration')}, requested {configuration}")
    for key, expected in (("ndkRevision", toolchain["ndkRevision"]), ("minApi", toolchain["minApi"]),
                          ("compileSdk", toolchain["compileSdk"]), ("stl", "c++_shared")):
        require(manifest.get(key) == expected, "wrong-toolchain", f"{key} is {manifest.get(key)!r}; expected {expected!r}")
    require(pathlib.Path(manifest.get("artifactDirectory", "")).resolve() == artifact_dir.resolve(),
            "wrong-artifact-path", "manifest artifactDirectory does not match the selected native root")

    toolchain_path = pathlib.Path(manifest.get("resolved", {}).get("ndkRoot", ""))
    properties = toolchain_path / "source.properties"
    require(properties.is_file(), "pinned-ndk-unavailable", f"NDK source.properties missing at {properties}")
    revision = re.search(r"^Pkg\.Revision\s*=\s*(.+)$", properties.read_text(), re.MULTILINE)
    require(revision is not None and revision.group(1).strip() == toolchain["ndkRevision"],
            "wrong-toolchain", "resolved NDK revision differs from the pinned revision")
    sdk_root = pathlib.Path(manifest.get("resolved", {}).get("sdkRoot", ""))
    android_jar = sdk_root / "platforms" / f"android-{toolchain['compileSdk']}" / "android.jar"
    require(android_jar.is_file(), "pinned-sdk-unavailable", f"pinned Android platform is missing: {android_jar}")
    build_tools_version = toolchain.get("buildToolsVersion")
    require(isinstance(build_tools_version, str), "invalid-toolchain-pin", "buildToolsVersion is not pinned")
    zipalign = sdk_root / "build-tools" / build_tools_version / "zipalign"
    require(zipalign.is_file() and os.access(zipalign, os.X_OK), "pinned-zipalign-unavailable",
            f"pinned zipalign is unavailable: {zipalign}")
    readelf = toolchain_path / "toolchains/llvm/prebuilt" / manifest["resolved"]["ndkHostTag"] / "bin/llvm-readelf"
    require(readelf.is_file() and os.access(readelf, os.X_OK), "pinned-readelf-unavailable",
            f"pinned llvm-readelf is unavailable: {readelf}")
    java = str(manifest.get("tools", {}).get("java", ""))
    java_major = re.search(r'version "(\d+)', java)
    require(java_major is not None and int(java_major.group(1)) == toolchain["javaMajorVersion"],
            "wrong-toolchain", "manifest Java major version differs from toolchain.json")

    input_hashes = validate_build_inputs(manifest)
    source_hashes = source_identities(manifest)
    library_records = manifest.get("libraries", {})
    names_to_records = {"libSDL2.so": "sdl2", "libc++_shared.so": "cxxShared", "libmgruntime.so": "mgruntime"}
    for name, record_key in names_to_records.items():
        path = artifact_dir / name
        require(path.is_file(), "missing-library", f"required library is missing: {path}")
        record = library_records.get(record_key)
        require(isinstance(record, dict) and record.get("path") and record.get("sha256"),
                "invalid-build-manifest", f"manifest library record is missing: {record_key}")
        require(pathlib.Path(record["path"]).resolve() == path.resolve() and digest(path) == record["sha256"],
                "stale-artifact", f"{name} does not match its manifest hash/path")
    required_exports = expected_api_exports(manifest["configuration"]) | expected_startup_exports(manifest)
    trace = manifest.get("androidTrace")
    trace_names = ("vulkan/MGG_Vulkan.cpp", "vulkan/MGG_VulkanTrace.inl", "include/api_MGG_AndroidTrace.h")
    trace_header = SOURCE_ROOT / "native/monogame/include/api_MGG_AndroidTrace.h"
    trace_symbols = set(re.findall(r"MG_EXPORT\s+[^;{]*?\b(MGG_AndroidTrace_\w+)\s*\(", trace_header.read_text()))
    debug_trace = manifest["configuration"] == "Debug"
    require(isinstance(trace, dict) and trace.get("schemaVersion") == 1 and
            trace.get("available") is debug_trace and trace.get("apiVersion") == 1 and
            trace.get("eventBytes") == 320 and trace.get("capacity") == 32768 and
            trace.get("exports") == (sorted(trace_symbols) if debug_trace else []) and
            isinstance(trace.get("inputs"), dict) and set(trace["inputs"]) == set(trace_names),
            "invalid-trace-manifest", "exact configuration-bound trace ABI/input manifest required")
    for name in trace_names:
        record = trace["inputs"][name]
        expected_path = SOURCE_ROOT / "native/monogame" / name
        require(isinstance(record, dict) and record.get("path") == str(expected_path) and
                record.get("sha256") == digest(expected_path) and record.get("bytes") == expected_path.stat().st_size,
                "stale-trace-source", f"trace input differs: {name}")
    if not debug_trace:
        actual_exports = run(readelf, "--dyn-syms", "--wide", artifact_dir / "libmgruntime.so")
        require(not any(re.search(r"\b" + re.escape(name) + r"\b", actual_exports) for name in trace_symbols),
                "release-trace-export", "Android Release must not export Debug trace API")
    elfs = {}
    for name in sorted(PACKAGED_LIBRARIES):
        elfs[name] = inspect_elf(artifact_dir / name, readelf, abi,
                                 (required_exports if name == "libmgruntime.so" else
                                  expected_sdl_exports() if name == "libSDL2.so" else set()),
                                 PACKAGED_LIBRARIES)
    return ({"manifest": str(manifest_path), "abi": abi, "configuration": configuration,
             "toolchain": {"ndkRevision": toolchain["ndkRevision"], "minApi": toolchain["minApi"],
                           "compileSdk": toolchain["compileSdk"], "buildToolsVersion": build_tools_version,
                           "javaMajorVersion": toolchain["javaMajorVersion"]},
             "buildInputSha256": input_hashes, "sources": source_hashes,
             "requiredExports": sorted(required_exports), "elf": elfs,
             "zipalign": str(zipalign), "readelf": str(readelf)}, artifact_dir)


def validate_apk(apk: pathlib.Path, abi: str, native: dict, artifact_dir: pathlib.Path) -> dict:
    require(apk.is_file(), "missing-apk", f"APK does not exist: {apk}")
    require(zipfile.is_zipfile(apk), "invalid-apk", "APK is not a readable ZIP archive")
    expected = {f"lib/{abi}/{name}" for name in PACKAGED_LIBRARIES}
    readelf = pathlib.Path(native["readelf"])
    native_details = {}
    try:
        with zipfile.ZipFile(apk) as archive:
            all_entries = archive.infolist()
            so_entries = [entry.filename for entry in all_entries if entry.filename.endswith(".so")]
            duplicates = sorted(name for name, count in Counter(so_entries).items() if count != 1)
            require(not duplicates, "duplicate-native-entry", f"duplicate native APK entries: {duplicates}")
            require(set(so_entries).issuperset(expected),
                    "missing-runtime-entry", f"APK is missing required native entries: {sorted(expected - set(so_entries))}")
            invalid_paths = sorted(name for name in so_entries if not name.startswith(f"lib/{abi}/"))
            require(not invalid_paths, "unexpected-native-path",
                    f"APK contains a wrong-ABI, desktop, or non-runtime native path: {invalid_paths}")
            invalid_paths = sorted(name for name in so_entries
                                   if len(pathlib.PurePosixPath(name).parts) != 3 or
                                   pathlib.PurePosixPath(name).parts[:2] != ("lib", abi))
            require(not invalid_paths, "unexpected-native-path",
                    f"APK native libraries must be direct children of lib/{abi}: {invalid_paths}")
            basenames = [pathlib.PurePosixPath(name).name for name in so_entries]
            duplicate_names = sorted(name for name, count in Counter(basenames).items() if count != 1)
            require(not duplicate_names, "duplicate-native-library",
                    f"APK contains duplicate native basenames: {duplicate_names}")
            apk_packaged_names = set(basenames)
            for name in sorted(so_entries):
                info = archive.getinfo(name)
                payload = archive.read(info)
                native_name = pathlib.Path(name).name
                if native_name in PACKAGED_LIBRARIES:
                    require(hashlib.sha256(payload).hexdigest() == native["elf"][native_name]["sha256"],
                            "stale-apk-runtime", f"APK {name} differs from the selected native build")
                with tempfile.NamedTemporaryFile(prefix="mg-apk-elf-", suffix=".so", delete=False) as extracted:
                    extracted.write(payload)
                    extracted_path = pathlib.Path(extracted.name)
                try:
                    expected_symbols = (set(native["requiredExports"]) if native_name == "libmgruntime.so" else
                                        expected_sdl_exports() if native_name == "libSDL2.so" else set())
                    native_details[name] = inspect_elf(extracted_path, readelf, abi,
                                                        expected_symbols, apk_packaged_names)
                    native_details[name]["archiveCompression"] = {
                        zipfile.ZIP_STORED: "stored", zipfile.ZIP_DEFLATED: "deflated"
                    }.get(info.compress_type, f"zip-method-{info.compress_type}")
                finally:
                    extracted_path.unlink(missing_ok=True)
    except (OSError, zipfile.BadZipFile, KeyError) as error:
        if isinstance(error, VerificationFailure):
            raise
        fail("invalid-apk", str(error))
    zipalign = pathlib.Path(native["zipalign"])
    check = subprocess.run([str(zipalign), "-c", "-P", "16", "-v", "4", str(apk)],
                           text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    require(check.returncode == 0, "invalid-apk-alignment",
            f"zipalign 16 KB check failed: {check.stdout.strip()}")
    return {"path": str(apk.resolve()), "sha256": digest(apk), "bytes": apk.stat().st_size,
            "nativeEntries": sorted(so_entries), "runtimeEntries": sorted(expected),
            "elf": native_details, "alignment": "zipalign -c -P 16 -v 4 passed"}


def self_test(native_root: pathlib.Path, abi: str, configuration: str) -> dict:
    """Build real temporary ELF and APK inputs that must fail for targeted reasons."""
    toolchain = json.loads(TOOLCHAIN_PATH.read_text(encoding="utf-8"))
    manifest_path = native_root / "artifacts" / abi / configuration / "build-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    ndk = pathlib.Path(manifest["resolved"]["ndkRoot"])
    clang = ndk / "toolchains/llvm/prebuilt" / manifest["resolved"]["ndkHostTag"] / "bin/clang"
    require(clang.is_file(), "self-test-tool-missing", f"NDK clang is missing: {clang}")
    sdk = pathlib.Path(manifest["resolved"]["sdkRoot"])
    zipalign = sdk / "build-tools" / toolchain["buildToolsVersion"] / "zipalign"
    readelf = ndk / "toolchains/llvm/prebuilt" / manifest["resolved"]["ndkHostTag"] / "bin/llvm-readelf"
    opposite_abi = "arm64-v8a" if abi == "x86_64" else "x86_64"
    opposite_target = "aarch64-linux-android24" if opposite_abi == "arm64-v8a" else "x86_64-linux-android24"
    current_target = "aarch64-linux-android24" if abi == "arm64-v8a" else "x86_64-linux-android24"
    observed = []

    def expect(reason: str, operation) -> None:
        try:
            operation()
        except VerificationFailure as error:
            require(error.reason == reason, "self-test-wrong-reason",
                    f"expected {reason}, got {error.reason}: {error.detail}")
            observed.append({"case": reason, "result": "rejected", "reason": error.reason})
            return
        fail("self-test-unexpected-pass", f"negative fixture unexpectedly passed: {reason}")

    with tempfile.TemporaryDirectory(prefix="mg-android-artifact-fixtures-") as temporary:
        temp = pathlib.Path(temporary)
        source = temp / "fixture.c"
        source.write_text("int fixture(void) { return 7; }\n")
        wrong = temp / "wrong-architecture.so"
        run(clang, f"--target={opposite_target}", "-fPIC", "-shared", "-Wl,-z,max-page-size=16384",
            source, "-o", wrong)
        expect("wrong-architecture", lambda: inspect_elf(wrong, readelf, abi, set(), PACKAGED_LIBRARIES))

        missing = temp / "missing-dependency.so"
        dep_source = temp / "dep.c"
        dep_source.write_text("int dependency(void) { return 3; }\n")
        run(clang, f"--target={current_target}", "-fPIC", "-shared", "-Wl,-soname,libmissing_fixture.so",
            dep_source, "-o", temp / "libmissing_fixture.so")
        run(clang, f"--target={current_target}", "-fPIC", "-shared", "-Wl,-z,max-page-size=16384",
            "-Wl,--no-as-needed", "-L", temp, "-lmissing_fixture", source, "-o", missing)
        expect("missing-dependency", lambda: inspect_elf(missing, readelf, abi, set(), PACKAGED_LIBRARIES))

        bad_alignment = temp / "bad-alignment.so"
        run(clang, f"--target={current_target}", "-fPIC", "-shared", "-Wl,-z,max-page-size=4096",
            source, "-o", bad_alignment)
        expect("invalid-elf-alignment",
               lambda: inspect_elf(bad_alignment, readelf, abi, set(), PACKAGED_LIBRARIES))

        bad_apk = temp / "unaligned.apk"
        with zipfile.ZipFile(bad_apk, "w") as archive:
            archive.writestr("assets/pad", b"x", compress_type=zipfile.ZIP_STORED)
            for name in sorted(PACKAGED_LIBRARIES):
                archive.write(native_root / "artifacts" / abi / configuration / name,
                              f"lib/{abi}/{name}", compress_type=zipfile.ZIP_STORED)
        aligned_check = subprocess.run([str(zipalign), "-c", "-P", "16", "4", str(bad_apk)],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        require(aligned_check.returncode != 0, "self-test-unexpected-pass",
                "unaligned ZIP fixture passed zipalign")
        failure_json = temp / "expected-failure.json"
        verifier = SCRIPT_DIR / "verify-artifacts.py"
        cli = subprocess.run([sys.executable, str(verifier), "--native-root", str(native_root),
                              "--configuration", configuration, "--abi", abi, "--apk", str(bad_apk),
                              "--output", str(failure_json)], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True)
        require(cli.returncode != 0 and failure_json.is_file(), "self-test-unexpected-pass",
                "full APK verifier accepted an unaligned generated APK or did not emit JSON")
        failure_report = json.loads(failure_json.read_text())
        require(failure_report.get("result") == "failed" and
                failure_report.get("errors", [{}])[0].get("reason") == "invalid-apk-alignment",
                "self-test-wrong-reason", "full APK verifier did not return invalid-apk-alignment")
        observed.append({"case": "invalid-apk-alignment", "result": "rejected",
                         "reason": "invalid-apk-alignment", "exitCode": cli.returncode,
                         "reportResult": failure_report["result"]})
    return {"result": "ok", "fixtureCompiler": run(clang, "--version").splitlines()[0],
            "generatedTemporaryFixtures": observed}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-root", type=pathlib.Path, required=True,
                        help="external root containing artifacts/<abi>/<configuration>")
    parser.add_argument("--configuration", choices=("Debug", "Release"), required=True)
    parser.add_argument("--abi", choices=tuple(MACHINE_BY_ABI), required=True,
                        help="Android ABI whose native artifacts are being checked")
    parser.add_argument("--apk", type=pathlib.Path, help="optional APK to inspect and align-check")
    parser.add_argument("--output", type=pathlib.Path, help="write the JSON result to this external path")
    parser.add_argument("--self-test", action="store_true",
                        help="also generate temporary ELF/ZIP negative fixtures and prove rejection")
    args = parser.parse_args()
    report = {"schemaVersion": 1, "result": "failed", "checks": [], "errors": []}
    exit_code = 1
    try:
        native, artifact_dir = validate_native(args.native_root.resolve(), args.abi, args.configuration)
        report["native"] = native
        report["checks"].append({"name": "native-artifacts", "result": "passed"})
        if args.apk:
            report["apk"] = validate_apk(args.apk.resolve(), args.abi, native, artifact_dir)
            report["checks"].append({"name": "apk-structure-and-alignment", "result": "passed"})
        if args.self_test:
            report["negativeFixtures"] = self_test(args.native_root.resolve(), args.abi, args.configuration)
            report["checks"].append({"name": "generated-negative-fixtures", "result": "passed"})
        report["result"] = "ok"
        exit_code = 0
    except VerificationFailure as error:
        report["errors"].append({"reason": error.reason, "detail": error.detail})
    except (OSError, json.JSONDecodeError, KeyError, ValueError, subprocess.CalledProcessError) as error:
        report["errors"].append({"reason": "verification-error", "detail": str(error)})
    except Exception as error:
        report["errors"].append({"reason": "internal-verifier-error", "detail": f"{type(error).__name__}: {error}"})
    rendered = json.dumps(report, indent=2, sort_keys=True)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
