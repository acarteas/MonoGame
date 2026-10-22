using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Input;
using Microsoft.Xna.Framework.Input.Touch;
using MonoGame.Interop;

internal static unsafe class Program
{
    private static NativeGamePlatform retiring;
    private static string observerFailure;
    private static int observations;
    private static delegate* unmanaged[Cdecl]<int, int> count;

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new Exception(message);
        }
    }

    private static void Neutral()
    {
        Check(GamePad.Handle == null, "retired gamepad native handle");
        Check(!GamePad.GetCapabilities(0).IsConnected && !GamePad.GetCapabilities(1).IsConnected, "retired controller maps");
        Check(!GamePad.GetState(0).IsConnected && GamePad.GetState(0).Buttons.A == ButtonState.Released, "neutral controller");
        Check(Keyboard.GetState().GetPressedKeys().Length == 0, "neutral keyboard");
        Check(Mouse.PrimaryWindow == null && TouchPanel.PrimaryWindow == null, "retired primary windows");
        Check(Mouse.GetState() == default(MouseState), "neutral mouse");
        Check(MessageBox._window == null, "retired messagebox window");
        int before = count(2);
        Check(!GamePad.SetVibration(0, 1, 1) && count(2) == before, "no retired native vibration");
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void BeforeNativeDestroy()
    {
        try
        {
            Neutral();
            if (retiring != null)
            {
                Check(GamePad.OwnsNativePlatform(retiring, retiring.Handle), "owner reserved through native destroy");
            }
            bool reserved = false;
            try { GamePad.EnsureNativePlatformAvailable(); }
            catch (InvalidOperationException) { reserved = true; }
            Check(reserved, "replacement blocked before destroy returns");
            ++observations;
        }
        catch (Exception error)
        {
            observerFailure = error.ToString();
        }
    }

    private static void Main(string[] args)
    {
        nint shim = NativeLibrary.Load(args[0]);
        NativeLibrary.SetDllImportResolver(typeof(Game).Assembly, (name, _, _) => name == "mgruntime" ? shim : 0);
        count = (delegate* unmanaged[Cdecl]<int, int>)NativeLibrary.GetExport(shim, "Test_Count");
        var configure = (delegate* unmanaged[Cdecl]<int, delegate* unmanaged[Cdecl]<void>, void>)NativeLibrary.GetExport(shim, "Test_Configure");
        bool desktop = args[1] == "desktop";
        configure(desktop ? 1 : 0, desktop ? null : &BeforeNativeDestroy);
        Run(args[1]);
        Console.WriteLine($"PASS actual Native input contracts: {args[1]}");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void Run(string mode)
    {
        if (mode.StartsWith("constructor"))
        {
            Exception failure = null;
            try { _ = new Game(); }
            catch (Exception error) { failure = error; }
            Check(failure is EntryPointNotFoundException && failure.Message.Contains("MGP_Mouse_SetVisible"), "original constructor exception preserved");
            Neutral();
            if (mode == "constructor")
            {
                Check(count(1) == 1, "constructor rollback destroys platform");
                Check(observerFailure == null && observations == 1, observerFailure ?? "constructor rollback ordering");
                GamePad.EnsureNativePlatformAvailable();
            }
            else
            {
                Check(count(1) == 0, "failed constructor rollback retains native platform");
                CheckQuarantined();
            }
            return;
        }

        var first = new Game();
        var old = (NativeGamePlatform)first.Platform;
        retiring = old;
        var nativeAddress = old.Handle;
        GamePad.Add(0);
        Check(GamePad.GetCapabilities(0).Identifier == "controller-session-1", "first session caps");
        GamePad.ChangeState(0, 1, ControllerInput.A, 1);
        GamePad.ChangeState(0, 2, ControllerInput.LeftStickX, 32767);
        Keyboard.Keys.Add(Keys.A);
        first.Window.MouseState = new MouseState(8, 9, 10, ButtonState.Pressed, ButtonState.Released, ButtonState.Released, ButtonState.Released, ButtonState.Released);
        Check(GamePad.GetState(0).Buttons.A == ButtonState.Pressed && Mouse.GetState().X == 8, "live input set");
        bool duplicateRejected = false;
        try { GamePad.Add(0); }
        catch (ArgumentException) { duplicateRejected = true; }
        Check(duplicateRejected, "duplicate Add remains strict within session");

        if (mode == "destroy")
        {
            Exception failure = null;
            try { old.Dispose(); }
            catch (Exception error) { failure = error; }
            Check(failure is EntryPointNotFoundException && failure.Message.Contains("MGP_Window_Destroy"), "native window destroy exception preserved");
            Neutral();
            Check(count(1) == 0 && GamePad.OwnsNativePlatform(old, nativeAddress), "failed retirement reservation");
            CheckQuarantined();
            GC.KeepAlive(first);
            return;
        }

        first.Dispose();
        if (mode == "desktop")
        {
            Check(GamePad.Handle == nativeAddress && GamePad.GetCapabilities(0).IsConnected, "desktop controller behavior preserved");
            Check(Keyboard.Keys.Contains(Keys.A) && MessageBox._window != null, "desktop cleanup unchanged");
            Check(!GamePad.OwnsNativePlatform(old, nativeAddress), "desktop never binds Android owner");
            return;
        }

        Check(observerFailure == null && observations == 1 && count(1) == 1, observerFailure ?? "first destroy ordering");
        Neutral();
        GamePad.EnsureNativePlatformAvailable();
        var second = new Game();
        retiring = (NativeGamePlatform)second.Platform;
        Check(retiring.Handle == nativeAddress, "native pointer explicitly reused");
        GamePad.Add(0);
        Check(GamePad.GetCapabilities(0).Identifier == "controller-session-2" && !GamePad.GetCapabilities(1).IsConnected, "fresh ID0 caps at slot0");
        Check(GamePad.GetState(0).Buttons.A == ButtonState.Released && GamePad.GetState(0).ThumbSticks.Left.X == 0, "fresh neutral controller state");
        Keyboard.Keys.Add(Keys.B);
        _ = NativeGamePlatform.GraphicsSystem;
        Check(!GamePad.ClearNativePlatformInput(old, nativeAddress) && !GamePad.CompleteNativePlatformRetirement(old, nativeAddress), "stale managed owner rejected at reused pointer");
        old.Dispose();
        Check(count(1) == 1 && count(3) == 0 && Keyboard.Keys.Contains(Keys.B) && GamePad.OwnsNativePlatform(retiring, nativeAddress), "stale Dispose preserves replacement input and graphics");
        Check(GamePad.SetVibration(0, 1, 1), "replacement vibration uses live handle");
        second.Dispose();
        Check(observerFailure == null && observations == 2 && count(1) == 2 && count(3) == 1, observerFailure ?? "second destroy ordering");
        Neutral();
        GamePad.EnsureNativePlatformAvailable();
    }

    private static void CheckQuarantined()
    {
        int before = count(0);
        bool rejected = false;
        try { _ = new Game(); }
        catch (InvalidOperationException) { rejected = true; }
        Check(rejected && count(0) == before, "quarantine rejects replacement before native create");
    }
}
