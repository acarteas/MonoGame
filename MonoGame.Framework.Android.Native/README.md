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
This same import adds SDL's existing `sdl2.proguard` directly to the app's R8
configuration. The SDK can extract a binding AAR's keep file without registering
it as an R8 input; the explicit app import preserves every SDL companion class
even when trimming removes unused managed wrappers. No separate app keep rules
are required.
Rebuild native artifacts after source revision changes. Build Debug and Release
native configurations separately to match the consuming app, or explicitly set
`AndroidVulkanNativeConfiguration` to the desired existing configuration.

The binding exposes `SDLActivity` and its protected `GetLibraries()`,
`GetMainFunction()`, `GetMainSharedObject()`, and Android lifecycle overrides.
`AndroidNativeGameActivity` owns game startup and callback lifetime. The bundled
keep rules preserve all SDL Java classes and JNI members when
an application enables R8, including companion classes without managed bindings.
Do not also reference the legacy `MonoGame.Framework.Android` implementation.

Derive the app's Android Activity from `Microsoft.Xna.Framework.AndroidNativeGameActivity`
and implement `protected override Game CreateGame()`. Construct the `Game` and its
`GraphicsDeviceManager` inside that method. The host calls it after SDL's real
surface readiness check on SDL's game thread, then runs the game synchronously
and disposes it on that same thread. Do not create the game in `OnCreate` or wait
synchronously for the Android UI thread from game/lifecycle callbacks. The Native
framework's historical `Threading` queue is explicitly bound to the game thread.

The host loads `c++_shared`, `SDL2`, then `mgruntime` through SDL Java, uses
`SDL_main`, validates Android host ABI version 1, and registers after SDL JNI
initialization before Android can resume/start SDL's thread. Each process session
has an increasing nonzero `uint64` ID and supplies exactly one SDL argument,
`--mgp-android-session=<decimal ID>`. Both argument delivery and callback entry
verify the actual owning Java SDL thread. A second activity runs the real Android
Activity superclass lifecycle through nonvirtual JNI and finishes without
resetting the live SDL singleton. Stale activity lifecycle calls follow the same
Android superclass path.

`RequestGameExit()` marks cancellation and sends SDL quit. Destruction sends quit
before SDL joins its thread; the UI thread never calls Game.Run or Game.Dispose.
The delegate and GCHandle remain rooted until native Clear succeeds after thread
completion. A shutdown/clear failure retains ownership and roots, rejects another
session and emits a failure checkpoint. SDL's synchronous native messagebox is
suppressed with a diagnostic and result -1 because waiting for a UI dialog would
deadlock SDL's destruction join.

`GameLifecycle` exposes a snapshot, and `OnGameLifecycleChanged` receives each
checkpoint on the emitting thread. Snapshots include session ID, managed and
native UI/game thread IDs, stage, cancellation, callback/root state, result and
failure details. Every checkpoint is also JSON logged with tag `LAL.AndroidVK`.
Observer/formatting failures are contained, including the final Cdecl callback
boundary. These startup diagnostics do not report a surface generation; real
generation tracking belongs to the surface-lifecycle implementation.

The activity gates every SDL state-mutating instance entrypoint: library loading;
start/resume/pause/stop/destroy and their SDL thread helpers; focus, configuration,
low-memory, key/back and permission callbacks; orientation and system-UI updates;
and native messageboxes. Rejected/stale callbacks that require Android superclass
behavior use explicit `android.app.Activity` nonvirtual calls. SDL static JNI APIs
remain implementation details and must not be called by a rejected activity.
The bound SDL surface also admits surface, input and sensor callbacks only while
its activity owns SDL. Initial real callbacks during base `OnCreate` remain
admitted before managed registration. Late callbacks after native quit only stop
that old view's sensors; they never call native SDL or mutate a replacement
session. This guard preserves current-owner base callback order and does not
implement generation tracking or surface recovery.

Focused host tests exercise the actual platform-independent session and ownership
code, plus the Native framework's thread queue:

```sh
flock --close /tmp/loadedandlocked-keybindings-dotnet-v2.lock \
  dotnet run --project Tests.AndroidNativeHost/AndroidNativeHost.Tests.csproj \
  -m:1 -p:BuildInParallel=false
```

These tests do not replace Android JNI, Java peer-cache or rendering execution
proof on a device/emulator.
