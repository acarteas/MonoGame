using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;

internal static unsafe class Program
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Request { public uint Size, Version, BufferBytes, RequiredBytes; }
    [DllImport("libmgruntime.so", EntryPoint = "MGG_GraphicsDevice_QueryVulkanSnapshot", CallingConvention = CallingConvention.Cdecl)]
    private static extern int Query(nint device, uint requestBytes, Request* request, byte* utf8);
    private static string Output;
    private static void Check(bool value, string label) { if (!value) { throw new Exception(label); } }
    private static Request NewRequest() => new Request { Size = 16, Version = 1 };
    private static object ManagedQuery(GraphicsDevice device) => typeof(GraphicsDevice).GetMethod("GetVulkanCapabilitySnapshot", BindingFlags.NonPublic | BindingFlags.Instance).Invoke(device, null);
    private static void ExpectManagedFailure(GraphicsDevice device, Type type)
    {
        try { ManagedQuery(device); throw new Exception("Expected managed failure"); }
        catch (TargetInvocationException e) { Check(e.InnerException.GetType() == type, "Managed exception type"); }
    }
    private static void Negative(nint device, bool live)
    {
        var request = NewRequest();
        Check(Query(device, 16, null, null) == -1, "Null request");
        Check(Query(device, 0, (Request*)1, null) == -1, "Tiny caller size before dereference");
        request.Size = 15;
        Check(Query(device, 16, &request, null) == -1, "Bad structure size");
        request = NewRequest(); request.Version = 99;
        Check(Query(device, 16, &request, null) == -2 && request.RequiredBytes == 0, "Version");
        request = NewRequest(); request.BufferBytes = 1;
        Check(Query(device, 16, &request, null) == -1 && request.RequiredBytes == 0, "Null buffer with capacity");
        request = NewRequest(); byte sentinel = 0xA5;
        Check(Query(device, 16, &request, &sentinel) == -1 && sentinel == 0xA5, "Buffer with zero capacity");
        request = NewRequest();
        Check(Query(device, 16, &request, null) == (live ? 1 : -3), "Live membership");
        if (!live) { Check(request.RequiredBytes == 0, "Missing report size"); return; }
        Check(request.RequiredBytes > 16, "Size query");
        request.BufferBytes = 1;
        Check(Query(device, 16, &request, &sentinel) == 1 && sentinel == 0xA5, "No partial write");
        var buffer = new byte[request.RequiredBytes + 128];
        request.BufferBytes = (uint)buffer.Length;
        fixed (byte* ptr = buffer) { Check(Query(device, 16, &request, ptr) == 0, "Full native report"); }
        Check(buffer[request.RequiredBytes - 1] == 0, "Terminator");
        var text = new UTF8Encoding(false, true).GetString(buffer, 0, (int)request.RequiredBytes - 1);
        using var document = JsonDocument.Parse(text);
        var root = document.RootElement;
        Check(root.GetProperty("schemaVersion").GetInt32() == 1, "Schema");
        Check(root.GetProperty("deviceCreation").GetProperty("succeeded").GetBoolean(), "Device creation outcome");
        Check(root.GetProperty("imageQueries").GetArrayLength() == 17, "Combined usage queries");
        Canonical(root);
        File.WriteAllText(Path.Combine(Output, "native-snapshot.json"), text + "\n");
    }
    private static void Canonical(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            var names = element.EnumerateObject().Select(x => x.Name).ToArray();
            Check(names.SequenceEqual(names.OrderBy(x => x, StringComparer.Ordinal)), "Canonical object keys");
            foreach (var property in element.EnumerateObject()) { Canonical(property.Value); }
        }
        if (element.ValueKind == JsonValueKind.Array) { foreach (var value in element.EnumerateArray()) { Canonical(value); } }
    }
    private sealed class Probe : Game
    {
        private readonly GraphicsDeviceManager Manager;
        internal nint NativeHandle;
        public Probe() { Manager = new GraphicsDeviceManager(this); Manager.PreferredBackBufferWidth = 320; Manager.PreferredBackBufferHeight = 180; }
        protected override void Initialize()
        {
            base.Initialize();
            NativeHandle = (nint)Pointer.Unbox(typeof(GraphicsDevice).GetField("Handle", BindingFlags.NonPublic | BindingFlags.Instance).GetValue(GraphicsDevice));
            Negative(NativeHandle, true);
            var first = ManagedQuery(GraphicsDevice);
            var second = ManagedQuery(GraphicsDevice);
            var text = (string)first.GetType().GetProperty("Json").GetValue(first);
            var text2 = (string)second.GetType().GetProperty("Json").GetValue(second);
            using var d1 = JsonDocument.Parse(text);
            using var d2 = JsonDocument.Parse(text2);

            // Memory-budget usage can change independently between read-only queries.
            Check(d1.RootElement.GetProperty("identity").GetRawText() == d2.RootElement.GetProperty("identity").GetRawText(), "Repeated identity");
            Check(d1.RootElement.GetProperty("coreFeatures").GetRawText() == d2.RootElement.GetProperty("coreFeatures").GetRawText(), "Repeated feature chain");
            Canonical(d1.RootElement); Canonical(d2.RootElement);
            File.WriteAllText(Path.Combine(Output, "managed-snapshot.json"), text + "\n");

            // Record the actual mapped binary while it is live.
            File.WriteAllText(Path.Combine(Output, "maps.txt"), File.ReadAllText("/proc/self/maps"));
        }
        protected override void Update(GameTime gameTime) { Exit(); }
        protected override void Draw(GameTime gameTime) { }
    }
    private static void Main(string[] args)
    {
        Output = args[0]; Directory.CreateDirectory(Output);
        Check(sizeof(Request) == 16, "Managed ABI size");
        Negative(0, false); Negative((nint)1, false);
        var missing = (GraphicsDevice)RuntimeHelpers.GetUninitializedObject(typeof(GraphicsDevice));
        ExpectManagedFailure(missing, typeof(InvalidOperationException));
        using var probe = new Probe();
        probe.Run();
        var device = probe.GraphicsDevice;
        var handle = probe.NativeHandle;
        probe.Dispose();
        Negative(handle, false);
        ExpectManagedFailure(device, typeof(ObjectDisposedException));
        File.WriteAllText(Path.Combine(Output, "result.json"), "{\"status\":\"PASS\",\"schemaVersion\":1,\"rendering\":\"No draws; selected-device query smoke only\"}\n");
        Console.WriteLine("Vulkan selected-device native/managed size, version, buffer, repeat and lifetime checks passed.");
    }
}
