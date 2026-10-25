using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("MonoGame.Framework.Android.Native")]

namespace Microsoft.Xna.Framework;

internal static class AndroidNativeThreading
{

    // Native graphics use Threading's historical "UI" queue on the SDL game
    // thread. Bind before Game construction even if the type was touched earlier.
    internal static void BindCurrentThread()
    {
        Threading.BindNativeGameThread();
        Threading.EnsureUIThread();
    }
}
