using System;
using Android.Content;
using Android.Graphics;
using Android.Hardware;
using Android.Views;
using Org.Libsdl.App;

namespace Microsoft.Xna.Framework;

// The Activity owns the lifecycle barrier; this is its one SDL surface path.
internal sealed class AndroidNativeSdlSurface : SDLSurface
{
    internal Func<bool> IsCurrentOwner { private get; set; }
    internal Func<bool> Created { private get; set; }
    internal Action<bool> Changed { private get; set; }
    internal Func<bool> Destroying { private get; set; }
    internal Action Destroyed { private get; set; }

    public AndroidNativeSdlSurface(Context context) : base(context) { }

    private bool IsOwner => IsCurrentOwner?.Invoke() == true;

    public override void SurfaceCreated(ISurfaceHolder holder)
    {
        if (IsOwner && Created?.Invoke() == true)
        {
            base.SurfaceCreated(holder);
        }
    }

    public override void SurfaceChanged(ISurfaceHolder holder, Format format, int width, int height)
    {
        if (IsOwner)
        {
            base.SurfaceChanged(holder, format, width, height);
            Changed?.Invoke(MIsSurfaceReady);
        }
    }

    public override void SurfaceDestroyed(ISurfaceHolder holder)
    {
        if (Destroying?.Invoke() == true)
        {
            base.SurfaceDestroyed(holder);
            Destroyed?.Invoke();
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
