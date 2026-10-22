#!/usr/bin/env python3
"""Validate the runtime ABI, Android dependency closure and 16 KB ELF alignment."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys

artifacts, llvm, script, shaders = map(pathlib.Path, sys.argv[1:])
manifest_path = artifacts / "build-manifest.json"
manifest = json.loads(manifest_path.read_text())

def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True)

def entry(path):
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "bytes": path.stat().st_size}

readelf = llvm / "llvm-readelf"
runtime = artifacts / "libmgruntime.so"
exports = set()
for line in run(readelf, "--dyn-syms", "--wide", runtime).splitlines():
    fields = line.split()
    if len(fields) >= 8 and fields[4] == "GLOBAL" and fields[6] != "UND":
        exports.add(fields[7])
expected = set()
for header in (script.parent / "include").glob("api_*.h"):
    expected.update(re.findall(r"MG_EXPORT\s+[^;{]*?\b((?:MGP|MGG|MGA|MGI|MGM|MG_Asset)_\w+)\s*\(", header.read_text()))
# This existing desktop declaration has no definition in any Native backend.
# Preserve that ABI baseline; the managed synchronous SDL loop never calls it.
baseline_unimplemented = {"MGP_Platform_StartRunLoop"}
expected -= baseline_unimplemented
startup_exports = {"SDL_main", "MGP_Android_GetHostApiVersion",
                   "MGP_Android_RegisterRunCallback", "MGP_Android_ClearRunCallback",
                   "MGP_Android_PublishLifecycle", "MGP_Android_GetLifecycle",
                   "MGP_Android_AcknowledgeRetired", "MGP_Android_WaitRetired"}
expected |= startup_exports
missing = expected - exports
if missing:
    raise SystemExit(f"Missing Native API exports: {sorted(missing)}")

# NDK system libraries only; SDL2 and the one shared C++ runtime are packaged.
system = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libOpenSLES.so",
          "libGLESv1_CM.so", "libGLESv2.so"}
packaged = {"libSDL2.so", "libc++_shared.so", "libmgruntime.so"}
elf = {}
for name in sorted(packaged):
    path = artifacts / name
    dynamic = run(readelf, "--dynamic", "--wide", path)
    needed = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", dynamic)
    unsupported = set(needed) - system - packaged
    if unsupported:
        raise SystemExit(f"Unsupported dependencies in {name}: {sorted(unsupported)}")
    headers = run(readelf, "--program-headers", "--wide", path)
    alignments = [int(line.split()[-1], 16) for line in headers.splitlines() if line.strip().startswith("LOAD ")]
    if not alignments or any(value < 16384 for value in alignments):
        raise SystemExit(f"{name} does not meet 16 KB ELF alignment: {alignments}")
    header = run(readelf, "--file-header", path)
    machine = re.search(r"Machine:\s+(.+)", header).group(1).strip()
    expected_machine = {"x86_64": "Advanced Micro Devices X86-64", "arm64-v8a": "AArch64"}[manifest["abi"]]
    if machine != expected_machine:
        raise SystemExit(f"Unexpected ELF machine in {name}: {machine}")
    elf[name] = {"needed": needed, "loadSegmentAlignments": alignments, "machine": machine}

symbols = artifacts / "symbols"
symbols.mkdir(exist_ok=True)
debug_file = symbols / "libmgruntime.so.debug"
run(llvm / "llvm-objcopy", "--only-keep-debug", runtime, debug_file)
manifest["result"] = "ok"
manifest["elf"] = elf
manifest["exports"] = {"expected": sorted(expected), "verifiedCount": len(expected),
                       "androidStartup": sorted(startup_exports),
                       "baselineUnimplementedDeclarations": sorted(baseline_unimplemented - exports),
                       "allNativeApi": sorted(name for name in exports if name.startswith(("MGP_", "MGG_", "MGA_", "MGI_", "MGM_", "MG_Asset_")))}
manifest["androidHost"] = {
    "apiVersion": 2, "callingConvention": "Cdecl",
    "sessionArgument": "--mgp-android-session=",
    "sessionIds": "nonzero strictly increasing uint64 for loaded-library lifetime",
    "inputs": {name: entry(path) for name, path in {
        "implementation": script / "host.cpp",
        "header": script.parent / "include/api_MGP_Android.h"}.items()}}
manifest["symbols"] = entry(debug_file)
manifest["shaders"] = json.loads((shaders / "shader-manifest.json").read_text())
manifest["shaders"]["manifest"] = entry(shaders / "shader-manifest.json")
manifest["compileFlags"] = json.loads((pathlib.Path(manifest["buildDirectory"]) / "compile_commands.json").read_text())
manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
print(f"Verified {len(expected)} API exports, dependency closure and 16 KB alignment: {manifest_path}")
