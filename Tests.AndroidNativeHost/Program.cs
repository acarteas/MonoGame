using System.Runtime.InteropServices;
using Microsoft.Xna.Framework;

var tests = new (string Name, Action Run)[]
{
    ("normal completion disposes on SDL thread before native clear and root release", NormalCompletion),
    ("active clear preserves delegate and GCHandle until callback returns", ActiveClear),
    ("SDL exit signal leases release host locks and drain before callback return", ExitSignalLease),
    ("destroy before entry cancels construction and stale callback", CancelBeforeEntry),
    ("exit during construction suppresses Run and disposes exactly once", CancelDuringCreation),
    ("Create/Run/Dispose exceptions become structured callback failure", FailurePaths),
    ("outer Cdecl boundary contains broken exception formatting and observers", BrokenDiagnostics),
    ("UI/wrong SDL admission never constructs a game", WrongThread),
    ("stale Activity release cannot steal the next monotonic session", StaleOwner),
    ("failed JNI teardown quarantines owner and callback roots", FailedShutdown),
    ("registration version/status failures release unowned roots", RegistrationFailure),
    ("surface generations, readiness and stale-view admission use observed callbacks", SurfaceState),
    ("fixed lifecycle and appended event ABI layouts", SurfaceAbi),
    ("Native Threading binds and drains its real queue on game thread", NativeThreadBinding)
};
foreach (var test in tests)
{
    test.Run();
    Console.WriteLine("PASS " + test.Name);
}

Console.WriteLine("PASS " + tests.Length + " Android Native host state tests");

static void SurfaceState()
{
    var state = new AndroidNativeSurfaceSession(71);
    object first = new(), second = new();
    state.Attach(first);
    Check(state.Destroying(first), "destroy before create was rejected");
    Check(state.Snapshot.SurfaceGeneration == 0 && !state.Changed(first, true), "destroy fabricated a generation/readiness");
    Check(state.Created(first) && !state.Created(first), "duplicate create advanced generation");
    Check(state.Changed(first, true) && state.Snapshot.SurfaceGeneration == 1, "actual first holder not ready");
    state.Pause(false);
    for (int cycle = 0; cycle < 4; cycle++)
    {
        state.Pause(true);
        Check(state.Snapshot.SurfacePaused && state.Snapshot.SurfaceReady, "pause fabricated surface destruction");
        state.Pause(false);
    }
    Check(state.Destroying(first) && !state.Snapshot.SurfaceReady, "destruction left admission open");
    state.Attach(second);
    Check(!state.IsCurrent(first) && !state.Created(first) && !state.Changed(first, true) && !state.Destroying(first), "same-owner old view admitted");
    Check(state.Created(second) && state.Changed(second, true) && state.Snapshot.SurfaceGeneration == 2, "replacement holder generation");
    Check(state.Destroying(second) && state.Created(second) && state.Snapshot.SurfaceGeneration == 3, "same view new holder generation");
    state.Exit(true);
    state.Pause(false);
    Check(!state.IsCurrent(second) && !state.Created(second) && !state.Changed(second, true), "exit admitted late callback");
    Check(state.Snapshot.Quarantined && state.Snapshot.SurfaceExiting && !state.Snapshot.SurfaceReady, "quarantine was reversible");
    var exhausted = new AndroidNativeSurfaceSession(72);
    exhausted.Attach(first);
    typeof(AndroidNativeSurfaceSession).GetField("_generation", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!
        .SetValue(exhausted, long.MaxValue);
    bool overflowRejected = false;
    try { exhausted.Created(first); }
    catch (OverflowException) { overflowRejected = true; }
    Check(overflowRejected && exhausted.Snapshot.SurfaceGeneration == long.MaxValue && !exhausted.Snapshot.SurfaceReady,
        "surface generation wrapped or became ready on overflow");
}

static void SurfaceAbi()
{
    Check(Marshal.SizeOf<MonoGame.Interop.MGP_AndroidLifecycle>() == 48, "native lifecycle size");
    Check(Marshal.OffsetOf<MonoGame.Interop.MGP_AndroidLifecycle>("Flags").ToInt32() == 40 &&
        Marshal.OffsetOf<MonoGame.Interop.MGP_AndroidLifecycle>("Reserved").ToInt32() == 44, "lifecycle fixed flags offsets");
    Check(Marshal.SizeOf<MonoGame.Interop.MGP_Event>() == 60 &&
        Marshal.OffsetOf<MonoGame.Interop.MGP_Event>("AndroidLifecycle").ToInt32() == 12, "event packed payload layout");
    Check((uint)MonoGame.Interop.EventType.DropComplete == 17 &&
        (uint)MonoGame.Interop.EventType.AndroidLifecycle == 18 &&
        (uint)MonoGame.Interop.EventType.AppTerminating == 21, "event values were not appended");
}

static void Check(bool condition, string message)
{
    if (!condition)
    {
        throw new Exception(message);
    }
}

static int OnGameThread(Func<int> action)
{
    int result = int.MinValue;
    Exception failure = null;
    var thread = new Thread(() =>
    {
        try { result = action(); }
        catch (Exception error) { failure = error; }
    });
    thread.Start();
    Check(thread.Join(TimeSpan.FromSeconds(5)), "game thread exceeded test deadline");
    if (failure != null)
    {
        throw new Exception("callback escaped", failure);
    }

    return result;
}

static AndroidNativeRunSession Session(FakeApi api, Func<IAndroidNativeGame> create,
    Func<bool> admit = null, Action<AndroidNativeGameLifecycle> observe = null)
    => new AndroidNativeRunSession(1, api, admit ?? (() => true), () => { }, create,
        () => Environment.CurrentManagedThreadId, observe ?? (_ => { }));

static void NormalCompletion()
{
    var api = new FakeApi();
    var game = new FakeGame();
    var stages = new List<string>();
    var session = Session(api, () => game, observe: state => stages.Add(state.Stage));
    session.Register();
    var callback = api.Callback;
    IntPtr data = api.Data;
    Check(GCHandle.FromIntPtr(data).Target == session, "GCHandle does not root the actual session");
    GC.Collect();
    Check(OnGameThread(api.Enter) == 0, "normal callback result");
    Check(game.Runs == 1 && game.Disposals == 1 && game.RunThread == game.DisposeThread, "game lifetime/thread");
    Check(session.Snapshot.GameThreadId != session.Snapshot.UiThreadId, "game ran on UI");
    Check(session.Snapshot.IsRegistrationRooted, "roots released before native clear");
    Check(stages.IndexOf("game-run-completed") < stages.IndexOf("game-disposed") &&
        stages.IndexOf("game-disposed") < stages.IndexOf("callback-completed"), "completion ordering");
    Check(OnGameThread(() => callback(data)) == -6, "duplicate callback admitted");
    Check(session.Clear() == 0 && !session.Snapshot.IsRegistrationRooted, "completed clear/root release");
    Check(session.Clear() == 0 && game.Disposals == 1, "double disposal/clear");
    Check(OnGameThread(() => callback(data)) == -2, "stale callback after root release");
}

static void ActiveClear()
{
    var api = new FakeApi();
    using var entered = new ManualResetEventSlim();
    using var finish = new ManualResetEventSlim();
    var game = new FakeGame { RunAction = () => { entered.Set(); Check(finish.Wait(TimeSpan.FromSeconds(5)), "finish timeout"); } };
    var session = Session(api, () => game);
    session.Register();
    int result = int.MinValue;
    var worker = new Thread(() => result = api.Enter());
    worker.Start();
    Check(entered.Wait(TimeSpan.FromSeconds(5)), "entry timeout");
    session.RequestStop();
    Check(session.Clear() == -4 && session.Snapshot.IsRegistrationRooted, "active clear freed roots");
    Check(game.Disposals == 0, "UI disposed active game");
    finish.Set();
    Check(worker.Join(TimeSpan.FromSeconds(5)), "completion timeout");
    Check(result == 0 && game.Disposals == 1, "active shutdown completion");
    Check(session.Clear() == 0 && !session.Snapshot.IsRegistrationRooted, "post-completion clear");
}

static void ExitSignalLease()
{
    var api = new FakeApi();
    using var running = new ManualResetEventSlim();
    using var finishRun = new ManualResetEventSlim();
    using var signalEntered = new ManualResetEventSlim();
    using var finishSignal = new ManualResetEventSlim();
    using var callbackReturned = new ManualResetEventSlim();
    var game = new FakeGame { RunAction = () => { running.Set(); Check(finishRun.Wait(TimeSpan.FromSeconds(5)), "run timeout"); } };
    var session = Session(api, () => game);
    session.Register();
    int signals = 0;
    session.RequestStop(() => ++signals);
    Check(signals == 0, "pre-entry cancellation sent an SDL signal");

    // Use a fresh session because the cancelled one correctly skips construction.
    Check(session.Clear() == 0, "pre-entry signal check clear");
    api = new FakeApi();
    session = Session(api, () => game);
    session.Register();
    int result = int.MinValue;
    var worker = new Thread(() => { result = api.Enter(); callbackReturned.Set(); });
    worker.Start();
    Check(running.Wait(TimeSpan.FromSeconds(5)), "callback did not start");
    var stopper = new Thread(() => session.RequestStop(() =>
    {
        Interlocked.Increment(ref signals);
        signalEntered.Set();
        Check(finishSignal.Wait(TimeSpan.FromSeconds(5)), "signal timeout");
    }));
    stopper.Start();
    Check(signalEntered.Wait(TimeSpan.FromSeconds(5)), "signal did not start");
    try
    {
        var snapshot = Task.Run(() => session.Snapshot);
        Check(snapshot.Wait(TimeSpan.FromSeconds(1)), "blocked SDL signal held the host state lock");
        Check(snapshot.Result.CancellationRequested && session.Clear() == -4, "state access blocked or lost active roots");
        finishRun.Set();
        Check(SpinWait.SpinUntil(() => !session.IsExitSignalAdmissionOpen, TimeSpan.FromSeconds(1)), "completion did not close signal admission");
        Check(!callbackReturned.IsSet && session.Snapshot.IsCallbackActive, "callback returned into SDL cleanup with signal in flight");
        session.RequestStop(() => Interlocked.Increment(ref signals));
        Check(signals == 1, "completion admitted another SDL quit");
    }
    finally
    {
        finishRun.Set();
        finishSignal.Set();
    }

    Check(stopper.Join(TimeSpan.FromSeconds(5)) && worker.Join(TimeSpan.FromSeconds(5)), "lease drain/join timeout");
    Check(result == 0 && game.Disposals == 1, "lease completion result/disposal");
    session.RequestStop(() => ++signals);
    Check(signals == 1, "post-completion Stop sent a late SDL quit");
    Check(session.Clear() == 0, "lease completion root clear");
}

static void CancelBeforeEntry()
{
    var api = new FakeApi();
    int constructions = 0;
    var session = Session(api, () => { constructions++; return new FakeGame(); });
    session.Register();
    session.RequestStop();
    Check(OnGameThread(api.Enter) == 0 && constructions == 0, "cancelled callback created game");
    Check(session.Clear() == 0, "cancelled clear");

    api = new FakeApi();
    session = Session(api, () => { constructions++; return new FakeGame(); });
    session.Register();
    var callback = api.Callback;
    IntPtr data = api.Data;
    session.RequestStop();
    Check(session.Clear() == 0, "pre-entry clear");
    Check(OnGameThread(() => callback(data)) == -2 && constructions == 0, "cleared callback admitted");
}

static void CancelDuringCreation()
{
    var api = new FakeApi();
    var game = new FakeGame();
    AndroidNativeRunSession session = null;
    session = Session(api, () => { session.RequestStop(); return game; });
    session.Register();
    Check(OnGameThread(api.Enter) == 0, "cancel-during-create result");
    Check(game.Runs == 0 && game.Disposals == 1, "constructed cancelled game not disposed exactly once");
    Check(session.Clear() == 0, "cancel-during-create clear");
}

static void FailurePaths()
{
    foreach (string stage in new[] { "create", "run", "dispose" })
    {
        var api = new FakeApi();
        var game = new FakeGame
        {
            RunAction = () => { if (stage == "run") { throw new InvalidOperationException("run failure"); } },
            DisposeAction = () => { if (stage == "dispose") { throw new InvalidOperationException("dispose failure"); } }
        };
        var session = Session(api, () =>
        {
            if (stage == "create") { throw new InvalidOperationException("create failure"); }
            return game;
        });
        session.Register();
        Check(OnGameThread(api.Enter) == -7, stage + " result");
        Check(session.Snapshot.Failure.Contains(stage + " failure"), stage + " diagnostic");
        Check(game.Disposals == (stage == "create" ? 0 : 1), stage + " disposal count");
        Check(session.Clear() == 0 && !session.Snapshot.IsRegistrationRooted, stage + " roots leaked");
    }
}

static void BrokenDiagnostics()
{
    foreach (bool observerFails in new[] { false, true })
    {
        var api = new FakeApi();
        var game = new FakeGame { RunAction = () => throw new UnprintableException() };
        var session = Session(api, () => game, observe: state =>
        {
            if (observerFails && state.Stage != "registered") { throw new UnprintableException(); }
        });
        session.Register();
        Check(OnGameThread(api.Enter) == -7, "secondary diagnostic exception escaped ABI");
        Check(session.Snapshot.Result == -7 && !session.Snapshot.IsCallbackActive, "secondary failure state");
        Check(game.Disposals == 1, "secondary fault disposal ordering");
        Check(session.Clear() == 0, "secondary fault roots leaked");
    }
}

static void WrongThread()
{
    foreach (bool wrongUi in new[] { true, false })
    {
        var api = new FakeApi();
        int constructions = 0;
        var session = Session(api, () => { constructions++; return new FakeGame(); }, admit: () => wrongUi);
        session.Register();
        int result = wrongUi ? api.Enter() : OnGameThread(api.Enter);
        Check(result == -7 && constructions == 0, "wrong UI/SDL admission constructed a game");
        Check(session.Clear() == 0, "wrong admission clear");
    }
}

static void StaleOwner()
{
    var owners = new AndroidNativeActivityOwner();
    var first = new object();
    var second = new object();
    Check(owners.TryAcquire(first, out ulong one) && one == 1, "first owner");
    Check(!owners.TryAcquire(second, out _) && !owners.Release(second), "concurrent owner mutated ownership");
    Check(owners.IsOwner(first) && owners.Release(first), "first release");
    Check(owners.TryAcquire(second, out ulong two) && two > one, "next monotonic session");
    Check(!owners.IsOwner(first) && !owners.Release(first) && owners.IsOwner(second), "stale lifecycle stole new owner");
    Check(owners.Release(second), "second release");
}

static void FailedShutdown()
{
    var owners = new AndroidNativeActivityOwner();
    var first = new object();
    var second = new object();
    Check(owners.TryAcquire(first, out _), "shutdown owner acquire");
    int clears = 0;
    int Clear() { clears++; return 0; }
    Check(!owners.TryCompleteShutdown(first, true, false, Clear), "failed JNI teardown released owner");
    Check(!owners.TryCompleteShutdown(first, false, true, Clear), "live thread released owner");
    Check(clears == 0 && !owners.TryAcquire(second, out _), "failed teardown cleared roots or admitted replacement");
    Check(!owners.TryCompleteShutdown(second, true, true, Clear) && clears == 0, "stale lifecycle called native clear");
    Check(!owners.TryCompleteShutdown(first, true, true, () => -4) && owners.IsOwner(first), "busy native clear released owner");
    Check(owners.TryCompleteShutdown(first, true, true, Clear) && clears == 1, "complete teardown did not clear/release");
    Check(owners.TryAcquire(second, out _), "next session blocked after complete teardown");
}

static void RegistrationFailure()
{
    foreach (bool version in new[] { true, false })
    {
        var api = new FakeApi { Version = version ? 9u : 2u, RegisterStatus = -4 };
        var session = Session(api, () => new FakeGame());
        bool failed = false;
        try { session.Register(); } catch (InvalidOperationException) { failed = true; }
        Check(failed && !session.Snapshot.IsRegistrationRooted && session.Snapshot.Result == -7, "failed register rooted session");
    }
}

static void NativeThreadBinding()
{
    Check(Threading.IsOnUIThread(), "initial Threading owner");
    int drainedThread = 0;
    Threading.OnUIThread(() => drainedThread = Environment.CurrentManagedThreadId);
    int gameThread = 0;
    Check(OnGameThread(() =>
    {
        AndroidNativeThreading.BindCurrentThread();
        gameThread = Environment.CurrentManagedThreadId;
        Threading.Run();
        return 0;
    }) == 0, "binding failed");
    Check(!Threading.IsOnUIThread() && drainedThread == gameThread, "Native queue did not follow bound SDL thread");
}

sealed class FakeApi : IAndroidNativeHostApi
{
    public uint Version = 2;
    public int RegisterStatus;
    private WeakReference<AndroidNativeRunCallback> _callback;
    private IntPtr _function;
    public AndroidNativeRunCallback Callback => _callback.TryGetTarget(out var callback)
        ? callback : throw new Exception("unmanaged callback delegate was collected");
    public IntPtr Data;
    private int _active;
    public uint GetVersion() => Version;
    public int Register(ulong id, AndroidNativeRunCallback callback, IntPtr data)
    {
        _callback = new WeakReference<AndroidNativeRunCallback>(callback);
        _function = Marshal.GetFunctionPointerForDelegate(callback);
        Data = data;
        return RegisterStatus;
    }
    public int Clear(ulong id) => Volatile.Read(ref _active) != 0 ? -4 : 0;
    public unsafe int Enter()
    {
        Interlocked.Exchange(ref _active, 1);
        try { return ((delegate* unmanaged[Cdecl]<IntPtr, int>)_function)(Data); }
        finally { Interlocked.Exchange(ref _active, 0); }
    }
}

sealed class FakeGame : IAndroidNativeGame
{
    public int Runs;
    public int Disposals;
    public int RunThread;
    public int DisposeThread;
    public Action RunAction;
    public Action DisposeAction;
    public void Run()
    {
        Runs++;
        RunThread = Environment.CurrentManagedThreadId;
        RunAction?.Invoke();
    }
    public void Dispose()
    {
        Disposals++;
        DisposeThread = Environment.CurrentManagedThreadId;
        DisposeAction?.Invoke();
    }
}

sealed class UnprintableException : Exception
{
    public override string ToString() => throw new InvalidOperationException("broken exception formatter");
}
