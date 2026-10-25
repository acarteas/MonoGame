using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace Microsoft.Xna.Framework;

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
internal delegate int AndroidNativeRunCallback(IntPtr userData);

internal interface IAndroidNativeHostApi
{
    uint GetVersion();
    int Register(ulong sessionId, AndroidNativeRunCallback callback, IntPtr userData);
    int Clear(ulong sessionId);
}

internal interface IAndroidNativeGame : IDisposable
{
    void Run();
}

// This class deliberately has no Android dependency so its real callback/root
// lifetime can be exercised without an emulator or graphics device.
internal sealed class AndroidNativeRunSession
{
    internal const int Ok = 0;
    internal const int StaleSession = -2;
    internal const int Busy = -4;
    internal const int AlreadyRun = -6;
    internal const int CallbackException = -7;
    private readonly object _gate = new object();
    private readonly IAndroidNativeHostApi _api;
    private readonly Func<bool> _admit;
    private readonly Action _bindThread;
    private readonly Func<IAndroidNativeGame> _create;
    private readonly Func<int> _nativeThreadId;
    private readonly Action<AndroidNativeGameLifecycle> _observe;
    private readonly AndroidNativeGameLifecycle _state;
    private AndroidNativeRunCallback _callback;
    private GCHandle _root;
    private bool _registered;
    private bool _entered;
    private bool _acceptingExitSignals;
    private int _exitSignals;

    internal AndroidNativeRunSession(ulong id, IAndroidNativeHostApi api, Func<bool> admit,
        Action bindThread, Func<IAndroidNativeGame> create, Func<int> nativeThreadId,
        Action<AndroidNativeGameLifecycle> observe)
    {
        _api = api;
        _admit = admit;
        _bindThread = bindThread;
        _create = create;
        _nativeThreadId = nativeThreadId;
        _observe = observe;
        _state = new AndroidNativeGameLifecycle
        {
            SessionId = id,
            Stage = "created",
            UiThreadId = Environment.CurrentManagedThreadId,
            UiNativeThreadId = nativeThreadId()
        };
    }

    internal AndroidNativeGameLifecycle Snapshot
    {
        get
        {
            lock (_gate)
            {
                return CopyState();
            }
        }
    }

    internal void Register()
    {
        try
        {
            if (_api.GetVersion() != 2)
            {
                throw new InvalidOperationException("MonoGame Android host API version 2 is required.");
            }

            lock (_gate)
            {
                if (_root.IsAllocated || _entered)
                {
                    throw new InvalidOperationException("This game session has already been registered.");
                }

                _callback = Run;
                _root = GCHandle.Alloc(this);
                int status = _api.Register(_state.SessionId, _callback, GCHandle.ToIntPtr(_root));
                if (status != Ok)
                {
                    ReleaseRoots();
                    throw new InvalidOperationException("MGP_Android_RegisterRunCallback returned " + status + ".");
                }

                _registered = true;
            }

            Publish("registered");
        }
        catch (Exception error)
        {
            lock (_gate)
            {
                if (!_registered)
                {
                    ReleaseRoots();
                }
            }

            Fail("registration-failed", error);
            throw;
        }
    }

    // Lease native signals without holding host locks across SDL/JNI. Callback
    // completion closes admission and drains these calls before SDL_Quit starts.
    internal void RequestStop(Action signalExit = null)
    {
        bool signal;
        lock (_gate)
        {
            _state.CancellationRequested = true;
            signal = _acceptingExitSignals && signalExit != null;
            if (signal)
            {
                ++_exitSignals;
            }
        }

        try
        {
            if (signal)
            {
                signalExit();
            }
        }
        finally
        {
            if (signal)
            {
                lock (_gate)
                {
                    --_exitSignals;
                    Monitor.PulseAll(_gate);
                }
            }
        }

        Publish("exit-requested");
    }

    internal bool IsExitSignalAdmissionOpen
    {
        get
        {
            lock (_gate)
            {
                return _acceptingExitSignals;
            }
        }
    }

    private void CloseExitSignals()
    {
        lock (_gate)
        {
            _acceptingExitSignals = false;
            while (_exitSignals != 0)
            {
                try
                {
                    Monitor.Wait(_gate);
                }
                catch (ThreadInterruptedException)
                {
                    _state.Result = CallbackException;
                    _state.Failure ??= "Callback interrupted while draining an SDL exit signal.";
                }
            }
        }
    }

    // Call only after SDL shutdown/join. A BUSY/error result deliberately keeps
    // the delegate, GCHandle and activity alive rather than risking a dangling ABI.
    internal int Clear()
    {
        int status;
        lock (_gate)
        {
            if (!_registered)
            {
                return Ok;
            }

            status = _api.Clear(_state.SessionId);
            if (status == Ok)
            {
                _registered = false;
                ReleaseRoots();
            }
        }

        Publish(status == Ok ? "registration-cleared" : "registration-clear-failed");
        return status;
    }

    private int Run(IntPtr userData)
    {

        // This outermost Cdecl boundary includes error formatting, observers,
        // finally/disposal and the return snapshot. Even a broken exception's
        // ToString must not escape back into SDL's native call stack.
        try
        {
            return RunCore(userData);
        }
        catch (Exception)
        {
            try
            {
                CloseExitSignals();
                lock (_gate)
                {
                    _state.Result = CallbackException;
                    _state.Failure ??= "A secondary exception occurred while completing the managed callback.";
                    _state.IsCallbackActive = false;
                }
            }
            catch (Exception)
            {

                // Diagnostics are best effort at the final ABI boundary.
            }

            return CallbackException;
        }
    }

    private int RunCore(IntPtr userData)
    {
        IAndroidNativeGame game = null;
        bool entered = false;
        try
        {
            lock (_gate)
            {
                if (!_registered || !_root.IsAllocated || userData != GCHandle.ToIntPtr(_root))
                {
                    return StaleSession;
                }

                if (_entered)
                {
                    return AlreadyRun;
                }

                _entered = true;
                entered = true;
                _state.IsCallbackActive = true;
                _acceptingExitSignals = true;
                _state.GameThreadId = Environment.CurrentManagedThreadId;
                _state.GameNativeThreadId = _nativeThreadId();
            }

            if (!_admit() || Environment.CurrentManagedThreadId == _state.UiThreadId)
            {
                throw new InvalidOperationException("Callback is not on the owning SDL game thread with a ready surface.");
            }

            Publish("callback-entered");
            _bindThread();
            if (!Snapshot.CancellationRequested)
            {
                game = _create() ?? throw new InvalidOperationException("CreateGame returned null.");
                Publish("game-created");
                if (!Snapshot.CancellationRequested)
                {
                    Publish("game-running");
                    game.Run();
                    Publish("game-run-completed");
                }
            }
        }
        catch (Exception error)
        {
            Fail("callback-failed", error);
        }
        finally
        {
            if (entered)
            {
                try
                {
                    if (game != null)
                    {
                        game.Dispose();
                        Publish("game-disposed");
                    }
                }
                catch (Exception error)
                {
                    Fail("game-dispose-failed", error);
                }

                CloseExitSignals();
                lock (_gate)
                {
                    _state.Result ??= Ok;
                    _state.IsCallbackActive = false;
                }

                Publish("callback-completed");
            }
        }

        return Snapshot.Result ?? CallbackException;
    }

    internal void Fail(string stage, Exception error)
    {
        lock (_gate)
        {
            _state.Result = CallbackException;
            string description = Describe(error);
            _state.Failure = _state.Failure == null ? description : _state.Failure + "\n" + description;
        }

        Publish(stage);
    }

    private void Publish(string stage)
    {
        AndroidNativeGameLifecycle snapshot;
        lock (_gate)
        {
            _state.Stage = stage;
            snapshot = CopyState();
        }

        try
        {
            _observe(snapshot);
        }
        catch (Exception error)
        {
            lock (_gate)
            {
                _state.Result = CallbackException;
                _state.Failure ??= "Lifecycle observer failed: " + Describe(error);
            }
        }
    }

    private void ReleaseRoots()
    {
        if (_root.IsAllocated)
        {
            _root.Free();
        }

        _callback = null;
    }

    private static string Describe(Exception error)
    {
        try
        {
            return error.ToString();
        }
        catch (Exception)
        {
            return "Exception diagnostics unavailable: exception formatting threw.";
        }
    }

    private AndroidNativeGameLifecycle CopyState() => new AndroidNativeGameLifecycle
    {
        SessionId = _state.SessionId,
        Stage = _state.Stage,
        UiThreadId = _state.UiThreadId,
        GameThreadId = _state.GameThreadId,
        UiNativeThreadId = _state.UiNativeThreadId,
        GameNativeThreadId = _state.GameNativeThreadId,
        Result = _state.Result,
        Failure = _state.Failure,
        CancellationRequested = _state.CancellationRequested,
        IsCallbackActive = _state.IsCallbackActive,
        IsRegistrationRooted = _root.IsAllocated
    };
}
