#!/usr/bin/env python3
"""Compile the real Native framework and a small ABI shim; keep all output external."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", required=True, type=Path)
parser.add_argument("--props", type=Path, help="Optional external Directory.Build.props override")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
output = args.output.resolve()
if output.is_relative_to(root):
    parser.error("--output must be outside the checkout")
output.mkdir(parents=True, exist_ok=True)
props = args.props.resolve() if args.props else output / "ExternalBuild.props"
if not args.props:
    props.write_text('''<Project>
  <Import Project="$(MSBuildProjectDirectory)/Directory.Build.props" Condition="Exists('$(MSBuildProjectDirectory)/Directory.Build.props')" />
  <PropertyGroup>
    <BaseIntermediateOutputPath>$(ArtifactsPath)/obj/$(MSBuildProjectName)/</BaseIntermediateOutputPath>
    <BaseOutputPath>$(ArtifactsPath)/bin/$(MSBuildProjectName)/</BaseOutputPath>
  </PropertyGroup>
</Project>
''')

def run(command):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(command, cwd=root, check=True)

run(["flock", "--close", "/tmp/loadedandlocked-keybindings-dotnet-v2.lock", "dotnet", "build",
     root / "Tests.NativeInputContracts/NativeInputContracts.csproj",
     f"-p:DirectoryBuildPropsPath={props}", f"-p:ArtifactsPath={output / 'managed'}",
     "-m:1", "-p:BuildInParallel=false"])
for variant, defines, modes in [
    ("normal", [], ["android", "desktop"]),
    ("construct", ["-DFAIL_CONSTRUCT"], ["constructor"]),
    ("destroy", ["-DFAIL_DESTROY"], ["destroy"]),
    ("both", ["-DFAIL_CONSTRUCT", "-DFAIL_DESTROY"], ["constructor-destroy"]),
]:
    shim = output / f"libinput-{variant}.so"
    run(["c++", "-std=c++17", "-shared", "-fPIC", *defines, "-I", root / "native/monogame/include",
         root / "Tests.NativeInputContracts/NativeInputShim.cpp", "-o", shim])
    for mode in modes:
        run(["dotnet", output / "managed/bin/NativeInputContracts/debug/MonoGame.Framework.Android.Native.dll", shim, mode])
