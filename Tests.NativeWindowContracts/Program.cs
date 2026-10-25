// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

using System;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;
using MonoGame.Framework.Utilities;
using MonoGame.Interop;

// Link the actual Native window and PlatformInfo sources. Only their dependencies
// are doubled; no runtime native library, graphics driver, or Android device is used.
internal static class Program
{
    private static void Check(bool condition)
    {
        if (!condition) { throw new Exception("Native window contract failed."); }
    }

    private static void Main(string[] args)
    {
        MGP.Android = args.Length == 1 && args[0] == "android";
        CheckContracts();
    }

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private static void CheckContracts()
    {
        Check((int)MonoGamePlatform.Android == 0 && (int)MonoGamePlatform.DesktopVK == 12);
        Check((int)MonoGamePlatform.WindowsDX12 == 7 && (int)MonoGamePlatform.NintendoSwitch2 == 13);
        Check((int)GraphicsBackend.Vulkan == 2 && (int)GraphicsBackend.DirectX12 == 4);
        Check(PlatformInfo.MonoGamePlatform == (MGP.Android ? MonoGamePlatform.Android : MonoGamePlatform.DesktopVK));
        Check(PlatformInfo.GraphicsBackend == GraphicsBackend.Vulkan);

        var platform = new NativeGamePlatform();
        var window = new NativeGameWindow(platform, true);
        Check(window.ClientBounds.Width == (MGP.Android ? 2340 : 800));
        Check(window.ClientBounds.Height == (MGP.Android ? 1080 : 480));
        Check(AssemblyHelper.IconQueries == (MGP.Android ? 0 : 1));
        var pp = platform.Game.GraphicsDevice.PresentationParameters;
        pp.BackBufferWidth = 320;
        pp.BackBufferHeight = 200;
        pp.IsFullScreen = true;
        pp.HardwareModeSwitch = true;
        window.OnPresentationChanged(pp);
        Check(pp.BackBufferWidth == (MGP.Android ? 2340 : 320));
        Check(pp.BackBufferHeight == (MGP.Android ? 1080 : 200));
        Check(MGP.SetSizeCalls == (MGP.Android ? 0 : 1));
        Check(MGP.FullScreenCalls == (MGP.Android ? 0 : 1));

        int resized = 0;
        window.ClientSizeChanged += (_, _) => ++resized;
        MGP.Width = 2160;
        MGP.Height = 1000;
        window.ClientResize(720, 333);
        Check(window.ClientBounds.Width == (MGP.Android ? 2160 : 720));
        Check(window.ClientBounds.Height == (MGP.Android ? 1000 : 333));
        Check(pp.BackBufferWidth == window.ClientBounds.Width && pp.BackBufferHeight == window.ClientBounds.Height);
        Check(platform.Game.GraphicsDevice.Viewport.Width == pp.BackBufferWidth);
        Check(platform.Game.GraphicsDevice.Viewport.Height == pp.BackBufferHeight);
        Check(resized == 1 && MGP.SetSizeCalls == (MGP.Android ? 0 : 2));
        window.ClientResize(720, 333);
        Check(resized == 1);
        window.Destroy();
        Console.WriteLine($"{(MGP.Android ? "Android" : "Desktop Vulkan")} managed Native window contracts passed (boundary doubles)");
    }
}

namespace Microsoft.Xna.Framework
{
    public struct Point { public int X, Y; public Point(int x, int y) { X = x; Y = y; } }
    public struct Rectangle { public int Width, Height; public Rectangle(int x, int y, int width, int height) { Width = width; Height = height; } }
    public enum DisplayOrientation { Default }
    public abstract class GameWindow
    {
        public abstract bool AllowUserResizing { get; set; }
        public abstract bool IsBorderless { get; set; }
        public abstract Point Position { get; set; }
        public abstract Rectangle ClientBounds { get; }
        public abstract DisplayOrientation CurrentOrientation { get; }
        public abstract IntPtr Handle { get; }
        public abstract string ScreenDeviceName { get; }
        public string Title { get; }
        public event EventHandler ClientSizeChanged;
        protected void OnClientSizeChanged() { ClientSizeChanged?.Invoke(this, EventArgs.Empty); }
        public abstract void BeginScreenDeviceChange(bool fullScreen);
        public abstract void EndScreenDeviceChange(string name, int width, int height);
        protected internal abstract void SetSupportedOrientations(DisplayOrientation orientations);
        protected abstract void SetTitle(string title);
    }
    internal unsafe class NativeGamePlatform { public MGP_Platform* Handle = (MGP_Platform*)1; public Game Game = new(); }
    public class Game { public GraphicsDevice GraphicsDevice = new(); }
    public static class GraphicsDeviceManager { public const int DefaultBackBufferWidth = 800, DefaultBackBufferHeight = 480; }
}
namespace Microsoft.Xna.Framework.Graphics
{
    public class NoSuitableGraphicsDeviceException : Exception { public NoSuitableGraphicsDeviceException(string message) : base(message) { } }
    public class PresentationParameters { public bool IsFullScreen, HardwareModeSwitch; public int BackBufferWidth, BackBufferHeight; }
    public struct Viewport { public int Width, Height; public Viewport(int x, int y, int width, int height) { Width = width; Height = height; } }
    public class GraphicsDevice { public PresentationParameters PresentationParameters = new(); public Viewport Viewport; }
}
namespace MonoGame.Framework.Utilities
{
    internal static class AssemblyHelper
    {
        internal static int IconQueries;
        public static string GetDefaultWindowTitle() => "Contract test";
        public static byte[] GetDefaultWindowIcon() { ++IconQueries; return null; }
    }
}
namespace MonoGame.Interop
{
    internal struct MGP_Window { }
    internal struct MGP_Platform { }
    internal static unsafe class MGP
    {
        internal static bool Android;
        internal static int Width = 2340, Height = 1080, SetSizeCalls, FullScreenCalls;
        public static MonoGamePlatform Platform_GetPlatform() => Android ? MonoGamePlatform.Android : MonoGamePlatform.DesktopVK;
        public static GraphicsBackend Platform_GetGraphicsBackend() => GraphicsBackend.Vulkan;
        public static MGP_Window* Window_Create(MGP_Platform* platform, ref int width, ref int height, string title)
        { if (Android) { width = Width; height = Height; } return (MGP_Window*)1; }
        public static void Window_GetClientSize(MGP_Window* window, out int width, out int height) { width = Width; height = Height; }
        public static void Window_SetClientSize(MGP_Window* window, int width, int height) { ++SetSizeCalls; }
        public static void Window_EnterFullScreen(MGP_Window* window, byte mode) { ++FullScreenCalls; }
        public static void Window_ExitFullScreen(MGP_Window* window) { ++FullScreenCalls; }
        public static nint Window_GetNativeHandle(MGP_Window* window) => 1;
        public static byte Window_GetAllowUserResizing(MGP_Window* window) => 0;
        public static void Window_SetAllowUserResizing(MGP_Window* window, byte value) { }
        public static byte Window_GetIsBorderless(MGP_Window* window) => 0;
        public static void Window_SetIsBorderless(MGP_Window* window, byte value) { }
        public static void Window_GetPosition(MGP_Window* window, out int x, out int y) { x = y = 0; }
        public static void Window_SetPosition(MGP_Window* window, int x, int y) { }
        public static void Window_SetIconBitmap(MGP_Window* window, byte* icon, int length) { }
        public static void Window_SetTitle(MGP_Window* window, string title) { }
        public static void Window_Show(MGP_Window* window, byte show) { }
        public static void Window_Destroy(MGP_Window* window) { }
    }
}
