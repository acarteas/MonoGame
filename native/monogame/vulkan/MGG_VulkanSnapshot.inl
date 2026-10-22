// Included in the Vulkan implementation so diagnostics use exactly the same
// format selection and device creation state as the renderer.
#include <map>
#include <set>

namespace
{
using SnapshotObject = std::map<std::string, std::string>;
static std::string SnapshotString(const std::string& value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << '"';
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\') { out << '\\' << c; }
        else if (c < 32) { out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (unsigned)c << std::dec; }
        else { out << c; }
    }
    out << '"';
    return out.str();
}
template<class T> static std::string SnapshotNumber(T value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(9) << value;
    return out.str();
}
static std::string SnapshotBool(bool value) { return value ? "true" : "false"; }
static std::string SnapshotMap(const SnapshotObject& values)
{
    std::string result = "{";
    for (const auto& entry : values)
    {
        if (result.size() > 1) { result += ','; }
        result += SnapshotString(entry.first) + ':' + entry.second;
    }
    return result + '}';
}
static std::string SnapshotArray(const std::vector<std::string>& values, bool strings = false)
{
    std::string result = "[";
    for (const auto& value : values)
    {
        if (result.size() > 1) { result += ','; }
        result += strings ? SnapshotString(value) : value;
    }
    return result + ']';
}
static std::string SnapshotImage(const MGG_Texture* texture)
{
    const auto& info = texture->info;
    return SnapshotMap({{"resourceId", SnapshotNumber(texture->id)},
        {"format", SnapshotNumber(info.format)},
        {"extent", SnapshotMap({{"width", SnapshotNumber(info.extent.width)},
            {"height", SnapshotNumber(info.extent.height)}, {"depth", SnapshotNumber(info.extent.depth)}})},
        {"samples", SnapshotNumber(info.samples)}, {"arrayLayers", SnapshotNumber(info.arrayLayers)},
        {"mipLevels", SnapshotNumber(info.mipLevels)}, {"usage", SnapshotNumber(info.usage)},
        {"imageType", SnapshotNumber(info.imageType)}, {"tiling", SnapshotNumber(info.tiling)},
        {"flags", SnapshotNumber(info.flags)}});
}
static std::string SnapshotOffscreenTargets(const MGG_GraphicsDevice* device)
{

    // Queued children can retire before their parent. Compare queued addresses
    // without dereferencing, and reject the parent before inspecting any fields.
    auto pending = device->destroyTextures;
    std::set<const MGG_Texture*> queued;
    while (!pending.empty())
    {
        queued.insert(pending.front());
        pending.pop();
    }
    std::vector<const MGG_Texture*> targets;
    for (const auto* texture : device->all_textures)
    {
        if (texture == nullptr || queued.count(texture) != 0)
        {
            continue;
        }
        if (texture->isTarget && !texture->isSwapchain && texture->image != VK_NULL_HANDLE)
        {
            targets.push_back(texture);
        }
    }
    std::sort(targets.begin(), targets.end(), [](const auto* left, const auto* right)
    {
        return left->id < right->id;
    });
    std::vector<std::string> rows;
    for (const auto* texture : targets)
    {
        const auto* depth = texture->depthTexture;
        const bool depthQueued = depth != nullptr && queued.count(depth) != 0;
        const bool depthLive = depth != nullptr && !depthQueued && depth->image != VK_NULL_HANDLE;
        const bool msaaPresent = texture->msImage != VK_NULL_HANDLE;
        rows.push_back(SnapshotMap({{"schemaVersion", "1"}, {"targetId", SnapshotNumber(texture->id)},
            {"requested", SnapshotMap({{"nativeTextureType", SnapshotNumber((int)texture->type)},
                {"nativeSurfaceFormat", SnapshotNumber((int)texture->format)},
                {"nativeDepthFormat", SnapshotNumber((int)texture->depthFormat)},
                {"nativeMultiSampleCount", SnapshotNumber(texture->multiSampleCount)},
                {"nativeRenderTargetUsage", SnapshotNumber((int)texture->usage)}})},
            {"color", SnapshotImage(texture)},
            {"depth", depthLive ? SnapshotImage(depth) : "null"},
            {"depthStatus", SnapshotString(depthLive ? "OBSERVED" : depthQueued ? "UNAVAILABLE_QUEUED" :
                depth != nullptr ? "UNAVAILABLE_NO_IMAGE" : texture->depthFormat == MGDepthFormat::None ? "ABSENT_NOT_REQUESTED" : "UNAVAILABLE_MISSING")},
            {"auxiliaryMsaa", SnapshotMap({{"present", SnapshotBool(msaaPresent)},
                {"image", "null"}, {"status", SnapshotString(msaaPresent ?
                    "UNOBSERVED_CREATE_INFO_NOT_RETAINED" : "ABSENT")}})}}));
    }
    return SnapshotArray(rows);
}
}

#ifndef MGVK_SNAPSHOT_HELPERS_ONLY
namespace
{
static bool SnapshotHas(const MGG_GraphicsDevice* device, const char* name)
{
    const auto& extensions = device->snapshotAvailableExtensions;
    return std::binary_search(extensions.begin(), extensions.end(), std::string(name));
}
static std::string SnapshotFeature(bool queried, VkBool32 available, VkBool32 requested, bool created)
{
    return SnapshotMap({{"available", queried ? SnapshotBool(available != 0) : "null"},
        {"enabled", SnapshotBool(created && requested != 0)}, {"queried", SnapshotBool(queried)},
        {"requested", SnapshotBool(requested != 0)}});
}
static std::string MGVK_MakeSnapshot(MGG_GraphicsDevice* device)
{
    const auto physical = device->physicalDevice;
    const auto& properties = device->deviceProperties;
    const auto& limits = properties.limits;
    const bool created = device->snapshotCreateResult == VK_SUCCESS;
    const auto requestedApiVersion = device->snapshotRequestedApiVersion == 0 ? VK_API_VERSION_1_0 : device->snapshotRequestedApiVersion;
    const auto effectiveInstanceVersion = std::min(requestedApiVersion, device->snapshotLoaderApiVersion);
    const auto effectiveDeviceVersion = std::min(effectiveInstanceVersion, properties.apiVersion);
    const bool instanceCore11 = effectiveInstanceVersion >= VK_API_VERSION_1_1;
    const bool deviceCore11 = effectiveDeviceVersion >= VK_API_VERSION_1_1;
    const bool deviceCore12 = effectiveDeviceVersion >= VK_API_VERSION_1_2;
    const auto queryProperties2 = instanceCore11 ? vkGetPhysicalDeviceProperties2 :
        (device->snapshotProperties2EXT ? vkGetPhysicalDeviceProperties2KHR : nullptr);
    const auto queryFeatures2 = instanceCore11 ? vkGetPhysicalDeviceFeatures2 :
        (device->snapshotProperties2EXT ? vkGetPhysicalDeviceFeatures2KHR : nullptr);
    const auto queryMemory2 = instanceCore11 ? vkGetPhysicalDeviceMemoryProperties2 :
        (device->snapshotProperties2EXT ? vkGetPhysicalDeviceMemoryProperties2KHR : nullptr);
    const bool properties2 = queryProperties2 != nullptr;
    const bool features2 = queryFeatures2 != nullptr;
    const std::string featuresRoute = instanceCore11 ? "vkGetPhysicalDeviceFeatures2" : "vkGetPhysicalDeviceFeatures2KHR";
    const std::string propertiesRoute = instanceCore11 ? "vkGetPhysicalDeviceProperties2" : "vkGetPhysicalDeviceProperties2KHR";
    const bool scalarQueryable = features2 && (deviceCore12 || SnapshotHas(device, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME));
    const bool borderQueryable = features2 && SnapshotHas(device, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES };
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    if (scalarQueryable) { scalar.pNext = features.pNext; features.pNext = &scalar; }
    if (borderQueryable) { border.pNext = features.pNext; features.pNext = &border; }
    if (features2) { queryFeatures2(physical, &features); }
    VkPhysicalDeviceDriverProperties driver = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceCustomBorderColorPropertiesEXT borderProperties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    const bool driverQueryable = properties2 && (deviceCore12 || SnapshotHas(device, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME));
    VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    const bool externalMemoryEnabled = std::binary_search(device->snapshotEnabledInstanceExtensions.begin(),
        device->snapshotEnabledInstanceExtensions.end(), std::string(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME));
    const bool idsQueryable = properties2 && (deviceCore11 || externalMemoryEnabled);
    if (idsQueryable) { ids.pNext = props.pNext; props.pNext = &ids; }
    if (driverQueryable) { driver.pNext = props.pNext; props.pNext = &driver; }
    if (properties2 && borderQueryable) { borderProperties.pNext = props.pNext; props.pNext = &borderProperties; }
    if (properties2) { queryProperties2(physical, &props); }
    auto uuid = [](const uint8_t* bytes)
    {
        std::ostringstream value;
        value << std::hex << std::setfill('0');
        for (uint32_t i = 0; i < VK_UUID_SIZE; ++i) { value << std::setw(2) << (unsigned)bytes[i]; }
        return SnapshotString(value.str());
    };
    SnapshotObject root;
    root["schemaVersion"] = "1";
    root["status"] = SnapshotString("SELECTED_DEVICE_SNAPSHOT");
    root["identity"] = SnapshotMap({{"adapterIndex", SnapshotNumber(device->snapshotAdapterIndex)},
        {"apiVersion", SnapshotNumber(properties.apiVersion)}, {"deviceID", SnapshotNumber(properties.deviceID)},
        {"deviceName", SnapshotString(properties.deviceName)}, {"deviceType", SnapshotNumber(properties.deviceType)},
        {"deviceUUID", idsQueryable ? uuid(ids.deviceUUID) : "null"}, {"driverUUID", idsQueryable ? uuid(ids.driverUUID) : "null"},
        {"uuidUnavailableReason", idsQueryable ? "null" : SnapshotString("Device ID query route was not enabled; no UUID inferred from another identity")},
        {"driverVersion", SnapshotNumber(properties.driverVersion)},
        {"driverID", driverQueryable ? SnapshotNumber(driver.driverID) : "null"},
        {"driverInfo", driverQueryable ? SnapshotString(driver.driverInfo) : "null"},
        {"driverName", driverQueryable ? SnapshotString(driver.driverName) : "null"},
        {"effectiveInstanceApiVersion", SnapshotNumber(effectiveInstanceVersion)},
        {"effectiveDeviceApiVersion", SnapshotNumber(effectiveDeviceVersion)},
        {"requestedInstanceApiVersion", SnapshotNumber(device->snapshotRequestedApiVersion)},
        {"loaderApiVersion", SnapshotNumber(device->snapshotLoaderApiVersion)}, {"vendorID", SnapshotNumber(properties.vendorID)}});
    root["extensions"] = SnapshotMap({{"available", SnapshotArray(device->snapshotAvailableExtensions, true)},
        {"enabled", SnapshotArray(device->snapshotEnabledExtensions, true)}});
    root["instanceExtensions"] = SnapshotMap({{"available", SnapshotArray(device->snapshotAvailableInstanceExtensions, true)},
        {"enabled", SnapshotArray(device->snapshotEnabledInstanceExtensions, true)}});
    root["deviceCreation"] = SnapshotMap({{"result", SnapshotNumber(device->snapshotCreateResult)},
        {"succeeded", SnapshotBool(created)}, {"enabledStateSource", SnapshotString("Exact submitted chain after successful vkCreateDevice; no inference from extension names")}});
    SnapshotObject core;
#define SNAP_FEATURE(name) core[#name] = SnapshotFeature(true, device->deviceFeatures.name, device->snapshotRequestedFeatures.name, created)
    SNAP_FEATURE(robustBufferAccess);
    SNAP_FEATURE(fullDrawIndexUint32);
    SNAP_FEATURE(imageCubeArray);
    SNAP_FEATURE(independentBlend);
    SNAP_FEATURE(geometryShader);
    SNAP_FEATURE(tessellationShader);
    SNAP_FEATURE(sampleRateShading);
    SNAP_FEATURE(dualSrcBlend);
    SNAP_FEATURE(logicOp);
    SNAP_FEATURE(multiDrawIndirect);
    SNAP_FEATURE(drawIndirectFirstInstance);
    SNAP_FEATURE(depthClamp);
    SNAP_FEATURE(depthBiasClamp);
    SNAP_FEATURE(fillModeNonSolid);
    SNAP_FEATURE(depthBounds);
    SNAP_FEATURE(wideLines);
    SNAP_FEATURE(largePoints);
    SNAP_FEATURE(alphaToOne);
    SNAP_FEATURE(multiViewport);
    SNAP_FEATURE(samplerAnisotropy);
    SNAP_FEATURE(textureCompressionETC2);
    SNAP_FEATURE(textureCompressionASTC_LDR);
    SNAP_FEATURE(textureCompressionBC);
    SNAP_FEATURE(occlusionQueryPrecise);
    SNAP_FEATURE(pipelineStatisticsQuery);
    SNAP_FEATURE(vertexPipelineStoresAndAtomics);
    SNAP_FEATURE(fragmentStoresAndAtomics);
    SNAP_FEATURE(shaderTessellationAndGeometryPointSize);
    SNAP_FEATURE(shaderImageGatherExtended);
    SNAP_FEATURE(shaderStorageImageExtendedFormats);
    SNAP_FEATURE(shaderStorageImageMultisample);
    SNAP_FEATURE(shaderStorageImageReadWithoutFormat);
    SNAP_FEATURE(shaderStorageImageWriteWithoutFormat);
    SNAP_FEATURE(shaderUniformBufferArrayDynamicIndexing);
    SNAP_FEATURE(shaderSampledImageArrayDynamicIndexing);
    SNAP_FEATURE(shaderStorageBufferArrayDynamicIndexing);
    SNAP_FEATURE(shaderStorageImageArrayDynamicIndexing);
    SNAP_FEATURE(shaderClipDistance);
    SNAP_FEATURE(shaderCullDistance);
    SNAP_FEATURE(shaderFloat64);
    SNAP_FEATURE(shaderInt64);
    SNAP_FEATURE(shaderInt16);
    SNAP_FEATURE(shaderResourceResidency);
    SNAP_FEATURE(shaderResourceMinLod);
    SNAP_FEATURE(sparseBinding);
    SNAP_FEATURE(sparseResidencyBuffer);
    SNAP_FEATURE(sparseResidencyImage2D);
    SNAP_FEATURE(sparseResidencyImage3D);
    SNAP_FEATURE(sparseResidency2Samples);
    SNAP_FEATURE(sparseResidency4Samples);
    SNAP_FEATURE(sparseResidency8Samples);
    SNAP_FEATURE(sparseResidency16Samples);
    SNAP_FEATURE(sparseResidencyAliased);
    SNAP_FEATURE(variableMultisampleRate);
    SNAP_FEATURE(inheritedQueries);
#undef SNAP_FEATURE
    root["coreFeatures"] = SnapshotMap(core);
    root["scalarBlockLayout"] = SnapshotMap({{"feature", SnapshotFeature(scalarQueryable, scalar.scalarBlockLayout, device->snapshotScalarRequested, created)},
        {"physicalCorePromoted", SnapshotBool(properties.apiVersion >= VK_API_VERSION_1_2)},
        {"effectiveCorePromoted", SnapshotBool(deviceCore12)},
        {"extensionAvailable", SnapshotBool(SnapshotHas(device, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME))},
        {"extensionEnabled", SnapshotBool(std::binary_search(device->snapshotEnabledExtensions.begin(), device->snapshotEnabledExtensions.end(), std::string(VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME)))},
        {"queryRoute", SnapshotString(scalarQueryable ? featuresRoute : "UNAVAILABLE")},
        {"negotiationPolicy", SnapshotString("SCALAR_LAYOUT_NEGOTIATION: existing extension-name-gated unchecked VK_TRUE request; promoted core path not requested")}});
    root["relaxedBlockLayout"] = SnapshotMap({{"extensionAvailable", SnapshotBool(SnapshotHas(device, VK_KHR_RELAXED_BLOCK_LAYOUT_EXTENSION_NAME))},
        {"extensionEnabled", SnapshotBool(std::binary_search(device->snapshotEnabledExtensions.begin(), device->snapshotEnabledExtensions.end(), std::string(VK_KHR_RELAXED_BLOCK_LAYOUT_EXTENSION_NAME)))},
        {"physicalCorePromoted", SnapshotBool(properties.apiVersion >= VK_API_VERSION_1_1)},
        {"effectiveCorePromoted", SnapshotBool(deviceCore11)}, {"featureBit", "null"},
        {"featureBitReason", SnapshotString("Relaxed block layout has no feature bit; usable through effective device API >=1.1 or enabled extension")}});
    root["customBorderColor"] = SnapshotMap({{"customBorderColors", SnapshotFeature(borderQueryable, border.customBorderColors, device->snapshotCustomRequested, created)},
        {"customBorderColorWithoutFormat", SnapshotFeature(borderQueryable, border.customBorderColorWithoutFormat, device->snapshotCustomWithoutFormatRequested, created)},
        {"extensionAvailable", SnapshotBool(SnapshotHas(device, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME))},
        {"extensionEnabled", SnapshotBool(device->customBorderColorSupported && created)},
        {"queryRoute", SnapshotString(borderQueryable ? featuresRoute + "/" + propertiesRoute : "UNAVAILABLE")},
        {"maxCustomBorderColorSamplers", properties2 && borderQueryable ? SnapshotNumber(borderProperties.maxCustomBorderColorSamplers) : "null"},
        {"backendCustomBranch", SnapshotBool(device->customBorderColorSupported)},
        {"fallback", SnapshotString("Standard transparent black/opaque black/opaque white use standard borders; other colors become INT_TRANSPARENT_BLACK when backend custom branch is unavailable; otherwise FLOAT_CUSTOM with UNDEFINED format. Feature bits are reported separately; branch eligibility does not prove feature enabled.")}});
    SnapshotObject cap;
#define SNAP_LIMIT(name) cap[#name] = SnapshotNumber(limits.name)
    SNAP_LIMIT(maxImageDimension2D); SNAP_LIMIT(maxImageArrayLayers); SNAP_LIMIT(maxFramebufferWidth);
    SNAP_LIMIT(maxFramebufferHeight); SNAP_LIMIT(maxColorAttachments); SNAP_LIMIT(maxVertexInputAttributes);
    SNAP_LIMIT(maxVertexInputBindings); SNAP_LIMIT(maxVertexInputAttributeOffset); SNAP_LIMIT(maxVertexInputBindingStride);
    SNAP_LIMIT(maxVertexOutputComponents); SNAP_LIMIT(maxFragmentInputComponents); SNAP_LIMIT(maxBoundDescriptorSets);
    SNAP_LIMIT(maxPerStageDescriptorSampledImages); SNAP_LIMIT(maxPerStageDescriptorSamplers); SNAP_LIMIT(maxPerStageDescriptorUniformBuffers);
    SNAP_LIMIT(maxPerStageResources); SNAP_LIMIT(maxDescriptorSetSampledImages); SNAP_LIMIT(maxDescriptorSetSamplers);
    SNAP_LIMIT(maxDescriptorSetUniformBuffersDynamic); SNAP_LIMIT(maxUniformBufferRange); SNAP_LIMIT(minUniformBufferOffsetAlignment);
    SNAP_LIMIT(nonCoherentAtomSize); SNAP_LIMIT(maxMemoryAllocationCount); SNAP_LIMIT(maxSamplerAllocationCount);
#undef SNAP_LIMIT
    cap["maxViewportDimensions"] = SnapshotArray({SnapshotNumber(limits.maxViewportDimensions[0]), SnapshotNumber(limits.maxViewportDimensions[1])});
    cap["viewportBoundsRange"] = SnapshotArray({SnapshotNumber(limits.viewportBoundsRange[0]), SnapshotNumber(limits.viewportBoundsRange[1])});
    cap["perInstanceInputRate"] = SnapshotString("VK_VERTEX_INPUT_RATE_INSTANCE; divisor 1; selected for a binding when any element has InstanceDataStepRate > 0");
    root["limits"] = SnapshotMap(cap);
    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProperties(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queueProperties.data());
    std::vector<std::string> queues;
    for (uint32_t i = 0; i < queueCount; ++i)
    {
        VkBool32 present = VK_FALSE;
        const auto result = device->surface ? vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, device->surface, &present) : VK_NOT_READY;
        queues.push_back(SnapshotMap({{"family", SnapshotNumber(i)}, {"flags", SnapshotNumber(queueProperties[i].queueFlags)},
            {"queueCount", SnapshotNumber(queueProperties[i].queueCount)}, {"selected", SnapshotBool(i == device->graphicsQueueFamily)},
            {"presentQueryResult", SnapshotNumber(result)}, {"present", result == VK_SUCCESS ? SnapshotBool(present) : "null"}}));
    }
    root["queues"] = SnapshotArray(queues);
    root["selectedQueueFamily"] = SnapshotNumber(device->graphicsQueueFamily);
    root["surfaceAvailable"] = SnapshotBool(device->surface != VK_NULL_HANDLE);
    std::vector<std::string> backbuffers;
    for (size_t i = 0; i < device->swapchains.size(); ++i)
    {
        const auto* texture = device->swapchains[i].texture;
        if (!texture) { continue; }
        const auto* depth = texture->depthTexture;
        backbuffers.push_back(SnapshotMap({{"index", SnapshotNumber(i)},
            {"colorFormat", SnapshotNumber(texture->info.format)},
            {"depthFormat", depth ? SnapshotNumber(depth->info.format) : "null"},
            {"width", SnapshotNumber(texture->info.extent.width)}, {"height", SnapshotNumber(texture->info.extent.height)},
            {"colorUsage", SnapshotNumber(texture->info.usage)},
            {"depthUsage", depth ? SnapshotNumber(depth->info.usage) : "null"},
            {"sampleCount", SnapshotNumber(texture->info.samples)}}));
    }
    root["actualResources"] = SnapshotMap({{"swapchainAvailable", SnapshotBool(device->swapchain != VK_NULL_HANDLE)},
        {"requestedNativeColorFormat", SnapshotNumber(device->colorFormat)},
        {"requestedNativeDepthFormat", SnapshotNumber(device->depthFormat)},
        {"backbuffers", SnapshotArray(backbuffers)},
        {"offscreenObservation", SnapshotMap({{"schemaVersion", "1"},
            {"source", SnapshotString("Retained successful VkImageCreateInfo scalars; live offscreen parents only; queued destruction excluded")},
            {"ownership", SnapshotString("Caller serializes all device operations on owning graphics thread; device mutex guards lifetime only")},
            {"requestedEnums", SnapshotString("Retained native creation arguments after managed validation; original managed inputs must be recorded by caller; actual image metadata is separate")},
            {"pendingTextureDestructionCount", SnapshotNumber(device->destroyTextures.size())},
            {"status", SnapshotString("OBSERVED_LIVE_SET; offscreenTargets [] means observed empty")},
            {"renderingProof", "false"}})},
        {"offscreenTargets", SnapshotOffscreenTargets(device)},
        {"representativeOffscreenResources", "null"},
        {"representativeOffscreenStatus", SnapshotString("UNAVAILABLE: generic offscreenTargets require consumer role mapping and do not establish representative rendering; imageQueries remain support and selection predictions")}});

    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
    VkPhysicalDeviceMemoryProperties2 memory2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 };
    const bool budgetQueryable = queryMemory2 && SnapshotHas(device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (budgetQueryable) { memory2.pNext = &budget; queryMemory2(physical, &memory2); }
    std::vector<std::string> heaps, types;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
    {
        heaps.push_back(SnapshotMap({{"index", SnapshotNumber(i)}, {"size", SnapshotNumber(memory.memoryHeaps[i].size)},
            {"flags", SnapshotNumber(memory.memoryHeaps[i].flags)}, {"budget", budgetQueryable ? SnapshotNumber(budget.heapBudget[i]) : "null"},
            {"usage", budgetQueryable ? SnapshotNumber(budget.heapUsage[i]) : "null"}}));
    }
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    {
        types.push_back(SnapshotMap({{"index", SnapshotNumber(i)}, {"heapIndex", SnapshotNumber(memory.memoryTypes[i].heapIndex)},
            {"propertyFlags", SnapshotNumber(memory.memoryTypes[i].propertyFlags)}}));
    }
    root["memory"] = SnapshotMap({{"heaps", SnapshotArray(heaps)}, {"types", SnapshotArray(types)},
        {"budgetQueried", SnapshotBool(budgetQueryable)}, {"budgetExtensionAvailable", SnapshotBool(SnapshotHas(device, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))}, {"actualAllocationBytes", "null"}, {"allocationStatus", SnapshotString("UNMEASURED")}});
    std::vector<std::string> formats;
    const VkFormat selectedFormats[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    for (auto format : selectedFormats)
    {
        VkFormatProperties fp = {};
        vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
        formats.push_back(SnapshotMap({{"format", SnapshotNumber(format)}, {"linearTilingFeatures", SnapshotNumber(fp.linearTilingFeatures)},
            {"optimalTilingFeatures", SnapshotNumber(fp.optimalTilingFeatures)}, {"bufferFeatures", SnapshotNumber(fp.bufferFeatures)},
            {"sampled", SnapshotBool(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)},
            {"linearFilter", SnapshotBool(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)},
            {"colorAttachment", SnapshotBool(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)},
            {"colorBlend", SnapshotBool(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)},
            {"depthStencilAttachment", SnapshotBool(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)},
            {"transferSource", SnapshotBool(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)},
            {"transferDestination", SnapshotBool(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)},
            {"vertexBuffer", SnapshotBool(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)}}));
    }
    root["formats"] = SnapshotArray(formats);
    std::vector<std::string> images;
    auto image = [&](const char* role, VkFormat requested, uint32_t usage, uint32_t width, uint32_t height, uint32_t layers, bool depth)
    {
        const auto actual = depth ? MGVK_PickSupportedDepthFormat(device, requested, width, height, 1) : requested;
        VkImageFormatProperties ip = {};
        const auto result = actual == VK_FORMAT_UNDEFINED ? VK_ERROR_FORMAT_NOT_SUPPORTED :
            vkGetPhysicalDeviceImageFormatProperties(physical, actual, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &ip);
        const bool fits = result == VK_SUCCESS && width <= ip.maxExtent.width && height <= ip.maxExtent.height && layers <= ip.maxArrayLayers && (ip.sampleCounts & VK_SAMPLE_COUNT_1_BIT);
        images.push_back(SnapshotMap({{"role", SnapshotString(role)}, {"requestedFormat", SnapshotNumber(requested)},
            {"selectedFormat", SnapshotNumber(actual)}, {"selectionKind", SnapshotString("Read-only backend selection prediction; no image allocated")},
            {"width", SnapshotNumber(width)}, {"height", SnapshotNumber(height)}, {"layers", SnapshotNumber(layers)},
            {"imageType", "1"}, {"tiling", "0"}, {"flags", "0"}, {"samples", "1"}, {"usage", SnapshotNumber(usage)},
            {"queryResult", SnapshotNumber(result)}, {"supported", SnapshotBool(fits)},
            {"maxExtent", result == VK_SUCCESS ? SnapshotArray({SnapshotNumber(ip.maxExtent.width), SnapshotNumber(ip.maxExtent.height), SnapshotNumber(ip.maxExtent.depth)}) : "null"},
            {"maxArrayLayers", result == VK_SUCCESS ? SnapshotNumber(ip.maxArrayLayers) : "null"},
            {"maxMipLevels", result == VK_SUCCESS ? SnapshotNumber(ip.maxMipLevels) : "null"},
            {"sampleCounts", result == VK_SUCCESS ? SnapshotNumber(ip.sampleCounts) : "null"},
            {"maxResourceSize", result == VK_SUCCESS ? SnapshotNumber(ip.maxResourceSize) : "null"}}));
    };
    const uint32_t sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image("ColorArray", ToVkFormat(MGSurfaceFormat::Color), sampled, 128, 128, 3, false);
    image("ColorArray", ToVkFormat(MGSurfaceFormat::Color), sampled, 128, 128, 2, false);
    image("ColorArray", ToVkFormat(MGSurfaceFormat::Color), sampled, 256, 256, 2, false);
    for (auto extent : {std::pair<uint32_t,uint32_t>{1280,720}, {960,540}, {320,180}, {240,96}})
    {
        image("ColorTarget", ToVkFormat(MGSurfaceFormat::Color), sampled | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, extent.first, extent.second, 1, false);
    }
    image("SingleShadowColor", ToVkFormat(MGSurfaceFormat::Single), sampled | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 4096, 4096, 1, false);
    for (auto extent : {std::pair<uint32_t,uint32_t>{4096,4096}, {1280,720}, {960,540}})
    {
        image("Depth24", ToVkFormat(MGDepthFormat::Depth24), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, extent.first, extent.second, 1, true);
        image("Depth24Candidate", VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, extent.first, extent.second, 1, false);
        image("Depth32StencilCandidate", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, extent.first, extent.second, 1, false);
    }
    root["imageQueries"] = SnapshotArray(images);
    return SnapshotMap(root);
}
}

int32_t MGG_GraphicsDevice_QueryVulkanSnapshot(MGG_GraphicsDevice* device, uint32_t requestBytes,
    MGG_VulkanSnapshotRequest* request, char* utf8)
{
    if (!request || requestBytes < sizeof(MGG_VulkanSnapshotRequest)) { return -1; }
    request->RequiredBytes = 0;
    if (request->Size != sizeof(MGG_VulkanSnapshotRequest) || requestBytes != request->Size) { return -1; }
    if (request->Version != 1) { return -2; }
    if ((request->BufferBytes != 0 && !utf8) || (request->BufferBytes == 0 && utf8)) { return -1; }
    try
    {
        std::lock_guard<std::mutex> lock(MGVK_SnapshotMutex);
        if (!device || MGVK_SnapshotDevices.count(device) == 0) { return -3; }
        const auto snapshot = MGVK_MakeSnapshot(device);
        if (snapshot.size() >= UINT32_MAX) { return -1; }
        request->RequiredBytes = (uint32_t)snapshot.size() + 1;
        if (request->BufferBytes < request->RequiredBytes) { return 1; }
        memcpy(utf8, snapshot.c_str(), request->RequiredBytes);
        return 0;
    }
    catch (...)
    {
        request->RequiredBytes = 0;
        return -4;
    }
}

#endif
