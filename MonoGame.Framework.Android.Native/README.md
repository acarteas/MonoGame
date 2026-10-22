# MonoGame Native Android Java bridge

This project binds the complete `org.libsdl.app` Java source set from the pinned
SDL2 submodule. It builds against Android SDK 36 with minimum API 24 using .NET
10. Initialize the repository submodules before building. The Android SDK's
`AndroidJavaSource` binding pipeline compiles the Java sources, embeds the full
class archive, and generates the managed `Org.Libsdl.App` bindings in `obj`.
`MonoGame.Framework.Android.Native` references the generic
`MonoGame.Framework.Native` implementation.

Build the native runtime into an external directory first:

```sh
native/monogame/android/build.sh --build-root /absolute/native-build \
  --sdk-root "$ANDROID_HOME" --configuration Debug
```

An Android app references this project and selects a single RID per build:

```xml
<PropertyGroup>
  <TargetFramework>net10.0-android36.0</TargetFramework>
  <SupportedOSPlatformVersion>24.0</SupportedOSPlatformVersion>
  <RuntimeIdentifier>android-x64</RuntimeIdentifier>
  <AndroidVulkanNativeRoot>/absolute/native-build</AndroidVulkanNativeRoot>
</PropertyGroup>
<ItemGroup>
  <ProjectReference Include="../MonoGame/MonoGame.Framework.Android.Native/MonoGame.Framework.Android.Native.csproj" />
</ItemGroup>
<Import Project="../MonoGame/MonoGame.Framework.Android.Native/MonoGame.Framework.Android.Native.Source.targets" />
```

Use `android-arm64` for ARM64. Import the source targets in the app as shown: the Android SDK builds referenced
libraries without an app RID. The imported runtime targets validate the native
manifest against the current source checkout and package the selected ABI.
Rebuild native artifacts after source revision changes. Build Debug and Release
native configurations separately to match the consuming app, or explicitly set
`AndroidVulkanNativeConfiguration` to the desired existing configuration.

The binding exposes `SDLActivity` and its protected `GetLibraries()`,
`GetMainFunction()`, `GetMainSharedObject()`, and Android lifecycle overrides.
Game startup and managed callback lifetime belong to the higher-level activity
host. The bundled keep rules preserve all SDL Java classes and JNI members when
an application enables R8, including companion classes without managed bindings.
Do not also reference the legacy `MonoGame.Framework.Android` implementation.
