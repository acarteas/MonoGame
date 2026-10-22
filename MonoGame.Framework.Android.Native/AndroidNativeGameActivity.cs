using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Threading.Tasks;
using Android.App;
using Android.Content.Res;
using Android.OS;
using Android.Runtime;
using Android.Views;
using Java.Interop;
using Org.Libsdl.App;
using MonoGame.Interop;

namespace Microsoft.Xna.Framework;

/// <summary>Runs a MonoGame Native game on SDL's Android game thread.</summary>
public abstract class AndroidNativeGameActivity : SDLActivity
{
    private static readonly AndroidNativeActivityOwner Owners = new AndroidNativeActivityOwner();
    private static readonly JniPeerMembers ActivityMembers = new JniPeerMembers("android/app/Activity", typeof(Activity));
    private AndroidNativeRunSession _session;
    private bool _sdlInitialized;
    private volatile bool _startupSucceeded;
    private volatile bool _destroying;
    private volatile bool _quarantined;
    private readonly object _surfacePublishGate = new object();
    private ulong _surfaceObservation;
    private bool _resumed;
    private bool _focused = true;
    private AndroidNativeSurfaceSession _surfaces;
    private AndroidNativeSdlSurface _surface;
#if DEBUG
    private TaskCompletionSource<bool> _recreation;
    private long _recreationGeneration;
#endif

    /// <summary>Construct the game here. This method runs on the SDL game thread.</summary>
    protected abstract Game CreateGame();

    /// <summary>The latest immutable copy of this activity's host diagnostics.</summary>
    protected AndroidNativeGameLifecycle GameLifecycle => _session?.Snapshot;

    /// <summary>
    /// Receives startup, completion and failure checkpoints on the emitting thread.
    /// Implementations must not synchronously wait for the Android UI thread.
    /// </summary>
    protected virtual void OnGameLifecycleChanged(AndroidNativeGameLifecycle lifecycle) { }

    private bool IsOwner => Owners.IsOwner(this);
    private bool CanUseSdl => IsOwner && _startupSucceeded && !_destroying && !_quarantined;

    /// <summary>The latest real SurfaceHolder observation; no generation is inferred from resize.</summary>
    protected AndroidNativeSurfaceLifecycle SurfaceLifecycle => _surfaces?.Snapshot;

    /// <summary>Receives immutable observations on the callback's emitting thread.</summary>
    protected virtual void OnSurfaceLifecycleChanged(AndroidNativeSurfaceLifecycle lifecycle) { }

    protected override void OnCreate(Bundle savedInstanceState)
    {
        bool acquired = Owners.TryAcquire(this, out ulong sessionId);
        if (acquired)
        {
            _surfaces = new AndroidNativeSurfaceSession(sessionId);
            _session = new AndroidNativeRunSession(sessionId, new NativeHostApi(),
                AdmitCallback, AndroidNativeThreading.BindCurrentThread,
                () => new GameLifetime(CreateGame()), global::Android.OS.Process.MyTid, Observe);
        }

        if (!acquired)
        {

            // Android still requires Activity.onCreate, but SDLActivity.onCreate
            // would reset the active owner's process-wide JNI/surface/thread state.
            ActivityVoid("onCreate", "(Landroid/os/Bundle;)V", new JniArgumentValue(savedInstanceState?.Handle ?? IntPtr.Zero));
            GC.KeepAlive(savedInstanceState);
            global::Android.Util.Log.Error("LAL.AndroidVK", "{\"stage\":\"activity-rejected\",\"reason\":\"another-session-owns-SDL-or-session-ids-exhausted\"}");
            Finish();
            return;
        }

        try
        {
            base.OnCreate(savedInstanceState);
            if (MBrokenLibraries)
            {
                throw new InvalidOperationException("SDL failed to load or validate the Android native libraries; see SDL log.");
            }

            _sdlInitialized = true;

            // SDL.setupJNI and SDL.initialize have completed in base.OnCreate.
            // onStart/onResume cannot run on this UI thread until this returns.
            _session.Register();
            _startupSucceeded = true;
            PublishSurface();
        }
        catch (Exception error)
        {
            _session.Fail("activity-initialization-failed", error);
            _session.RequestStop();
            Finish();
        }
    }

    protected sealed override string[] GetLibraries() => new[] { "c++_shared", "SDL2", "mgruntime" };
    protected sealed override string GetMainFunction() => "SDL_main";

    protected override SDLSurface CreateSDLSurface(global::Android.Content.Context context)
    {
        if (!IsOwner || _destroying)
        {
            return null;
        }

        var surface = new AndroidNativeSdlSurface(context);
        _surfaces.Attach(surface);
        _surface = surface;
        surface.IsCurrentOwner = () => IsOwner && !_destroying && !_quarantined && _surfaces.IsCurrent(surface);
        surface.Created = () =>
        {
            if (!_surfaces.Created(surface))
            {
                return false;
            }
            PublishSurface();
            return true;
        };
        surface.Changed = ready =>
        {
            if (_surfaces.Changed(surface, ready))
            {
                PublishSurface();
#if DEBUG
                if (ready && _recreation != null && _surfaces.Snapshot.SurfaceGeneration > _recreationGeneration)
                {
                    CompleteRecreation(true);
                }
#endif
            }
        };
        surface.Destroying = () => RetireSurface(surface);
        surface.Destroyed = () =>
        {
#if DEBUG
            if (_recreation != null)
            {

                // Post after the real destroy callback has returned. No synchronous
                // game-thread/UI rendezvous is needed to attach the view again.
                var request = _recreation;
                ulong session = _session.Snapshot.SessionId;
                MLayout.Post(() => ReattachSurface(surface, request, session));
            }
#endif
        };
        return surface;
    }

    protected sealed override string[] GetArguments()
    {

        // SDLMain reads mSingleton, so an old Java thread must never be given
        // a replacement activity's otherwise valid native registration token.
        if (!AdmitSdlThread())
        {
            return Array.Empty<string>();
        }

        if (_surface?.MIsSurfaceReady == true && _surfaces.Changed(_surface, true))
        {
            PublishSurface();
        }
        return new[] { "--mgp-android-session=" + _session.Snapshot.SessionId.ToString(CultureInfo.InvariantCulture) };
    }

    public sealed override void LoadLibraries()
    {
        if (IsOwner && !_destroying)
        {
            base.LoadLibraries();
        }
    }

    private bool AdmitSdlThread()
    {
        var current = Java.Lang.Thread.CurrentThread();
        var expected = MSDLThread;
        bool admitted = CanUseSdl && expected != null && current.Equals(expected) && MSingleton?.Equals(this) == true;
        GC.KeepAlive(current);
        GC.KeepAlive(expected);
        return admitted;
    }

    private bool AdmitCallback() => AdmitSdlThread() && MSurface?.MIsSurfaceReady == true;

    /// <summary>Requests an orderly exit without running Game lifecycle code on the UI thread.</summary>
    protected void RequestGameExit()
    {
        if (!IsOwner || _destroying)
        {
            return;
        }

        _surfaces.Exit();
        PublishSurface();
        _session.RequestStop(_sdlInitialized ? NativeSendQuit : null);
    }

    protected override void OnDestroy()
    {
        if (!IsOwner || _destroying)
        {
            ActivityVoid("onDestroy", "()V");
            return;
        }

        _destroying = true;
        _surfaces.Exit();
        PublishSurface();
#if DEBUG
        CompleteRecreation(false);
#endif
        ObserveNativeQuarantine();
        if (_quarantined)
        {
            _session.RequestStop(_sdlInitialized ? NativeSendQuit : null);
            ActivityVoid("onDestroy", "()V");
            return;
        }
        Java.Lang.Thread gameThread = null;
        bool stopped = false;
        bool teardownSucceeded = false;
        try
        {
            if (_sdlInitialized)
            {
                gameThread = MSDLThread;
                MSurface?.HandlePause();
            }

            _session.RequestStop(_sdlInitialized ? NativeSendQuit : null);

            // A quit signal cannot race the callback's inactive transition into
            // native SDL_Quit. Join outside all host locks before Java nativeQuit
            // destroys SDL's Android JNI state; base must not send another quit.
            gameThread?.Join(2000);
            if (gameThread?.IsAlive == true)
            {
                Quarantine("activity-join-timeout");
                ActivityVoid("onDestroy", "()V");
                return;
            }
            stopped = gameThread == null || !gameThread.IsAlive;
            ObserveNativeQuarantine();
            if (_quarantined)
            {
                ActivityVoid("onDestroy", "()V");
                return;
            }
            if (stopped && MSDLThread?.Equals(gameThread) == true)
            {
                MSDLThread = null;
            }

            base.OnDestroy();
            teardownSucceeded = true;
        }
        catch (Exception error)
        {
            Quarantine("activity-shutdown-failed");
            _session.Fail("activity-shutdown-failed", error);
            try
            {
                _session.RequestStop(_sdlInitialized ? NativeSendQuit : null);
            }
            catch (Exception quitError)
            {
                _session.Fail("activity-quit-failed", quitError);
            }

            ActivityVoid("onDestroy", "()V");
        }

        try
        {
            if (!Owners.TryCompleteShutdown(this, stopped, teardownSucceeded, _session.Clear))
            {
                _session.Fail("activity-shutdown-retained", new InvalidOperationException(
                    "SDL thread, JNI teardown or registration remains owned; callback roots retained and new sessions rejected."));
            }
        }
        catch (Exception error)
        {
            _session.Fail("registration-clear-failed", error);
        }

        GC.KeepAlive(gameThread);
    }

    protected override void OnStart()
    {
        if (CanUseSdl) { base.OnStart(); } else { ActivityVoid("onStart", "()V"); }
    }

    protected override void OnResume()
    {
        if (CanUseSdl) { base.OnResume(); } else { ActivityVoid("onResume", "()V"); }
    }

    protected override void OnPause()
    {
        if (CanUseSdl) { base.OnPause(); } else { ActivityVoid("onPause", "()V"); }
    }

    protected override void OnStop()
    {
        if (CanUseSdl) { base.OnStop(); } else { ActivityVoid("onStop", "()V"); }
    }

    protected sealed override void PauseNativeThread()
    {
        if (CanUseSdl)
        {
            _resumed = false;
            _surfaces.Pause(true);
            PublishSurface();
            base.PauseNativeThread();
        }
    }

    protected sealed override void ResumeNativeThread()
    {
        if (CanUseSdl)
        {
            _resumed = true;
            _surfaces.Pause(!_focused);
            PublishSurface();
            base.ResumeNativeThread();
        }
    }

    public override void OnWindowFocusChanged(bool hasFocus)
    {
        if (CanUseSdl)
        {
            _focused = hasFocus;
            _surfaces.Pause(!_resumed || !hasFocus);
            PublishSurface();
            base.OnWindowFocusChanged(hasFocus);
        }
        else { ActivityVoid("onWindowFocusChanged", "(Z)V", new JniArgumentValue(hasFocus)); }
    }

    public override void OnLowMemory()
    {
        if (CanUseSdl) { base.OnLowMemory(); } else { ActivityVoid("onLowMemory", "()V"); }
    }

    public override void OnConfigurationChanged(Configuration newConfig)
    {
        if (CanUseSdl) { base.OnConfigurationChanged(newConfig); }
        else { ActivityVoid("onConfigurationChanged", "(Landroid/content/res/Configuration;)V", new JniArgumentValue(newConfig.Handle)); }
        GC.KeepAlive(newConfig);
    }

    public override void OnBackPressed()
    {
        if (CanUseSdl) { base.OnBackPressed(); } else { ActivityVoid("onBackPressed", "()V"); }
    }

    public sealed override void PressBackButton()
    {
        if (CanUseSdl) { base.PressBackButton(); }
    }

    public sealed override void SuperOnBackPressed()
    {
        ActivityVoid("onBackPressed", "()V");
    }

    public override unsafe bool DispatchKeyEvent(KeyEvent e)
    {
        if (CanUseSdl)
        {
            return base.DispatchKeyEvent(e);
        }

        var type = ActivityMembers.JniPeerType.PeerReference;
        var method = JniEnvironment.InstanceMethods.GetMethodID(type, "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z");
        JniArgumentValue argument = new JniArgumentValue(e.Handle);
        bool result = JniEnvironment.InstanceMethods.CallNonvirtualBooleanMethod(PeerReference, type, method, &argument);
        GC.KeepAlive(e);
        return result;
    }

    public sealed override void OnSystemUiVisibilityChange(StatusBarVisibility visibility)
    {
        if (CanUseSdl) { base.OnSystemUiVisibilityChange(visibility); }
    }

    public override void OnRequestPermissionsResult(int requestCode, string[] permissions, global::Android.Content.PM.Permission[] grantResults)
    {
        if (CanUseSdl)
        {
            base.OnRequestPermissionsResult(requestCode, permissions, grantResults);
            return;
        }

        // SDL's implementation does not call Activity's superclass, but rejected
        // peers still deliver the real Android callback without nativePermissionResult.
        IntPtr permissionArray = JNIEnv.NewArray(permissions);
        IntPtr resultArray = JNIEnv.NewArray(Array.ConvertAll(grantResults, value => (int)value));
        try
        {
            ActivityVoid("onRequestPermissionsResult", "(I[Ljava/lang/String;[I)V",
                new JniArgumentValue(requestCode), new JniArgumentValue(permissionArray), new JniArgumentValue(resultArray));
        }
        finally
        {
            JNIEnv.DeleteLocalRef(permissionArray);
            JNIEnv.DeleteLocalRef(resultArray);
        }
    }

    public sealed override void SetOrientationBis(int width, int height, bool resizable, string hint)
    {
        if (CanUseSdl) { base.SetOrientationBis(width, height, resizable, hint); }
    }

    // SDL's synchronous messagebox blocks the game thread until UI completion,
    // which would deadlock Android's SDL-thread join during destruction.
    public sealed override int MessageboxShowMessageBox(int flags, string title, string message,
        int[] buttonFlags, int[] buttonIds, string[] buttonTexts, int[] colors)
    {
        global::Android.Util.Log.Warn("LAL.AndroidVK", "SDL messagebox suppressed: " + title + ": " + message);
        return -1;
    }

    protected sealed override void MessageboxCreateAndShow(Bundle args) { }

    private ulong PublishSurface()
    {
        AndroidNativeSurfaceLifecycle state;
        ulong revision = 0;
        lock (_surfacePublishGate)
        {

            // Capture inside the publication gate: a game-thread startup readiness
            // publication must not overwrite a newer UI destruction observation.
            state = _surfaces?.Snapshot;
            if (state == null)
            {
                return 0;
            }
            state.Observation = checked(++_surfaceObservation);
            if (_startupSucceeded)
            {
                uint flags = (state.SurfaceReady ? 1u : 0u) | (state.SurfacePaused ? 2u : 0u) |
                    (state.SurfaceExiting ? 4u : 0u) | (state.Quarantined ? 8u : 0u);
                int result = MGP_Android_PublishLifecycle(state.SessionId, (ulong)state.SurfaceGeneration, flags, out revision);
                if (result != 0)
                {
                    throw new InvalidOperationException("Lifecycle publication rejected: " + result);
                }
            }
        }
        OnSurfaceLifecycleChanged(state);
        return revision;
    }

    private bool RetireSurface(AndroidNativeSdlSurface surface)
    {
        if (!IsOwner || _quarantined || !_surfaces.Destroying(surface))
        {
            return false;
        }
        ulong revision = PublishSurface();
        if (_startupSucceeded && MGP_Android_WaitRetired(_session.Snapshot.SessionId, revision, 1500) != 0)
        {

            // Never invoke base: SDL would release ANativeWindow while the game
            // may still own GPU work. Retain this owner and all native/Java roots.
            Quarantine("surface-retirement-timeout");
            return false;
        }
        ObserveNativeQuarantine();
        return !_quarantined;
    }

    private void ObserveNativeQuarantine()
    {
        if (_startupSucceeded && !_quarantined &&
            MGP_Android_GetLifecycle(_session.Snapshot.SessionId, out var state) == 0 && (state.Flags & 8) != 0)
        {
            Quarantine("native-surface-quarantined");
        }
    }

    private void Quarantine(string reason)
    {
        _quarantined = true;
        _surfaces.Exit(true);
        PublishSurface();
#if DEBUG
        RunOnUiThread(() => CompleteRecreation(false));
#endif
        _session.Fail(reason, new InvalidOperationException("Android Native session quarantined; SDL window/JNI ownership retained."));
        _session.RequestStop(_sdlInitialized ? NativeSendQuit : null);
    }

#if DEBUG

    /// <summary>Debug diagnostics only: remove/reattach the real SDL SurfaceView.</summary>
    protected Task<bool> RecreateSurfaceForDiagnostics()
    {
        var completion = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        ulong session = _session?.Snapshot.SessionId ?? 0;
        RunOnUiThread(() =>
        {
            var surface = _surface;
            if (!CanUseSdl || session == 0 || _session.Snapshot.SessionId != session ||
                surface == null || _recreation != null || !_surfaces.IsCurrent(surface) ||
                !_surfaces.Snapshot.SurfaceReady || surface.Parent is not ViewGroup parent)
            {
                completion.TrySetResult(false);
                return;
            }
            _recreation = completion;
            _recreationGeneration = _surfaces.Snapshot.SurfaceGeneration;
            parent.RemoveView(surface);
            _ = ExpireRecreation(completion);
        });
        return completion.Task;
    }

    private async Task ExpireRecreation(TaskCompletionSource<bool> request)
    {
        await Task.Delay(4000).ConfigureAwait(false);
        RunOnUiThread(() =>
        {
            if (ReferenceEquals(_recreation, request))
            {
                CompleteRecreation(false);
            }
        });
    }

    private void ReattachSurface(AndroidNativeSdlSurface surface, TaskCompletionSource<bool> request, ulong session)
    {
        if (request == null || !ReferenceEquals(_recreation, request))
        {
            request?.TrySetResult(false);
            return;
        }
        if (!CanUseSdl || _session.Snapshot.SessionId != session || !_surfaces.IsCurrent(surface) || surface.Parent != null)
        {
            CompleteRecreation(false);
            return;
        }
        MLayout.AddView(surface);
    }

    private void CompleteRecreation(bool success)
    {
        var completion = _recreation;
        _recreation = null;
        completion?.TrySetResult(success);
    }
#endif

    [DllImport("mgruntime", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern int MGP_Android_PublishLifecycle(ulong sessionId, ulong generation, uint flags, out ulong revision);

    [DllImport("mgruntime", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern int MGP_Android_WaitRetired(ulong sessionId, ulong revision, uint timeoutMilliseconds);

    [DllImport("mgruntime", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
    private static extern int MGP_Android_GetLifecycle(ulong sessionId, out MGP_AndroidLifecycle snapshot);

    private unsafe void ActivityVoid(string name, string signature, params JniArgumentValue[] arguments)
    {
        var type = ActivityMembers.JniPeerType.PeerReference;
        var method = JniEnvironment.InstanceMethods.GetMethodID(type, name, signature);
        fixed (JniArgumentValue* values = arguments)
        {
            JniEnvironment.InstanceMethods.CallNonvirtualVoidMethod(PeerReference, type, method, values);
        }

        GC.KeepAlive(this);
    }

    private void Observe(AndroidNativeGameLifecycle lifecycle)
    {
        if (lifecycle.Stage == "game-dispose-failed" && !_quarantined)
        {
            Quarantine("game-disposal-quarantined");
        }
        global::Android.Util.Log.Info("LAL.AndroidVK", JsonSerializer.Serialize(lifecycle,
            AndroidNativeLifecycleJsonContext.Default.AndroidNativeGameLifecycle));
        OnGameLifecycleChanged(lifecycle);
    }

    private sealed class GameLifetime : IAndroidNativeGame
    {
        private readonly Game _game;
        internal GameLifetime(Game game) => _game = game ?? throw new InvalidOperationException("CreateGame returned null.");
        public void Run() => _game.Run(GameRunBehavior.Synchronous);
        public void Dispose() => _game.Dispose();
    }

    private sealed class NativeHostApi : IAndroidNativeHostApi
    {
        public uint GetVersion() => MGP_Android_GetHostApiVersion();
        public int Register(ulong id, AndroidNativeRunCallback callback, IntPtr data) => MGP_Android_RegisterRunCallback(id, callback, data);
        public int Clear(ulong id) => MGP_Android_ClearRunCallback(id);

        [DllImport("mgruntime", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern uint MGP_Android_GetHostApiVersion();
        [DllImport("mgruntime", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int MGP_Android_RegisterRunCallback(ulong sessionId, AndroidNativeRunCallback callback, IntPtr userData);
        [DllImport("mgruntime", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int MGP_Android_ClearRunCallback(ulong sessionId);
    }
}
