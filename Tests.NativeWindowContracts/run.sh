#!/usr/bin/env bash
set -Eeuo pipefail

# Linux host contracts using actual Native sources with controlled SDL/MGP boundaries.
# The caller owns the external output directory; no Android device is exercised.
source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
output_root="$(realpath -e -- "${1:?Supply an existing external output directory}")"
[[ -d "$output_root" && -w "$output_root" && "$output_root" != "$source_root" && "$output_root" != "$source_root/"* ]]
cd "$source_root"

for android in 0 1; do
    "${CXX:-g++}" -std=c++17 -ffunction-sections -fdata-sections -DMG_VULKAN -DTEST_ANDROID="$android" \
        -I native/monogame/include -I native/monogame/external/sdl2/sdl/include \
        Tests.NativeWindowContracts/SdlWindowContracts.cpp -Wl,--gc-sections \
        -l:libSDL2-2.0.so.0 -o "$output_root/sdl-contract-$android"
    "$output_root/sdl-contract-$android"
done

for project in Tests.NativeWindowContracts Tests.NativeWindowContracts.Interop; do
    flock --close /tmp/loadedandlocked-keybindings-dotnet-v2.lock \
        dotnet build "Tests.NativeWindowContracts/$project.csproj" -m:1 -p:BuildInParallel=false \
        --artifacts-path "$output_root/managed-artifacts"
done
managed_bin="$output_root/managed-artifacts/bin"
flock --close /tmp/loadedandlocked-keybindings-dotnet-v2.lock dotnet "$managed_bin/Tests.NativeWindowContracts/debug/Tests.NativeWindowContracts.dll" android
flock --close /tmp/loadedandlocked-keybindings-dotnet-v2.lock dotnet "$managed_bin/Tests.NativeWindowContracts/debug/Tests.NativeWindowContracts.dll" desktop
flock --close /tmp/loadedandlocked-keybindings-dotnet-v2.lock dotnet "$managed_bin/Tests.NativeWindowContracts.Interop/debug/Tests.NativeWindowContracts.Interop.dll"
