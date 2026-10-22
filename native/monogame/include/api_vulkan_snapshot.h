// Separately versioned Vulkan diagnostics. Does not change the Android host ABI.
#pragma once
#include "api_common.h"
#include <stdint.h>
struct MGG_GraphicsDevice;
struct MGG_VulkanSnapshotRequest
{
    uint32_t Size;
    uint32_t Version;
    uint32_t BufferBytes;
    uint32_t RequiredBytes;
};
// UTF8 JSON including a terminating NUL. Query with BufferBytes=0 and utf8=nullptr.
// 0=success, 1=buffer too small, -1=invalid argument/size, -2=unsupported version,
// -3=device unavailable, -4=report generation failed. No output bytes are written on failure. RequiredBytes
// is set only for a live device. Serialize calls with other device operations.
MG_EXPORT int32_t MGG_GraphicsDevice_QueryVulkanSnapshot(MGG_GraphicsDevice* device,
    uint32_t requestBytes, MGG_VulkanSnapshotRequest* request, char* utf8);
