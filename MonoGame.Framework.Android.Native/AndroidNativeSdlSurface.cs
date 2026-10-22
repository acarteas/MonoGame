using System;
using Android.Content;
using Android.Graphics;
using Android.Hardware;
using Android.Views;
using Org.Libsdl.App;

namespace Microsoft.Xna.Framework;

// This only admits the current activity's real SDL callbacks. Surface generation,
// invalidation and recovery remain independent of this peer-ownership boundary.
internal sealed class AndroidNativeSdlSurface : SDLSurface
{
    internal Func<bool> IsCurrentOwner { private get; set; }

    public AndroidNativeSdlSurface(Context context) : base(context) { }

    private bool IsOwner => IsCurrentOwner?.Invoke() == true;

    public override void SurfaceCreated(ISurfaceHolder holder)
    {
        if (IsOwner) { base.SurfaceCreated(holder); }
    }

    public override void SurfaceChanged(ISurfaceHolder holder, Format format, int width, int height)
    {
        if (IsOwner) { base.SurfaceChanged(holder, format, width, height); }
    }

    public override void SurfaceDestroyed(ISurfaceHolder holder)
    {
        if (IsOwner)
        {
            base.SurfaceDestroyed(holder);
        }
        else
        {
            MIsSurfaceReady = false;
            base.HandlePause();
        }
    }

    public override bool OnKey(View view, Keycode keyCode, KeyEvent keyEvent)
        => IsOwner && base.OnKey(view, keyCode, keyEvent);

    public override bool OnTouch(View view, MotionEvent motionEvent)
        => IsOwner && base.OnTouch(view, motionEvent);

    public override bool OnCapturedPointerEvent(MotionEvent motionEvent)
        => OperatingSystem.IsAndroidVersionAtLeast(26) &&
            IsOwner && base.OnCapturedPointerEvent(motionEvent);

    public override bool DispatchGenericMotionEvent(MotionEvent motionEvent)
        => IsOwner && base.DispatchGenericMotionEvent(motionEvent);

    public override void OnSensorChanged(SensorEvent sensorEvent)
    {
        if (IsOwner) { base.OnSensorChanged(sensorEvent); }
    }

    public override void HandleResume()
    {
        if (IsOwner) { base.HandleResume(); }
    }

    public override void EnableSensor(int sensorType, bool enabled)
    {
        if (!enabled || IsOwner) { base.EnableSensor(sensorType, enabled); }
    }
}
