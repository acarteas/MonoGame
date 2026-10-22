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
}
