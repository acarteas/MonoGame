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

`RequestGameExit()` marks cancellation and leases an SDL quit signal only while
the callback accepts signals. SDL/JNI calls run outside host locks. Callback
completion closes signal admission and drains any in-flight signal before
returning. After the managed callback has run and disposed the game, the native
`SDL_main` host calls `SDL_Quit` on that same game thread before it becomes
completed. This retires the old event queue/subsystems; a later activity gets a
fresh SDL initialization without discarding that new session's cancellation.

Android Native input belongs to the managed platform instance as well as its
native handle. Disposal clears controller state/capabilities, pressed keyboard
state, and matching mouse/touch/messagebox window references before freeing the
native platform. The next session can reuse controller IDs and native addresses.
An incomplete native teardown keeps the old owner reserved and blocks replacement;
a stale managed owner cannot clear a replacement's input or graphics system.
Desktop Native input behavior is unchanged. Custom/static mouse cursor lifetime
across SDL shutdown is outside this host's current session-reuse coverage.

Destruction signals/cancels first and joins the SDL thread outside host locks,
then clears its matching Java thread field before SDLActivity performs JNI
teardown. This prevents Java from injecting a redundant quit during SDL cleanup.
The UI thread never calls Game.Run or Game.Dispose.
The delegate and GCHandle remain rooted until native Clear succeeds after thread
completion and successful Java teardown. A shutdown/clear failure retains ownership and roots, rejects another
session and emits a failure checkpoint. SDL's synchronous native messagebox is
suppressed with a diagnostic and result -1 because waiting for a UI dialog would
deadlock SDL's destruction join.

`GameLifecycle` exposes a snapshot, and `OnGameLifecycleChanged` receives each
checkpoint on the emitting thread. Snapshots include session ID, managed and
native UI/game thread IDs, stage, cancellation, callback/root state, result and
failure details. Every checkpoint is also JSON logged with tag `LAL.AndroidVK`.
Observer/formatting failures are contained, including the final Cdecl callback
boundary. `SurfaceLifecycle` and `OnSurfaceLifecycleChanged` separately expose
immutable real holder generation/readiness and application paused/exiting state.
Observers may run on the game or UI thread; `Observation` orders their snapshots.

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
session. The same surface path also checks active view identity, including old
views from the same Activity. Generation advances only on real `SurfaceCreated`;
`SurfaceChanged` publishes readiness only after SDL has accepted its dimensions.
The initial SDL thread publishes SDL's observed ready state before managed entry.

The native host ABI is version 2. Its 48-byte fixed-width lifecycle snapshot uses
64-bit session, generation, revision, retirement and acknowledgment values plus
32-bit flags/reserved (offsets 40/44). Native framework consumers bind the current
session on the owning SDL game thread once, then use its explicit ID for all later
queries and acknowledgments. Android lifecycle events append values 18..21 to the
existing event ABI; its packed payload remains at offset 12. C/C++ and managed
layout tests cover the 60-byte event and 48-byte snapshot.

Android sets `SDL_HINT_ANDROID_BLOCK_ON_PAUSE=0` before SDL video initialization.
The UI closes draw admission before invoking SDL's native surface-destroy path,
then waits at most 1500 ms for that exact session/revision's acknowledgment. The
game checkpoint abandons unsubmitted recording, quiesces GPU work, destroys the
swapchain and `VkSurfaceKHR`, and acknowledges without holding a host mutex across
SDL, JNI or Vulkan calls. The UI never asks the game to perform UI work. Failure
quarantines the session instead of releasing its native window or Java/JNI state;
quarantine survives callback completion and rejects native Clear/new owners.
Disposal failures publish quarantine before returning to native SDL_main, which
skips SDL_Quit and automatic acknowledgment for that unsafe session.

Recreation checks observed holder generation even if `SDL_Window*` is unchanged.
Acquisition uses a finite 10 ms timeout on Android; surface-lost/out-of-date and
suboptimal outcomes schedule one recreation attempt per draw checkpoint. Deferred
frames preserve managed dirty state; paused/unready frames do not acquire or
present. Desktop preparation, resize and present behavior retain their existing
paths. Deactivation neutralizes held keyboard/mouse/touch/controller state without
retiring controller identity or the owner-qualified session handle. Each consumer
remembers its last consumed retirement revision, so a pause/resume coalesced before
the next game tick still neutralizes held input. On that new revision Android SDL
polling drops queued held-input changes (including managed text fragments) before
forwarding input again; controller add/remove, window, lifecycle and unrelated
events are retained. Unchanged ready checkpoints preserve fresh input.

Debug alone exposes protected `Task<bool> RecreateSurfaceForDiagnostics()`. It
removes and reattaches the real SDL view on the UI thread and completes only after
real destruction and a newer ready holder callback, with a four-second failure
deadline. Each posted phase validates the captured session/view/request. Release
has no helper or exported automation endpoint.

Focused host tests exercise the actual platform-independent session and ownership
code, plus the Native framework's thread queue:

```sh
flock --close /tmp/loadedandlocked-keybindings-dotnet-v2.lock \
  dotnet run --project Tests.AndroidNativeHost/AndroidNativeHost.Tests.csproj \
  -m:1 -p:BuildInParallel=false
```

These tests do not replace Android JNI, Java peer-cache or rendering execution
proof on a device/emulator.

The actual compiled Native framework's Android input retirement tests use a small
native ABI shim. They cover repeated controller IDs and native addresses, stale
owners, teardown failure quarantine, constructor rollback and desktop behavior.
All generated files go into the required external output directory:

```sh
python3 Tests.NativeInputContracts/run-tests.py --output /absolute/external/native-input-tests
```

Native host tests compile the production entrypoint and a fresh static build of
the pinned SDL event queue (with device subsystems disabled) into the caller's
external build tree. They cover repeated sessions, late old quits, intact new
cancellation, cleanup thread/order, active clear and callback failure cleanup:

```sh
cmake -S native/monogame/android/tests -B /absolute/external/android-host-tests -G Ninja
cmake --build /absolute/external/android-host-tests
ctest --test-dir /absolute/external/android-host-tests --output-on-failure
```
