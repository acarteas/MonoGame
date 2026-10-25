// Isolate the production query-only helpers without exporting renderer internals.
#include "api_common.h"
#include "api_enums.h"
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <locale>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

struct MGG_Texture
{
    bool isTarget = true;
    bool isSwapchain = false;
    uint64_t id = 0;
    VkImageCreateInfo info = {};
    VkImage image = (VkImage)1;
    VkImage msImage = VK_NULL_HANDLE;
    MGTextureType type = MGTextureType::_2D;
    MGSurfaceFormat format = MGSurfaceFormat::Color;
    MGDepthFormat depthFormat = MGDepthFormat::None;
    MGRenderTargetUsage usage = MGRenderTargetUsage::PreserveContents;
    int multiSampleCount = 0;
    MGG_Texture* depthTexture = nullptr;
};
struct MGG_GraphicsDevice
{
    std::queue<MGG_Texture*> destroyTextures;
    std::vector<MGG_Texture*> all_textures;
};
#define MGVK_SNAPSHOT_HELPERS_ONLY
#include "../native/monogame/vulkan/MGG_VulkanSnapshot.inl"

static void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::abort();
    }
}
int main()
{
    MGG_GraphicsDevice device;
    Check(SnapshotOffscreenTargets(&device) == "[]", "Observed empty live set");
    MGG_Texture color, twin, depth, regular, swapchain, unsuccessful;
    color.id = 7;
    color.info.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.info.extent = {1280, 720, 1};
    color.info.samples = VK_SAMPLE_COUNT_1_BIT;
    color.info.arrayLayers = 2;
    color.info.mipLevels = 3;
    color.info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    color.info.imageType = VK_IMAGE_TYPE_2D;
    color.info.tiling = VK_IMAGE_TILING_OPTIMAL;
    color.info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    color.depthFormat = MGDepthFormat::Depth24Stencil8;
    depth.id = 8;
    depth.info = color.info;
    depth.info.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    color.depthTexture = &depth;
    twin = color;
    twin.id = 4;
    twin.depthTexture = nullptr;
    twin.depthFormat = MGDepthFormat::None;
    regular.isTarget = false;
    swapchain.isSwapchain = true;
    unsuccessful.image = VK_NULL_HANDLE;
    device.all_textures = {&color, &regular, &swapchain, &twin, &unsuccessful};
    const auto json = SnapshotOffscreenTargets(&device);
    Check(json.find("\"targetId\":4") < json.find("\"targetId\":7"), "Sort by monotonic ID");
    Check(json.find("\"targetId\":0") == std::string::npos, "Exclude ordinary/swapchain/unsuccessful images");
    Check(json.find("\"format\":" + std::to_string((int)VK_FORMAT_D32_SFLOAT_S8_UINT)) != std::string::npos, "Actual fallback depth, not requested D24S8");
    Check(json.find("\"nativeDepthFormat\":3") != std::string::npos, "Independent requested depth enum");
    Check(json.find("\"arrayLayers\":2") != std::string::npos && json.find("\"mipLevels\":3") != std::string::npos, "Actual layers/mips");
    Check(json.find("\"width\":1280") != std::string::npos && json.find("\"flags\":8") != std::string::npos, "Actual extent/flags");
    Check(json.find("ABSENT_NOT_REQUESTED") != std::string::npos, "No depth distinguished");
    Check(json.find("\"samples\":1") != std::string::npos, "Actual samples");
    Check(json.find("\"resourceId\":8") != std::string::npos, "Attached depth ID");
    color.msImage = (VkImage)2;
    Check(SnapshotOffscreenTargets(&device).find("UNOBSERVED_CREATE_INFO_NOT_RETAINED") != std::string::npos, "MSAA info must not be inferred");
    device.destroyTextures.push(&depth);
    const auto pendingDepth = SnapshotOffscreenTargets(&device);
    Check(pendingDepth.find("UNAVAILABLE_QUEUED") != std::string::npos && pendingDepth.find("\"resourceId\":8") == std::string::npos, "Exclude queued child without reading its fields");
    auto* retiredChild = new MGG_Texture();
    color.depthTexture = retiredChild;
    delete retiredChild;
    device.destroyTextures = {};
    device.destroyTextures.push(&color);
    device.all_textures.push_back(reinterpret_cast<MGG_Texture*>(1));
    device.destroyTextures.push(reinterpret_cast<MGG_Texture*>(1));
    Check(SnapshotOffscreenTargets(&device).find("\"targetId\":7") == std::string::npos, "Exclude queued parent BEFORE dereferencing retired child");
    device.all_textures = {&twin};
    device.destroyTextures = {};
    Check(SnapshotOffscreenTargets(&device).find("\"targetId\":4") != std::string::npos, "Completed retirement preserves remaining live parent");
    device.all_textures.clear();
    Check(SnapshotOffscreenTargets(&device) == "[]", "Empty after retirement");
    std::cout << json << '\n' << "Offscreen scalar, ordering, queue and retired-child tests passed\n";
}
