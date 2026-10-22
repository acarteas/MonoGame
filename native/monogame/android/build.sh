#!/usr/bin/env bash
set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly NATIVE_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
readonly SOURCE_ROOT="$(cd -- "${NATIVE_ROOT}/../.." && pwd -P)"
original_args=("$@")

usage() {
    cat >&2 <<'USAGE'
Usage: build.sh --build-root DIR [--sdk-root DIR] [--ndk-root DIR]
                [--abi x86_64|arm64-v8a] [--configuration Debug|Release]
                [--preflight-only]

Builds vendored shared SDL2 and static PIC FAudio for both ABIs by default.
--preflight-only validates the tools without configuring or building. The root must be an
existing directory owned and writable by the caller, outside the SDK, NDK,
source checkout, and desktop dependency build roots. ANDROID_HOME is used when
--sdk-root is omitted. ANDROID_NDK_ROOT is not implicitly selected.
USAGE
}

fail() {
    printf 'android preflight error: %s\n' "$1" >&2
    exit 2
}

is_allowed() {
    local wanted="$1"
    shift
    local candidate
    for candidate in "$@"; do
        [[ "$wanted" == "$candidate" ]] && return 0
    done
    return 1
}

canonical_existing_dir() {
    local label="$1" path="$2"
    [[ -n "$path" ]] || fail "$label is empty"
    [[ -d "$path" ]] || fail "$label does not exist or is not a directory: $path"
    realpath -e -- "$path"
}

is_same_or_below() {
    local path="$1" root="$2"
    [[ "$path" == "$root" || "$path" == "$root"/* ]]
}

is_same_or_above() {
    local path="$1" protected="$2"
    [[ "$protected" == "$path" || "$protected" == "$path"/* ]]
}

command -v python3 >/dev/null 2>&1 || fail "required host tool is unavailable: python3 (needed to read toolchain.json and write preflight evidence)"
toolchain_values="$(python3 - "${SCRIPT_DIR}/toolchain.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    config = json.load(source)
required = {"schemaVersion", "ndkRevision", "minApi", "compileSdk", "abis", "configurations", "javaMajorVersion"}
if config.get("schemaVersion") != 1 or not required.issubset(config):
    raise SystemExit("toolchain.json has an unsupported schema or missing required fields")
if not isinstance(config["abis"], list) or not isinstance(config["configurations"], list):
    raise SystemExit("toolchain.json ABI and configuration values must be arrays")
for key in ("ndkRevision", "minApi", "compileSdk", "javaMajorVersion"):
    print(config[key])
print(" ".join(config["abis"]))
print(" ".join(config["configurations"]))
PY
)" || fail "could not read ${SCRIPT_DIR}/toolchain.json"
mapfile -t toolchain_config <<<"$toolchain_values"
[[ ${#toolchain_config[@]} -eq 6 ]] || fail "toolchain.json does not contain the expected pinned values"
readonly EXPECTED_NDK_REVISION="${toolchain_config[0]}"
readonly EXPECTED_API="${toolchain_config[1]}"
readonly EXPECTED_COMPILE_SDK="${toolchain_config[2]}"
readonly EXPECTED_JAVA_MAJOR="${toolchain_config[3]}"
read -r -a ALLOWED_ABIS <<<"${toolchain_config[4]}"
read -r -a ALLOWED_CONFIGURATIONS <<<"${toolchain_config[5]}"

version_line() {
    local output="$1"
    printf '%s\n' "$output" | head -n 1 | sed -E 's/^[^0-9]*([0-9]+(\.[0-9]+)*).*/\1/'
}

sdk_root_arg=""
ndk_root_arg=""
build_root_arg=""
abi="all"
configuration="Debug"
preflight_only=false
while (($#)); do
    case "$1" in
        --sdk-root|--ndk-root|--build-root|--abi|--configuration)
            (($# >= 2)) || { usage; fail "missing value for $1"; }
            option="$1"; value="$2"; shift 2
            case "$option" in
                --sdk-root) sdk_root_arg="$value" ;;
                --ndk-root) ndk_root_arg="$value" ;;
                --build-root) build_root_arg="$value" ;;
                --abi) abi="$value" ;;
                --configuration) configuration="$value" ;;
            esac
            ;;
        --preflight-only)
            preflight_only=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            fail "unknown option: $1"
            ;;
    esac
done

[[ -n "$build_root_arg" ]] || { usage; fail "--build-root is required"; }
[[ "$abi" == all ]] || is_allowed "$abi" "${ALLOWED_ABIS[@]}" || fail "unsupported ABI '$abi'; allowed: ${ALLOWED_ABIS[*]}"
is_allowed "$configuration" "${ALLOWED_CONFIGURATIONS[@]}" || fail "unsupported configuration '$configuration'; allowed: ${ALLOWED_CONFIGURATIONS[*]}"

if [[ -n "$sdk_root_arg" ]]; then
    sdk_root="$(canonical_existing_dir "Android SDK root" "$sdk_root_arg")"
elif [[ -n "${ANDROID_HOME:-}" ]]; then
    sdk_root="$(canonical_existing_dir "ANDROID_HOME" "$ANDROID_HOME")"
else
    fail "Android SDK not specified; use --sdk-root or set ANDROID_HOME"
fi

sdk_ndk="${sdk_root}/ndk/${EXPECTED_NDK_REVISION}"
if [[ -n "$ndk_root_arg" ]]; then
    ndk_root="$(canonical_existing_dir "Android NDK root" "$ndk_root_arg")"
else
    ndk_root="$(canonical_existing_dir "pinned NDK ${EXPECTED_NDK_REVISION}" "$sdk_ndk")"
fi

[[ -f "${ndk_root}/source.properties" ]] || fail "NDK source.properties is missing: ${ndk_root}"
ndk_revision="$(sed -n 's/^[[:space:]]*Pkg.Revision[[:space:]]*=[[:space:]]*//p' "${ndk_root}/source.properties" | head -n 1 | tr -d '\r')"
[[ "$ndk_revision" == "$EXPECTED_NDK_REVISION" ]] || fail "NDK revision mismatch: expected ${EXPECTED_NDK_REVISION}, found '${ndk_revision:-unknown}'"

host_tag=""
case "$(uname -s):$(uname -m)" in
    Linux:x86_64) host_tag="linux-x86_64" ;;
    Linux:aarch64|Linux:arm64) host_tag="linux-aarch64" ;;
    Darwin:x86_64) host_tag="darwin-x86_64" ;;
    Darwin:arm64) host_tag="darwin-arm64" ;;
    *) fail "unsupported host for Android NDK: $(uname -s)/$(uname -m)" ;;
esac
llvm_bin="${ndk_root}/toolchains/llvm/prebuilt/${host_tag}/bin"
for required in clang clang++ llvm-ar llvm-readelf; do
    [[ -x "${llvm_bin}/${required}" ]] || fail "NDK LLVM toolchain is incomplete: missing executable ${llvm_bin}/${required}"
done

[[ -f "${sdk_root}/platforms/android-${EXPECTED_COMPILE_SDK}/android.jar" ]] || fail "Android SDK platform android-${EXPECTED_COMPILE_SDK} is missing under ${sdk_root}/platforms"

for tool in cmake ninja java javac jar readelf realpath git; do
    command -v "$tool" >/dev/null 2>&1 || fail "required host tool is unavailable: $tool"
done
cmake_version="$(version_line "$(cmake --version 2>&1)")"
ninja_version="$(version_line "$(ninja --version 2>&1)")"
java_version_output="$(java -version 2>&1 | head -n 1)"
java_major="$(sed -nE 's/.*version "([0-9]+).*/\1/p' <<<"$java_version_output")"
[[ "$java_major" == "$EXPECTED_JAVA_MAJOR" ]] || fail "Java ${EXPECTED_JAVA_MAJOR} is required; found '${java_version_output:-unrecognized version}'"
javac_version_output="$(javac -version 2>&1 | head -n 1)"
javac_major="$(sed -nE 's/^[^0-9]*([0-9]+).*/\1/p' <<<"$javac_version_output")"
[[ "$javac_major" == "$EXPECTED_JAVA_MAJOR" ]] || fail "javac ${EXPECTED_JAVA_MAJOR} is required; found '${javac_version_output:-unrecognized version}'"
jar_version_output="$(jar --version 2>&1 | head -n 1)"
jar_major="$(sed -nE 's/^[^0-9]*([0-9]+).*/\1/p' <<<"$jar_version_output")"
[[ "$jar_major" == "$EXPECTED_JAVA_MAJOR" ]] || fail "jar ${EXPECTED_JAVA_MAJOR} is required; found '${jar_version_output:-unrecognized version}'"
readelf_version="$(version_line "$(readelf --version 2>&1)")"
llvm_readelf_version="$("${llvm_bin}/llvm-readelf" --version 2>&1 | tr '\n' ' ' | sed -E 's/[[:space:]]+/ /g; s/[[:space:]]+$//')"

[[ -d "$build_root_arg" ]] || fail "caller-owned --build-root must already exist: $build_root_arg"
build_root="$(canonical_existing_dir "build root" "$build_root_arg")"
[[ -w "$build_root" ]] || fail "build root is not writable: ${build_root}"
owner_uid="$(stat -c '%u' -- "$build_root" 2>/dev/null || stat -f '%u' -- "$build_root" 2>/dev/null || true)"
[[ -n "$owner_uid" && "$owner_uid" == "$(id -u)" ]] || fail "build root must be owned by the current user (uid $(id -u)): ${build_root}"

# These are protected by canonical path checks, so symlink aliases are rejected.
protected_roots=("$sdk_root" "$ndk_root" "$SOURCE_ROOT")
for desktop_root in \
    "${NATIVE_ROOT}/external/sdl2/build" \
    "${NATIVE_ROOT}/external/faudio/build" \
    "${NATIVE_ROOT}/external/sdl2/sdl/build" \
    "${NATIVE_ROOT}/external/faudio"; do
    if [[ -e "$desktop_root" ]]; then
        protected_roots+=("$(realpath -e -- "$desktop_root")")
    fi
done
for protected in "${protected_roots[@]}"; do
    if is_same_or_below "$build_root" "$protected" || is_same_or_above "$build_root" "$protected"; then
        fail "unsafe build root overlaps protected SDK, NDK, source, or desktop build path: ${build_root} <-> ${protected}"
    fi
done

# The isolated checkout is a separate Git worktree from the original developer
# checkout. Resolve from the canonical path to reject either repository, its
# desktop dependency build directories, and aliases into either worktree.
if git_top="$(git -C "$build_root" rev-parse --show-toplevel 2>/dev/null)"; then
    git_top="$(realpath -e -- "$git_top")"
    fail "build root is inside a Git worktree: ${build_root} (worktree ${git_top})"
fi

run_stamp="$(date -u +'%Y%m%dT%H%M%SZ')"
preflight_json="${build_root}/preflight-${run_stamp}-$$.json"
[[ ! -e "$preflight_json" && ! -L "$preflight_json" ]] || fail "refusing to overwrite existing preflight evidence: ${preflight_json}"

# All validation is complete before this point. Write one evidence file atomically
# without deleting or replacing any caller data.
tmp_json="${build_root}/.preflight.json.$$"
[[ ! -e "$tmp_json" && ! -L "$tmp_json" ]] || fail "temporary preflight path already exists: ${tmp_json}"
trap 'rm -f -- "$tmp_json"' EXIT
ANDROID_PREFLIGHT_SDK="$sdk_root" \
ANDROID_PREFLIGHT_NDK="$ndk_root" \
ANDROID_PREFLIGHT_NDK_REVISION="$ndk_revision" \
ANDROID_PREFLIGHT_HOST_TAG="$host_tag" \
ANDROID_PREFLIGHT_ABI="$abi" \
ANDROID_PREFLIGHT_CONFIGURATION="$configuration" \
ANDROID_PREFLIGHT_API="$EXPECTED_API" \
ANDROID_PREFLIGHT_COMPILE_SDK="$EXPECTED_COMPILE_SDK" \
ANDROID_PREFLIGHT_BUILD_ROOT="$build_root" \
ANDROID_PREFLIGHT_CMAKE_VERSION="$cmake_version" \
ANDROID_PREFLIGHT_NINJA_VERSION="$ninja_version" \
ANDROID_PREFLIGHT_JAVA_VERSION="$java_version_output" \
ANDROID_PREFLIGHT_JAVAC_VERSION="$javac_version_output" \
ANDROID_PREFLIGHT_JAR_VERSION="$jar_version_output" \
ANDROID_PREFLIGHT_READelf_VERSION="$readelf_version" \
ANDROID_PREFLIGHT_LLVM_READELF_VERSION="$llvm_readelf_version" \
ANDROID_PREFLIGHT_SOURCE_ROOT="$SOURCE_ROOT" \
ANDROID_PREFLIGHT_COMMAND="$0" \
ANDROID_PREFLIGHT_EVIDENCE_PATH="$preflight_json" \
python3 - "$tmp_json" "${original_args[@]}" <<'PY'
import json
import os
import sys
from datetime import datetime, timezone

payload = {
    "schemaVersion": 1,
    "result": "ok",
    "phase": "preflight",
    "recordedUtc": datetime.now(timezone.utc).isoformat(),
    "invocation": {"argv": [os.environ["ANDROID_PREFLIGHT_COMMAND"], *sys.argv[2:]], "cwd": os.getcwd()},
    "evidencePath": os.environ["ANDROID_PREFLIGHT_EVIDENCE_PATH"],
    "resolved": {
        "sdkRoot": os.environ["ANDROID_PREFLIGHT_SDK"],
        "ndkRoot": os.environ["ANDROID_PREFLIGHT_NDK"],
        "ndkRevision": os.environ["ANDROID_PREFLIGHT_NDK_REVISION"],
        "ndkHostTag": os.environ["ANDROID_PREFLIGHT_HOST_TAG"],
        "minApi": int(os.environ["ANDROID_PREFLIGHT_API"]),
        "compileSdk": int(os.environ["ANDROID_PREFLIGHT_COMPILE_SDK"]),
        "abi": os.environ["ANDROID_PREFLIGHT_ABI"],
        "configuration": os.environ["ANDROID_PREFLIGHT_CONFIGURATION"],
        "buildRoot": os.environ["ANDROID_PREFLIGHT_BUILD_ROOT"],
        "sourceRoot": os.environ["ANDROID_PREFLIGHT_SOURCE_ROOT"],
    },
    "tools": {
        "cmake": os.environ["ANDROID_PREFLIGHT_CMAKE_VERSION"],
        "ninja": os.environ["ANDROID_PREFLIGHT_NINJA_VERSION"],
        "java": os.environ["ANDROID_PREFLIGHT_JAVA_VERSION"],
        "javac": os.environ["ANDROID_PREFLIGHT_JAVAC_VERSION"],
        "jar": os.environ["ANDROID_PREFLIGHT_JAR_VERSION"],
        "readelf": os.environ["ANDROID_PREFLIGHT_READelf_VERSION"],
        "llvmReadelf": os.environ["ANDROID_PREFLIGHT_LLVM_READELF_VERSION"],
    },
}
with open(sys.argv[1], "x", encoding="utf-8") as output:
    json.dump(payload, output, indent=2, sort_keys=True)
    output.write("\n")
print(json.dumps(payload, indent=2, sort_keys=True))
PY
mv -- "$tmp_json" "$preflight_json"
trap - EXIT

if "$preflight_only"; then
    exit 0
fi

[[ -f "${ndk_root}/build/cmake/android.toolchain.cmake" ]] || fail "official NDK CMake toolchain is missing"
selected_abis=("${ALLOWED_ABIS[@]}")
[[ "$abi" == all ]] || selected_abis=("$abi")
for selected_abi in "${selected_abis[@]}"; do
    build_dir="${build_root}/build/${selected_abi}/${configuration}"
    artifact_dir="${build_root}/artifacts/${selected_abi}/${configuration}"
    # Descendant symlinks must not redirect writes into a protected worktree.
    # Existing directories are reusable; no directories or caller files are deleted.
    python3 - "$build_root" "$build_dir" "$artifact_dir" <<'PY'
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
for value in sys.argv[2:]:
    path = pathlib.Path(value)
    for candidate in [path, *path.parents]:
        if candidate == root:
            break
        if candidate.is_symlink():
            raise SystemExit(f"Refusing symlink in native output path: {candidate}")
    path.mkdir(parents=True, exist_ok=True)
    if path.resolve() != path:
        raise SystemExit(f"Native output path changed during validation: {path}")
build, artifacts = map(pathlib.Path, sys.argv[2:])
for path in [build / "lib", artifacts / "libSDL2.so", artifacts / "libc++_shared.so",
             artifacts / "build-manifest.json", build / "lib/libFAudio.a"]:
    if path.is_symlink():
        raise SystemExit(f"Refusing symlink in native output path: {path}")
PY
    cmake -S "$SCRIPT_DIR" -B "$build_dir" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${ndk_root}/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$selected_abi" -DANDROID_PLATFORM="android-${EXPECTED_API}" \
        -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE="$configuration" \
        -DMONOGAME_ARTIFACT_DIR="$artifact_dir" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.10
    cmake --build "$build_dir" --target SDL2 FAudio-static

    case "$selected_abi" in
        x86_64) ndk_triple=x86_64-linux-android ;;
        arm64-v8a) ndk_triple=aarch64-linux-android ;;
    esac
    cxx_runtime="${llvm_bin}/../sysroot/usr/lib/${ndk_triple}/libc++_shared.so"
    [[ -f "$cxx_runtime" ]] || fail "NDK shared C++ runtime is missing: ${cxx_runtime}"
    [[ ! -L "${artifact_dir}/libc++_shared.so" ]] || fail "shared C++ runtime output must not be a symlink"
    cp -- "$cxx_runtime" "${artifact_dir}/libc++_shared.so"

    python3 - "$preflight_json" "$build_dir" "$artifact_dir" "$selected_abi" "$llvm_bin" "$SCRIPT_DIR" "$cxx_runtime" <<'PY'
import hashlib
import json
import pathlib
import subprocess
import sys
from datetime import datetime, timezone

preflight_path, build_path, artifact_path, abi, llvm_path, script_path, runtime_path = sys.argv[1:]
build = pathlib.Path(build_path)
artifacts = pathlib.Path(artifact_path)
script = pathlib.Path(script_path)
preflight = json.loads(pathlib.Path(preflight_path).read_text())
paths = json.loads((build / "dependency-paths.json").read_text())

def command(*args):
    return subprocess.check_output(args, text=True).strip()

def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()

def source(repo):
    # Include tracked file contents, not only HEAD: source overlays and recursive
    # submodule files must change the fingerprint even before a commit is made.
    names = subprocess.check_output(["git", "-C", str(repo), "ls-files", "--recurse-submodules", "-z"])
    fingerprint = hashlib.sha256()
    for name in sorted(names.split(b"\0")):
        if not name:
            continue
        path = repo / name.decode()
        fingerprint.update(name + b"\0")
        fingerprint.update(path.read_bytes() if path.is_file() else b"<missing>")
        fingerprint.update(b"\0")
    return {"path": str(repo), "commit": command("git", "-C", str(repo), "rev-parse", "HEAD"),
            "trackedContentSha256": fingerprint.hexdigest(),
            "status": command("git", "-C", str(repo), "status", "--porcelain", "--untracked-files=no")}

libraries = {}
for name, path in {**paths, "cxxShared": str(artifacts / "libc++_shared.so")}.items():
    libraries[name] = {"path": path, "sha256": digest(path), "bytes": pathlib.Path(path).stat().st_size}

manifest = {
    "schemaVersion": 1, "phase": "dependencies", "result": "ok",
    "recordedUtc": datetime.now(timezone.utc).isoformat(),
    "abi": abi, "configuration": preflight["resolved"]["configuration"],
    "minApi": preflight["resolved"]["minApi"], "compileSdk": preflight["resolved"]["compileSdk"],
    "ndkRevision": preflight["resolved"]["ndkRevision"], "stl": "c++_shared",
    "buildDirectory": str(build), "artifactDirectory": str(artifacts),
    "preflightEvidence": preflight_path, "resolved": preflight["resolved"],
    "tools": {**preflight["tools"], "clang": command(str(pathlib.Path(llvm_path) / "clang"), "--version"),
              "llvmAr": command(str(pathlib.Path(llvm_path) / "llvm-ar"), "--version")},
    "sources": {"monoGame": source(script.parents[2]),
                "sdl2Wrapper": source(script.parent / "external/sdl2"),
                "sdl2": source(script.parent / "external/sdl2/sdl"),
                "faudio": source(script.parent / "external/faudio")},
    "buildInputs": {name: {"path": str(script / name), "sha256": digest(script / name)}
                    for name in ("CMakeLists.txt", "build.sh", "toolchain.json")},
    "libraries": libraries,
    "cxxSharedSource": {"path": str(pathlib.Path(runtime_path).resolve()), "sha256": digest(runtime_path)},
    "cmakeCache": {"path": str(build / "CMakeCache.txt"), "sha256": digest(build / "CMakeCache.txt")},
    "compileCommands": {"path": str(build / "compile_commands.json"), "sha256": digest(build / "compile_commands.json")},
}
destination = artifacts / "build-manifest.json"
if destination.is_symlink():
    raise SystemExit(f"Refusing manifest symlink: {destination}")
temporary = artifacts / ".build-manifest.json.tmp"
with temporary.open("x", encoding="utf-8") as output:
    json.dump(manifest, output, indent=2, sort_keys=True)
    output.write("\n")
temporary.replace(destination)
print(f"Dependency build manifest: {destination}")
PY
done
