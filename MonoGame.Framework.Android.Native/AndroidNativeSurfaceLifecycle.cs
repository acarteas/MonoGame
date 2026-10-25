using System;

namespace Microsoft.Xna.Framework;

/// <summary>An immutable observation of this session's real Android surface callbacks.</summary>
public sealed class AndroidNativeSurfaceLifecycle
{
    public ulong SessionId { get; internal set; }
    public ulong Observation { get; internal set; }
    public long SurfaceGeneration { get; internal set; }
    public bool SurfaceReady { get; internal set; }
    public bool SurfacePaused { get; internal set; }
    public bool SurfaceExiting { get; internal set; }
    public bool Quarantined { get; internal set; }
}

// Android-free callback admission/state logic, linked directly into host tests.
// View identity supplements activity ownership: replacing a view in the same
// Activity never admits that old view's late input or SurfaceHolder callbacks.
internal sealed class AndroidNativeSurfaceSession
{
    private readonly object _gate = new object();
    private readonly ulong _id;
    private object _view;
    private long _generation;
    private bool _created;
    private bool _ready;
    private bool _paused = true;
    private bool _exiting;
    private bool _quarantined;

    internal AndroidNativeSurfaceSession(ulong id) => _id = id;

    internal bool IsCurrent(object view)
    {
        lock (_gate)
        {
            return view != null && ReferenceEquals(view, _view) && !_exiting && !_quarantined;
        }
    }

    internal void Attach(object view)
    {
        lock (_gate)
        {
            if (_exiting || _quarantined || _created)
            {
                throw new InvalidOperationException("A live or retired session cannot replace its surface view.");
            }
            _view = view ?? throw new ArgumentNullException(nameof(view));
        }
    }

    internal bool Created(object view)
    {
        lock (_gate)
        {
            if (!IsCurrent(view) || _created)
            {
                return false;
            }
            _generation = checked(_generation + 1);
            _created = true;
            _ready = false;
            return true;
        }
    }

    internal bool Changed(object view, bool ready)
    {
        lock (_gate)
        {
            if (!IsCurrent(view) || !_created)
            {
                return false;
            }
            _ready = ready;
            return true;
        }
    }

    internal bool Destroying(object view)
    {
        lock (_gate)
        {
            if (!ReferenceEquals(view, _view) || _quarantined)
            {
                return false;
            }
            _ready = false;
            _created = false;
            return true;
        }
    }

    internal void Pause(bool paused)
    {
        lock (_gate)
        {
            _paused = paused;
        }
    }

    internal void Exit(bool quarantine = false)
    {
        lock (_gate)
        {
            _exiting = true;
            _ready = false;
            _quarantined |= quarantine;
        }
    }

    internal AndroidNativeSurfaceLifecycle Snapshot
    {
        get
        {
            lock (_gate)
            {
                return new AndroidNativeSurfaceLifecycle
                {
                    SessionId = _id, SurfaceGeneration = _generation, SurfaceReady = _ready,
                    SurfacePaused = _paused, SurfaceExiting = _exiting, Quarantined = _quarantined
                };
            }
        }
    }
}
