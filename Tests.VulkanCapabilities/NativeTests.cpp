#include "api_vulkan_snapshot.h"
#include <cstdlib>
#include <cstddef>
#include <cstdio>

static void Check(bool condition)
{
    if (!condition) { std::fputs("Native snapshot contract test failed\n", stderr); std::abort(); }
}

int main()
{
    static_assert(sizeof(MGG_VulkanSnapshotRequest) == 16);
    static_assert(offsetof(MGG_VulkanSnapshotRequest, RequiredBytes) == 12);
    MGG_VulkanSnapshotRequest request = {16, 1, 0, 123};
    auto query = MGG_GraphicsDevice_QueryVulkanSnapshot;
    Check(query(nullptr, 16, nullptr, nullptr) == -1);
    Check(query(nullptr, 0, reinterpret_cast<MGG_VulkanSnapshotRequest*>(1), nullptr) == -1);
    Check(query(nullptr, 15, &request, nullptr) == -1);
    request.Size = 17;
    Check(query(nullptr, 16, &request, nullptr) == -1);
    request = {16, 2, 0, 123};
    Check(query(nullptr, 16, &request, nullptr) == -2 && request.RequiredBytes == 0);
    request = {16, 1, 1, 123};
    Check(query(nullptr, 16, &request, nullptr) == -1 && request.RequiredBytes == 0);
    request = {16, 1, 0, 123};
    char sentinel = 'x';
    Check(query(nullptr, 16, &request, &sentinel) == -1 && sentinel == 'x');
    Check(query(nullptr, 16, &request, nullptr) == -3 && request.RequiredBytes == 0);
    Check(query(reinterpret_cast<MGG_GraphicsDevice*>(1), 16, &request, nullptr) == -3);
    std::puts("Native snapshot structure/version/missing-device/buffer tests passed");
}
