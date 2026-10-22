// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

using System;
using System.Runtime.InteropServices;
using System.Text;

namespace MonoGame.Interop
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct MGG_VulkanSnapshotRequest
    {
        public uint Size;
        public uint Version;
        public uint BufferBytes;
        public uint RequiredBytes;
    }

    internal static unsafe partial class MGG
    {
        [DllImport(MGP.MonoGameNativeDLL, EntryPoint = "MGG_GraphicsDevice_QueryVulkanSnapshot", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
        internal static extern int GraphicsDevice_QueryVulkanSnapshot(MGG_GraphicsDevice* device,
            uint requestBytes, ref MGG_VulkanSnapshotRequest request, byte* utf8);
    }

    // An owned UTF8 JSON copy. Native handles and native memory never escape in
    // the report. Schema v1 object keys are ordinally sorted, extension names
    // are sorted, and format/image/queue arrays have a fixed documented order.
    internal sealed class VulkanCapabilitySnapshot
    {
        internal const uint CurrentVersion = 1;
        public uint SchemaVersion => CurrentVersion;
        public string Json { get; }

        internal VulkanCapabilitySnapshot(string json) { Json = json; }

        internal static unsafe VulkanCapabilitySnapshot Query(MGG_GraphicsDevice* device)
        {
            if (device == null) { throw new InvalidOperationException("The selected native graphics device is unavailable."); }
            var request = new MGG_VulkanSnapshotRequest
            {
                Size = (uint)Marshal.SizeOf<MGG_VulkanSnapshotRequest>(),
                Version = CurrentVersion
            };
            var result = MGG.GraphicsDevice_QueryVulkanSnapshot(device, request.Size, ref request, null);
            if (result != 1) { throw new InvalidOperationException($"Vulkan snapshot size query failed ({result})."); }
            for (var attempt = 0; attempt < 3; attempt++)
            {

                // Bound allocation even if an incompatible library supplies a
                // malformed size. Current reports are much smaller than 4 MiB.
                if (request.RequiredBytes == 0 || request.RequiredBytes > 4 * 1024 * 1024)
                {
                    throw new InvalidOperationException("Vulkan snapshot returned an invalid size.");
                }
                var bytes = new byte[request.RequiredBytes];
                request.BufferBytes = (uint)bytes.Length;
                fixed (byte* buffer = bytes)
                {
                    result = MGG.GraphicsDevice_QueryVulkanSnapshot(device, request.Size, ref request, buffer);
                }
                if (result == 1) { continue; }
                if (result != 0 || request.RequiredBytes == 0 || request.RequiredBytes > bytes.Length || bytes[request.RequiredBytes - 1] != 0)
                {
                    throw new InvalidOperationException($"Vulkan snapshot query failed ({result}).");
                }
                var json = new UTF8Encoding(false, true).GetString(bytes, 0, (int)request.RequiredBytes - 1);
                return new VulkanCapabilitySnapshot(json);
            }
            throw new InvalidOperationException("Vulkan snapshot size changed repeatedly during query.");
        }
    }
}
