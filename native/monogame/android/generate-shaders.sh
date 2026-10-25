#!/usr/bin/env bash
set -Eeuo pipefail

# This script owns the .NET lock. Callers must not hold it recursively.
source_root="$1"
host_root="$2"
configuration="$3"
mkdir -p -- "$host_root"
flock --close /tmp/loadedandlocked-keybindings-dotnet-v2.lock bash -s -- \
    "$source_root" "$host_root" "$configuration" <<'HOST'
set -Eeuo pipefail
source_root="$1"
host_root="$2"
configuration="$3"
# Explicit host TFM/RID properties prevent Android global properties leaking in.
dotnet build "$source_root/Tools/MonoGame.Effect.Compiler/MonoGame.Effect.Compiler.csproj" \
    --configuration "$configuration" -m:1 -p:BuildInParallel=false \
    -p:TargetFramework=net8.0 -p:RuntimeIdentifier= -p:RuntimeIdentifiers= \
    -p:UseArtifactsOutput=true -p:ArtifactsPath="$host_root/compiler" \
    -p:BaseOutputPath="$host_root/compiler/bin/" -p:OutputPath="$host_root/compiler/bin/$configuration/"
compiler="$host_root/compiler/bin/$configuration/mgfxc.dll"
shader_dir="$host_root/shaders"
mkdir -p -- "$shader_dir"
for effect in "$source_root"/MonoGame.Framework/Platform/Graphics/Effect/Resources/*.fx; do
    name="$(basename -- "${effect%.fx}")"
    dotnet "$compiler" "$effect" "$shader_dir/$name.vk.mgfxo.h" /Profile:Vulkan
done
python3 - "$source_root" "$shader_dir" "$compiler" <<'PY'
import hashlib
import json
import pathlib
import sys
root, output, compiler = map(pathlib.Path, sys.argv[1:])
def entry(path):
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
sources = root / "MonoGame.Framework/Platform/Graphics/Effect/Resources"
manifest = {"profile": "Vulkan", "compiler": entry(compiler),
            "compilerDependencies": [entry(path) for path in sorted(compiler.parent.rglob("*"))
                                     if path.is_file() and (path.suffix in {".dll", ".so", ".dylib", ".exe", ".json"}
                                                            or path.name == "dxc")],
            "sources": [entry(path) for path in sorted(sources.rglob("*")) if path.is_file()],
            "headers": [entry(path) for path in sorted(output.glob("*.vk.mgfxo.h"))]}
(output / "shader-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY
HOST
