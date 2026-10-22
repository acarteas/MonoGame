using System;

namespace Microsoft.Xna.Framework;

// SDL's Java host stores process-wide singleton/surface/thread fields. Keep
// ownership until both its thread and the native callback registration are gone.
internal sealed class AndroidNativeActivityOwner
{
    private readonly object _gate = new object();
    private object _owner;
    private ulong _lastId;

    internal bool TryAcquire(object owner, out ulong id)
    {
        lock (_gate)
        {
            id = 0;
            if (_owner != null || _lastId == ulong.MaxValue)
            {
                return false;
            }

            _owner = owner;
            id = ++_lastId;
            return true;
        }
    }

    internal bool IsOwner(object owner)
    {
        lock (_gate)
        {
            return ReferenceEquals(_owner, owner);
        }
    }

    internal bool Release(object owner)
    {
        lock (_gate)
        {
            if (!ReferenceEquals(_owner, owner))
            {
                return false;
            }

            _owner = null;
            return true;
        }
    }

    internal bool TryCompleteShutdown(object owner, bool threadStopped, bool teardownSucceeded, Func<int> clear)
    {
        if (!threadStopped || !teardownSucceeded || !IsOwner(owner))
        {
            return false;
        }

        // Native clear runs outside the ownership lock and only after both the
        // SDL thread and Java JNI teardown completed. A failure quarantines SDL.
        return clear() == AndroidNativeRunSession.Ok && Release(owner);
    }
}
