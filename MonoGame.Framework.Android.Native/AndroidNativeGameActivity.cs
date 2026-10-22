using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using Android.App;
using Android.Content.Res;
using Android.OS;
using Android.Runtime;
using Android.Views;
using Java.Interop;
using Org.Libsdl.App;

namespace Microsoft.Xna.Framework;

/// <summary>Runs a MonoGame Native game on SDL's Android game thread.</summary>
public abstract class AndroidNativeGameActivity : SDLActivity
{
    private static readonly AndroidNativeActivityOwner Owners = new AndroidNativeActivityOwner();
    private static readonly JniPeerMembers ActivityMembers = new JniPeerMembers("android/app/Activity", typeof(Activity));
    private AndroidNativeRunSession _session;
    private bool _sdlInitialized;
    private bool _startupSucceeded;
    private bool _destroying;

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
    private bool CanUseSdl => IsOwner && _startupSucceeded && !_destroying;

    protected override void OnCreate(Bundle savedInstanceState)
    {
        bool acquired = Owners.TryAcquire(this, out ulong sessionId);
        if (acquired)
        {
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

        return new AndroidNativeSdlSurface(context) { IsCurrentOwner = () => IsOwner && !_destroying };
    }

    protected sealed override string[] GetArguments()
    {

        // SDLMain reads mSingleton, so an old Java thread must never be given
        // a replacement activity's otherwise valid native registration token.
        if (!AdmitSdlThread())
        {
            return Array.Empty<string>();
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
            gameThread?.Join();
            stopped = gameThread == null || !gameThread.IsAlive;
            if (stopped && MSDLThread?.Equals(gameThread) == true)
            {
                MSDLThread = null;
            }

            base.OnDestroy();
            teardownSucceeded = true;
        }
        catch (Exception error)
        {
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
        if (CanUseSdl) { base.PauseNativeThread(); }
    }

    protected sealed override void ResumeNativeThread()
    {
        if (CanUseSdl) { base.ResumeNativeThread(); }
    }

    public override void OnWindowFocusChanged(bool hasFocus)
    {
        if (CanUseSdl) { base.OnWindowFocusChanged(hasFocus); }
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
