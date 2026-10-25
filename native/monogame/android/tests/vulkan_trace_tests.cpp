// Actual production helpers with fake dispatch. No Vulkan loader/window/GPU opens.
#include <mutex>
#include <atomic>
#include <cstdlib>
#define __ANDROID__ 1
#define DEBUG 1
#define MGG_ANDROID_TRACE_HOST_TEST 1
#ifndef MGG_TRACE_RENDERER_SOURCE
#define MGG_TRACE_RENDERER_SOURCE "../../vulkan/MGG_Vulkan.cpp"
#endif
#include MGG_TRACE_RENDERER_SOURCE
#include "api_MGG_AndroidTrace.h"
#undef main
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <sstream>

static std::atomic<bool> failClock{false};
static std::atomic<uint64_t> nativeClockCalls{0};
extern "C" int __real_clock_gettime(clockid_t,timespec*);
extern "C" int __wrap_clock_gettime(clockid_t id,timespec* value)
{
    nativeClockCalls.fetch_add(1,std::memory_order_relaxed);
    if (failClock.load(std::memory_order_relaxed)) { errno = EIO; return -1; }
    return __real_clock_gettime(id,value);
}
static std::atomic<uint64_t> allocationCalls{0};
void* operator new(size_t size)
{
    allocationCalls.fetch_add(1, std::memory_order_relaxed);
    if (void* result = malloc(size ? size : 1)) { return result; }
    throw std::bad_alloc();
}
void* operator new(size_t size, const std::nothrow_t&) noexcept
{ try { return ::operator new(size); } catch (...) { return nullptr; } }
void operator delete(void* value, const std::nothrow_t&) noexcept { free(value); }
void operator delete(void* value) noexcept { free(value); }
void operator delete(void* value, size_t) noexcept { free(value); }

static void Require(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}
static std::vector<std::string> calls;
static uint64_t nextHandle = 1000;
static VkResult acquireResult = VK_SUCCESS, submitResult = VK_SUCCESS, presentResult = VK_SUCCESS;
static VkResult waitResult = VK_SUCCESS, endResult = VK_SUCCESS, createResult = VK_SUCCESS;
static VkResult resetResult = VK_SUCCESS, statusResult = VK_SUCCESS;
static bool recordCalls = true;
template<class T> static T Handle(uint64_t value) { return (T)(uintptr_t)value; }
static void Call(const std::string& name)
{
    if (recordCalls) { calls.push_back(name); }
}
static VKAPI_ATTR VkResult VKAPI_CALL Acquire(VkDevice, VkSwapchainKHR swap, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t* index)
{
    Call("acquire:" + std::to_string((uintptr_t)swap) + ":" + std::to_string(timeout) + ":" +
        std::to_string((uintptr_t)semaphore) + ":" + std::to_string((uintptr_t)fence));
    *index = acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR ? 0 : 999;
    return acquireResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL End(VkCommandBuffer command)
{
    Call("end:" + std::to_string((uintptr_t)command)); return endResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL ResetCommand(VkCommandBuffer command, VkCommandBufferResetFlags flags)
{
    Call("reset-command:" + std::to_string((uintptr_t)command) + ":" + std::to_string(flags)); return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL Begin(VkCommandBuffer command, const VkCommandBufferBeginInfo* info)
{
    Call("begin:" + std::to_string((uintptr_t)command) + ":" + std::to_string(info->flags)); return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* command)
{
    *command = Handle<VkCommandBuffer>(++nextHandle);
    Call("allocate:" + std::to_string((uintptr_t)info->commandPool)); return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL FreeCommand(VkDevice, VkCommandPool pool, uint32_t count, const VkCommandBuffer*)
{
    Call("free-command:" + std::to_string((uintptr_t)pool) + ":" + std::to_string(count));
}
static VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice, const VkFenceCreateInfo* info,
    const VkAllocationCallbacks*, VkFence* fence)
{
    if (createResult == VK_SUCCESS) { *fence = Handle<VkFence>(777); }
    Call("create-fence:" + std::to_string(info->flags)); return createResult;
}
static VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*)
{
    Call("destroy-fence:" + std::to_string((uintptr_t)fence));
}
static VKAPI_ATTR VkResult VKAPI_CALL ResetFence(VkDevice, uint32_t count, const VkFence* fence)
{
    Call("reset-fence:" + std::to_string(count) + ":" + std::to_string((uintptr_t)*fence)); return resetResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice, uint32_t count, const VkFence* fence, VkBool32 all, uint64_t timeout)
{
    Call("wait:" + std::to_string(count) + ":" + std::to_string((uintptr_t)*fence) + ":" +
        std::to_string(all) + ":" + std::to_string(timeout)); return waitResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL Status(VkDevice, VkFence fence)
{
    Call("status:" + std::to_string((uintptr_t)fence)); return statusResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
{
    std::ostringstream out;
    out << "submit:" << (uintptr_t)queue << ':' << count << ':' << info->waitSemaphoreCount << ':'
        << (info->waitSemaphoreCount ? (uintptr_t)*info->pWaitSemaphores : 0) << ':'
        << (info->waitSemaphoreCount ? *info->pWaitDstStageMask : 0) << ':' << info->commandBufferCount << ':'
        << (uintptr_t)*info->pCommandBuffers << ':' << info->signalSemaphoreCount << ':'
        << (info->signalSemaphoreCount ? (uintptr_t)*info->pSignalSemaphores : 0) << ':' << (uintptr_t)fence;
    Call(out.str()); return submitResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue queue, const VkPresentInfoKHR* info)
{
    Call("present:" + std::to_string((uintptr_t)queue) + ":" + std::to_string(info->waitSemaphoreCount) + ":" +
        std::to_string((uintptr_t)*info->pWaitSemaphores) + ":" + std::to_string(info->swapchainCount) + ":" +
        std::to_string((uintptr_t)*info->pSwapchains) + ":" + std::to_string(*info->pImageIndices));
    return presentResult;
}
static VKAPI_ATTR VkResult VKAPI_CALL DeviceIdle(VkDevice) { Call("device-idle"); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL QueueIdle(VkQueue) { Call("queue-idle"); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL ResetPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags flags)
{ Call("reset-pool:" + std::to_string(flags)); return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL CreateView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* view)
{ *view = Handle<VkImageView>(++nextHandle); return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL DestroyView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
static VKAPI_ATTR void VKAPI_CALL DestroySwap(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) {}
static VKAPI_ATTR void VKAPI_CALL DestroySurface(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*) {}
static VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
static VKAPI_ATTR void VKAPI_CALL Barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
    VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*) {}
static VKAPI_ATTR void VKAPI_CALL Blit(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter) {}
static VKAPI_ATTR void VKAPI_CALL CopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*) {}
static VKAPI_ATTR void VKAPI_CALL CopyToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*) {}
static VKAPI_ATTR void VKAPI_CALL EndPass(VkCommandBuffer) {}

// GPU allocation is a fake dispatch too. The production helper bodies remain intact.
struct Allocation { std::vector<uint8_t> Bytes; explicit Allocation(size_t count) : Bytes(count, 0x42) {} };
static VkResult AllocateMemory(size_t size, VmaAllocation* allocation, VmaAllocationInfo* info)
{
    auto* value = new Allocation(size); *allocation = reinterpret_cast<VmaAllocation>(value);
    if (info) { *info = {}; info->pMappedData = value->Bytes.data(); info->size = size; }
    return VK_SUCCESS;
}
VkResult vmaCreateBuffer(VmaAllocator, const VkBufferCreateInfo* info, const VmaAllocationCreateInfo*, VkBuffer* buffer,
    VmaAllocation* allocation, VmaAllocationInfo* result)
{ *buffer = Handle<VkBuffer>(++nextHandle); return AllocateMemory(info->size, allocation, result); }
VkResult vmaCreateImage(VmaAllocator, const VkImageCreateInfo* info, const VmaAllocationCreateInfo*, VkImage* image,
    VmaAllocation* allocation, VmaAllocationInfo* result)
{ *image = Handle<VkImage>(++nextHandle); return AllocateMemory(info->extent.width * info->extent.height * 4, allocation, result); }
void vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation allocation) { delete reinterpret_cast<Allocation*>(allocation); }
void vmaDestroyImage(VmaAllocator, VkImage, VmaAllocation allocation) { delete reinterpret_cast<Allocation*>(allocation); }
void vmaFreeMemory(VmaAllocator, VmaAllocation allocation) { delete reinterpret_cast<Allocation*>(allocation); }
VkResult vmaMapMemory(VmaAllocator, VmaAllocation allocation, void** data)
{ *data = reinterpret_cast<Allocation*>(allocation)->Bytes.data(); return VK_SUCCESS; }
void vmaUnmapMemory(VmaAllocator, VmaAllocation) {}
VkResult vmaInvalidateAllocation(VmaAllocator, VmaAllocation, VkDeviceSize, VkDeviceSize) { return VK_SUCCESS; }
VkResult vmaFlushAllocation(VmaAllocator, VmaAllocation, VkDeviceSize, VkDeviceSize) { return VK_SUCCESS; }

static void Install()
{
    vkAcquireNextImageKHR = Acquire; vkEndCommandBuffer = End; vkBeginCommandBuffer = Begin;
    vkResetCommandBuffer = ResetCommand; vkAllocateCommandBuffers = Allocate; vkFreeCommandBuffers = FreeCommand;
    vkCreateFence = CreateFence; vkDestroyFence = DestroyFence; vkWaitForFences = Wait;
    vkResetFences = ResetFence; vkGetFenceStatus = Status; vkQueueSubmit = Submit; vkQueuePresentKHR = Present;
    vkDeviceWaitIdle = DeviceIdle; vkQueueWaitIdle = QueueIdle; vkResetCommandPool = ResetPool;
    vkCreateImageView = CreateView; vkDestroyImageView = DestroyView; vkDestroySwapchainKHR = DestroySwap;
    vkDestroySurfaceKHR = DestroySurface; vkDestroySemaphore = DestroySemaphore; vkCmdPipelineBarrier = Barrier;
    vkCmdBlitImage = Blit; vkCmdCopyImageToBuffer = CopyImage; vkCmdCopyBufferToImage = CopyToImage;
    vkCmdEndRenderPass = EndPass; vkSetDebugUtilsObjectNameEXT = nullptr;
}
struct Fixture
{
    MGG_GraphicsDevice Device;
    MGG_Buffer Uniform;
    MGG_Texture Image[2];
    Fixture()
    {
        Device.device = Handle<VkDevice>(1); Device.queue = Handle<VkQueue>(2);
        Device.cmdPool = Handle<VkCommandPool>(3); Device.transferPool = Handle<VkCommandPool>(4);
        Device.swapchain = Handle<VkSwapchainKHR>(5); Device.swapchainCount = 2;
        Device.androidNeedsSwapchain = false; Device.androidSurface.SessionId = 1;
        MGP_Android_Lifecycle lifecycle{};
        Require(MGP_Android_GetLifecycle(1, &lifecycle) == 0, "native session");
        Device.androidSurface.Generation = lifecycle.Generation;
        Device.androidSurface.RetiredRevision = lifecycle.RetireRevision;
        Device.frames.resize(2); Device.swapchains.resize(2);
        for (int i = 0; i < 2; ++i)
        {
            auto& frame = Device.frames[i]; frame.commandBuffer = Handle<VkCommandBuffer>(10 + i);
            frame.completedFence = Handle<VkFence>(20 + i); frame.imageAcquiredSemaphore = Handle<VkSemaphore>(30 + i);
            frame.uniforms = &Uniform; Device.swapchains[i].renderCompleteSemaphore = Handle<VkSemaphore>(40 + i);
            Device.swapchains[i].texture = &Image[i]; Image[i].image = Handle<VkImage>(50 + i);
            Image[i].info.extent = {4,4,1}; Image[i].info.mipLevels = 1; Image[i].info.arrayLayers = 1;
            Image[i].layouts[0] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
#ifndef MGG_TRACE_BASELINE
        MGVK_TraceRegisterDevice(&Device);
        std::lock_guard<std::mutex> lock(MGVK_SnapshotMutex); MGVK_SnapshotDevices.insert(&Device);
#endif
        acquireResult = submitResult = presentResult = waitResult = endResult = createResult = resetResult = statusResult = VK_SUCCESS;
        nextHandle = 1000; calls.clear();
    }
    ~Fixture()
    {
#ifndef MGG_TRACE_BASELINE
        std::lock_guard<std::mutex> lock(MGVK_SnapshotMutex);
        MGVK_TraceSealDevice(&Device); MGVK_SnapshotDevices.erase(&Device);
#endif
        for (auto& pending : Device.pendingTransfers) { vmaDestroyBuffer(nullptr, pending.staging.buffer, pending.staging.allocation); }
    }
};

#ifndef MGG_TRACE_BASELINE
template<class T> static T Abi() { T value{}; value.Size = sizeof(T); value.Version = 1; return value; }
static MGG_AndroidTrace_Context Context()
{
    auto context = Abi<MGG_AndroidTrace_Context>(); Require(MGG_AndroidTrace_GetCurrentContext(&context) == 0, "current context"); return context;
}
static MGG_AndroidTrace_Control Enable()
{
    auto request = Abi<MGG_AndroidTrace_EnableRequest>(); request.Context = Context();
    request.Diagnostic = 1; request.CallerRunNonceLo = 123;
    auto control = Abi<MGG_AndroidTrace_Control>();
    Require(MGG_AndroidTrace_Enable(&request, &control) == 0, "enable"); return control;
}
static std::vector<MGG_AndroidTrace_Event> Read(uint64_t epoch, int expected = 0)
{
    auto request = Abi<MGG_AndroidTrace_ReadRequest>(); request.TraceEpoch = epoch;
    request.EventBytes = sizeof(MGG_AndroidTrace_Event); request.Capacity = 256;
    std::vector<MGG_AndroidTrace_Event> events;
    while (true)
    {
        auto result = Abi<MGG_AndroidTrace_ReadResult>(); MGG_AndroidTrace_Event page[256];
        Require(MGG_AndroidTrace_Read(&request, &result, page) == expected, "read status");
        for (uint32_t i = 0; i < result.Copied; ++i) { events.push_back(page[i]); }
        if (!result.Copied) { break; }
        request.AfterSequence = events.back().Sequence;
    }
    for (size_t i = 0; i < events.size(); ++i)
    {
        Require(events[i].Size == 320 && events[i].Version == 1 && events[i].Sequence == i + 1 &&
            events[i].TraceEpoch == epoch && events[i].MonotonicNs != 0 && events[i].NativeTid != 0, "complete sequence");
    }
    return events;
}
static const MGG_AndroidTrace_Event* Last(const std::vector<MGG_AndroidTrace_Event>& events, uint32_t kind, uint32_t work = 0)
{
    for (auto i = events.rbegin(); i != events.rend(); ++i)
    { if (i->EventKind == kind && (!work || i->WorkKind == work)) { return &*i; } }
    return nullptr;
}
static bool ExactJoin(const MGG_AndroidTrace_Event& a, const MGG_AndroidTrace_Event& b)
{
    return a.TraceEpoch && a.TraceEpoch == b.TraceEpoch && a.DeviceId == b.DeviceId && a.SessionId == b.SessionId &&
        a.HostSurfaceGeneration == b.HostSurfaceGeneration && a.AppliedSurfaceGeneration == b.AppliedSurfaceGeneration &&
        a.SwapchainGeneration == b.SwapchainGeneration && a.NativeSurfaceInstanceId == b.NativeSurfaceInstanceId &&
        a.QueueId == b.QueueId && a.LifecycleRevision == b.LifecycleRevision && a.RetireRevision == b.RetireRevision &&
        (a.WorkKind != b.WorkKind || (a.CallerRequestIdLo == b.CallerRequestIdLo && a.CallerRequestIdHi == b.CallerRequestIdHi)) &&
        !(a.Flags & MGG_TRACE_PARTIAL_AT_ENABLE) && !(b.Flags & MGG_TRACE_PARTIAL_AT_ENABLE) && a.FrameSequence && a.FrameSequence == b.FrameSequence &&
        a.AcquireAttemptId && a.AcquireAttemptId == b.AcquireAttemptId && a.ImageIndex == b.ImageIndex &&
        (a.Flags & MGG_TRACE_IMAGE_VALID) && (b.Flags & MGG_TRACE_IMAGE_VALID);
}
// Test-only schema joins over actual produced events; future R37 consumer is not implemented here.
static bool Recovery(const std::vector<MGG_AndroidTrace_Event>& events, int invalid = 0)
{
    if (events.empty() || invalid) { return false; }
    for (size_t i = 0; i < events.size(); ++i)
    { if (events[i].Sequence != i + 1 || events[i].TraceEpoch != events[0].TraceEpoch) { return false; } }
    const auto* flush = Last(events, MGG_TRACE_SUBMIT_RESULT, MGG_TRACE_READBACK_RENDER_FLUSH);
    const auto* copy = Last(events, MGG_TRACE_SUBMIT_RESULT, MGG_TRACE_READBACK_COPY);
    const auto* final = Last(events, MGG_TRACE_SUBMIT_RESULT, MGG_TRACE_PRESENT_FINAL);
    const auto* present = Last(events, MGG_TRACE_QUEUE_PRESENT_RESULT, MGG_TRACE_PRESENT_FINAL);
    if (!flush || !copy || !final || !present || !ExactJoin(*flush,*copy) || !ExactJoin(*copy,*final) || !ExactJoin(*final,*present) ||
        flush->Sequence >= copy->Sequence || copy->Sequence >= final->Sequence || final->Sequence >= present->Sequence ||
        !flush->ParentOperationId || flush->ParentOperationId != copy->ParentOperationId ||
        present->SubmitId != final->SubmitId || present->FenceInstanceId != final->FenceInstanceId ||
        present->FencePayloadId != final->FencePayloadId || present->CommandBufferId != final->CommandBufferId ||
        present->CommandRecordingId != final->CommandRecordingId) { return false; }
    for (const auto* submit : {flush,copy,final})
    {
        if (!(submit->Flags & MGG_TRACE_RESULT_VALID) || submit->Result != VK_SUCCESS || !submit->FenceInstanceId || !submit->FencePayloadId) { return false; }
        bool retired = false;
        for (const auto& event : events)
        {
            if (event.EventKind == MGG_TRACE_FENCE_WAIT_RESULT && event.SubmitId == submit->SubmitId &&
                event.FenceInstanceId == submit->FenceInstanceId && event.FencePayloadId == submit->FencePayloadId &&
                event.WorkKind == submit->WorkKind && event.Sequence > submit->Sequence &&
                (event.Flags & MGG_TRACE_RESULT_VALID) && event.Result == VK_SUCCESS && ExactJoin(event,*submit)) { retired = true; }
        }
        if (!retired) { return false; }
    }
    return (present->Flags & MGG_TRACE_RESULT_VALID) && present->Result == VK_SUCCESS;
}
struct AbortSink { uint32_t Count; MGG_AndroidTrace_Event Events[128]; };
static AbortSink* abortSink = nullptr;
extern "C" void __real___assert_fail(const char*,const char*,unsigned int,const char*) __attribute__((noreturn));
extern "C" void __wrap___assert_fail(const char* expression,const char* file,unsigned int line,const char* function)
{
    if (abortSink && MGVK_TraceStorageOwner)
    {
        std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
        auto& state = *MGVK_TraceStorageOwner; abortSink->Count = std::min(state.Count, uint32_t(128));
        for (uint32_t i = 0; i < abortSink->Count; ++i) { abortSink->Events[i] = state.Events[i]; }
    }
    __real___assert_fail(expression,file,line,function);
}
static void Fatal(int failure, uint32_t expectedKind)
{
    abortSink = static_cast<AbortSink*>(mmap(nullptr,sizeof(AbortSink),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0));
    Require(abortSink != MAP_FAILED,"mmap"); abortSink->Count = 0;
    pid_t child = fork(); Require(child >= 0,"fork");
    if (!child)
    {
        Fixture f; auto control = Enable(); (void)control;
        auto& frame = f.Device.frames[0]; Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == 0,"fatal begin");
        if (failure == 0) { submitResult = VK_ERROR_DEVICE_LOST; MGG_GraphicsDevice_Present(&f.Device,0,0); }
        if (failure == 1) { presentResult = VK_ERROR_DEVICE_LOST; MGG_GraphicsDevice_Present(&f.Device,0,0); }
        if (failure == 2) { waitResult = VK_ERROR_DEVICE_LOST; frame.is_rendering = true; MGVK_TryAcquireSwap(&f.Device,frame); }
        if (failure == 3) { endResult = VK_ERROR_DEVICE_LOST; MGVK_FlushCommands(&f.Device,frame); }
        if (failure == 4) { createResult = VK_ERROR_OUT_OF_HOST_MEMORY; MGVK_FlushCommands(&f.Device,frame); }
        if (failure == 5) { resetResult = VK_ERROR_DEVICE_LOST; MGG_GraphicsDevice_Present(&f.Device,0,0); }
        if (failure >= 6 && failure <= 8)
        {
            auto command = MGVK_BeginNewCommandBuffer(&f.Device);
            if (failure == 6) { submitResult = VK_ERROR_DEVICE_LOST; }
            if (failure == 7) { waitResult = VK_ERROR_DEVICE_LOST; }
            if (failure == 8) { endResult = VK_ERROR_DEVICE_LOST; }
            MGVK_ExecuteAndFreeCommandBuffer(&f.Device,command);
        }
        if (failure == 9) { submitResult = VK_ERROR_DEVICE_LOST; MGVK_FlushCommands(&f.Device,frame); }
        if (failure == 10) { waitResult = VK_ERROR_DEVICE_LOST; MGVK_FlushCommands(&f.Device,frame); }
        _exit(99);
    }
    int status = 0; Require(waitpid(child,&status,0) == child,"waitpid");
    Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,"original fatal abort");
    bool found = false;
    for (uint32_t i = 0; i < abortSink->Count; ++i)
    {
        auto& event = abortSink->Events[i];
        if (event.EventKind == expectedKind && (event.Flags & MGG_TRACE_RESULT_VALID) && event.Result < 0) { found = true; }
    }
    Require(found,"fatal result recorded before original assertion");
    std::cout << "EXPECTED_FATAL case=" << failure << " signal=" << WTERMSIG(status) << " committedSinkEvents=" << abortSink->Count << " exactFailureEvent=verified\n";
    munmap(abortSink,sizeof(AbortSink)); abortSink = nullptr;
}
static std::atomic<int> controlBarrierStage{0}, controlBarrierReached{0}, disableAttempted{0}, disableCompleted{0};
static void ControlBarrier(uint32_t stage)
{
    if (stage != uint32_t(controlBarrierStage.load())) { return; }
    controlBarrierReached = stage;
    if (stage <= 2)
    {
        while (!disableCompleted.load()) { std::this_thread::yield(); }
    }
    else
    {
        while (!disableAttempted.load()) { std::this_thread::yield(); }
    }
}
static void ControlDisableRaces()
{
    for (bool terminal : {false,true})
    {
        for (bool controlWins : {false,true})
        {
            Fixture fixture; auto control = Enable();
            Require(MGG_GraphicsDevice_BeginFrame(&fixture.Device) == 0,"control-race acquired frame");
            auto anchor = Abi<MGG_AndroidTrace_Anchor>(); anchor.Context = Context(); anchor.TraceEpoch = control.TraceEpoch;
            anchor.CallerRequestIdLo = 909; anchor.CallerStopwatchFrequency = 1000000000; anchor.CallerAnchorKind = 17;
            auto operation = Abi<MGG_AndroidTrace_Control>();
            if (terminal)
            {
                Require(MGG_AndroidTrace_BeginOperation(&anchor,&operation) == 0,"end race admitted parent");
                anchor.OperationId = operation.OperationId; anchor.CallerAnchorKind = 18;
            }
            controlBarrierStage = (terminal ? 2 : 1) + (controlWins ? 2 : 0);
            controlBarrierReached = disableAttempted = disableCompleted = 0;
            MGVK_TraceControlTestBarrier = ControlBarrier;
            int disableStatus = 99;
            std::thread disabler([&] {
                while (!controlBarrierReached.load()) { std::this_thread::yield(); }
                disableAttempted = 1;
                disableStatus = MGG_AndroidTrace_Disable(&control);
                disableCompleted = 1;
            });
            int status = terminal ? MGG_AndroidTrace_EndOperation(&anchor) : MGG_AndroidTrace_BeginOperation(&anchor,&operation);
            disabler.join(); MGVK_TraceControlTestBarrier = nullptr;
            auto events = Read(control.TraceEpoch); const MGG_AndroidTrace_Event* committed = nullptr;
            for (auto& event : events)
            {
                if (event.EventKind == MGG_TRACE_MANAGED_CALLER_ANCHOR && event.CallerAnchorKind == anchor.CallerAnchorKind)
                { committed = &event; }
            }
            Require(disableStatus == 0,"racing disable sealed exact trace");
            if (controlWins)
            {
                Require(status == 0 && committed && committed->TraceEpoch && committed->OperationId && committed->DeviceId &&
                    committed->SessionId && committed->NativeTid && committed->FrameSequence && committed->AcquireAttemptId,
                    "control OK requires committed nonzero identity");
                auto* sealed = Last(events,MGG_TRACE_TRACE_SEALED);
                Require(sealed && committed->Sequence < sealed->Sequence,"control anchor committed before Disable seal");
                if (terminal) { Require(!MGVK_TraceParent.OperationId,"terminal scope cleared with committed anchor"); }
                else { Require(operation.TraceEpoch == committed->TraceEpoch && operation.OperationId == committed->OperationId &&
                    MGVK_TraceParent.OperationId == operation.OperationId,"begin scope/output published with committed anchor"); }
            }
            else
            {
                Require(status == MGG_TRACE_NOT_ENABLED && !committed,"Disable winner prevents dropped-record OK");
                if (terminal) { Require(MGVK_TraceParent.OperationId == operation.OperationId,"failed terminal never clears as if committed"); }
                else { Require(!operation.TraceEpoch && !operation.OperationId && !MGVK_TraceParent.OperationId,"failed activation publishes no scope/output"); }
            }
            std::cout << "CONTROL_RACE operation=" << (terminal ? "end" : "begin") << " winner=" << (controlWins ? "control" : "disable")
                << " status=" << status << " anchorCommitted=" << bool(committed) << " identities=verified\n";
        }
    }
}
static void TraceTests()
{
    Require(MGG_AndroidTrace_GetApiVersion() == 1,"trace API version");
    {
        Fixture f; Require(!MGVK_TraceEnabled.load() && !MGVK_TraceStorageOwner,"default has no ring");
        auto beforeClock = nativeClockCalls.load();
        acquireResult = VK_TIMEOUT; Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == -1,"disabled timeout");
        Require(nativeClockCalls.load() == beforeClock && !MGVK_TraceStorageOwner,"disabled calls have no trace clock/ring");
        auto request = Abi<MGG_AndroidTrace_EnableRequest>(); request.Context = Context(); request.Diagnostic = 1;
        request.CallerRunNonceLo = 1; request.Measurement = 1; auto control = Abi<MGG_AndroidTrace_Control>();
        Require(MGG_AndroidTrace_Enable(&request,&control) == MGG_TRACE_MEASUREMENT_FORBIDDEN && !MGVK_TraceStorageOwner,"measurement no allocation");
        request.Measurement = 0;
        failClock = true;
        Require(MGG_AndroidTrace_Enable(&request,&control) == MGG_TRACE_CLOCK_FAILURE && !MGVK_TraceEnabled.load() &&
            MGVK_TraceStorageOwner->Sealed && MGVK_TraceStorageOwner->Invalid == MGG_TRACE_CLOCK_FAILURE,"cold clock failure preserved and inactive");
        failClock = false;
        request.Version = 99; Require(MGG_AndroidTrace_Enable(&request,&control) == MGG_TRACE_UNSUPPORTED_VERSION,"version");
        request.Version = 1; --request.Size;
        Require(MGG_AndroidTrace_Enable(&request,&control) == MGG_TRACE_INVALID_ARGUMENT,"enable exact size");
        ++request.Size; request.Reserved0 = 1;
        Require(MGG_AndroidTrace_Enable(&request,&control) == MGG_TRACE_INVALID_ARGUMENT,"enable reserved zero");
        auto clock = Abi<MGG_AndroidTrace_Clock>(); Require(MGG_AndroidTrace_GetClock(&clock) == 0 && clock.MonotonicBeforeNs <= clock.MonotonicAfterNs && clock.NativePid == uint64_t(getpid()),"clock");
    }
    {
        Fixture f; uint64_t revision; MGP_Android_Lifecycle observed{};
        Require(MGP_Android_GetLifecycle(1,&observed) == 0,"read initial lifecycle");
        Require(MGP_Android_PublishLifecycle(1,observed.Generation,MGP_ANDROID_PAUSED,&revision) == 0,"same-generation pause");
        Require(MGP_Android_PublishLifecycle(1,observed.Generation,MGP_ANDROID_SURFACE_READY,&revision) == 0,"same-generation missed resume");
        Require(MGP_Android_GetLifecycle(1,&observed) == 0 && observed.RetireRevision > f.Device.androidSurface.RetiredRevision,"retirement pending");
        auto context = Context(); Require(!(context.Flags & MGG_TRACE_LIFECYCLE_READY),"context conservative until retire applied");
        auto control = Enable(); auto events = Read(control.TraceEpoch);
        Require(events[0].LifecycleFlags == MGP_ANDROID_SURFACE_READY && !(events[0].Flags & MGG_TRACE_LIFECYCLE_READY),"cold enable preserves actual observed flags and pending readiness");
        MGVK_TraceRefresh(&f.Device,&observed); auto event = MGVK_TraceContext(&f.Device);
        Require(event.LifecycleFlags == MGP_ANDROID_SURFACE_READY && !(event.Flags & MGG_TRACE_LIFECYCLE_READY),"event readiness matches conservative context");
        f.Device.androidSurface.RetiredRevision = observed.RetireRevision;
        Require(MGP_Android_AcknowledgeRetired(1,observed.RetireRevision) == 0,"fake GPU retirement acknowledgment");
        MGVK_TraceRefresh(&f.Device,&observed); event = MGVK_TraceContext(&f.Device);
        Require(event.Flags & MGG_TRACE_LIFECYCLE_READY,"ready only after matching applied retirement");
    }
    ControlDisableRaces();
    for (VkResult result : {VK_TIMEOUT,VK_NOT_READY,VK_ERROR_OUT_OF_DATE_KHR,VK_ERROR_SURFACE_LOST_KHR})
    {
        Fixture f; auto control = Enable(); acquireResult = result;
        Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == -1,"failed BeginFrame token");
        Require(calls.size() == 1 && f.Device.frames[0].image_index == UINT32_MAX && !f.Device.frames[0].imageAcquiredPending && !f.Device.frames[0].is_recording,"no Draw/EndDraw dispatch or consumption");
        auto events = Read(control.TraceEpoch); auto* acquire = Last(events,MGG_TRACE_ACQUIRE_END);
        auto* returned = Last(events,MGG_TRACE_BEGIN_FRAME_RETURN);
        Require(returned && returned->NativeReturnValue == UINT64_MAX && (returned->Flags & MGG_TRACE_NATIVE_RETURN_VALID) &&
            !(returned->Flags & MGG_TRACE_RESULT_VALID) && returned->SkipReason == MGG_TRACE_SKIP_NO_VALID_FRAME,"actual native token distinct from VkResult");
        Require(acquire && acquire->Result == result && !(acquire->Flags & MGG_TRACE_IMAGE_VALID) && !acquire->FrameSequence,"failure image invalid");
        acquireResult = VK_SUCCESS; f.Device.androidNeedsSwapchain = false; f.Device.androidSurfaceLost = false;
        Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == 0 && f.Device.frames[0].is_recording,"next success resumes");
    }
    {
        Fixture f; auto control = Enable(); acquireResult = VK_SUBOPTIMAL_KHR;
        Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == 0 && f.Device.androidNeedsSwapchain,"suboptimal exact behavior");
        auto events = Read(control.TraceEpoch); Require(Last(events,MGG_TRACE_ACQUIRE_END)->Result == VK_SUBOPTIMAL_KHR,"suboptimal event");
    }
    for (uint32_t skip : {MGG_TRACE_SKIP_NO_SWAPCHAIN,MGG_TRACE_SKIP_NO_FRAMES,MGG_TRACE_SKIP_NO_RECORDING})
    {
        Fixture f; auto control = Enable();
        if (skip == MGG_TRACE_SKIP_NO_SWAPCHAIN) { f.Device.swapchain = VK_NULL_HANDLE; }
        if (skip == MGG_TRACE_SKIP_NO_FRAMES) { f.Device.frames.clear(); }
        MGG_GraphicsDevice_Present(&f.Device,0,0);
        auto events = Read(control.TraceEpoch); auto* returned = Last(events,MGG_TRACE_PRESENT_RETURN);
        Require(returned && returned->SkipReason == skip && !(returned->Flags & MGG_TRACE_RESULT_VALID) && !Last(events,MGG_TRACE_SUBMIT_RESULT),"typed skip return");
    }
    {
        Fixture f; auto control = Enable(); f.Device.androidSurface.Generation = 999;
        MGG_GraphicsDevice_Present(&f.Device,0,0);
        auto events = Read(control.TraceEpoch); Require(Last(events,MGG_TRACE_PRESENT_RETURN)->SkipReason == MGG_TRACE_SKIP_LIFECYCLE,"lifecycle skip");
        f.Device.androidSurface.Generation = 1;
    }
    for (int i = 0; i < 11; ++i)
    {
        const uint32_t expected[] = {MGG_TRACE_SUBMIT_RESULT,MGG_TRACE_QUEUE_PRESENT_RESULT,MGG_TRACE_FENCE_WAIT_RESULT,MGG_TRACE_COMMAND_END_RESULT,MGG_TRACE_FENCE_CREATE_RESULT,MGG_TRACE_FENCE_RESET_RESULT,MGG_TRACE_SUBMIT_RESULT,MGG_TRACE_FENCE_WAIT_RESULT,MGG_TRACE_COMMAND_END_RESULT,MGG_TRACE_SUBMIT_RESULT,MGG_TRACE_FENCE_WAIT_RESULT};
        Fatal(i,expected[i]);
    }
    for (VkResult result : {VK_ERROR_OUT_OF_DATE_KHR,VK_ERROR_SURFACE_LOST_KHR,VK_SUBOPTIMAL_KHR})
    {
        Fixture f; auto control = Enable(); Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == 0,"begin deferred");
        presentResult = result; MGG_GraphicsDevice_Present(&f.Device,0,0); auto events = Read(control.TraceEpoch);
        Require(Last(events,MGG_TRACE_QUEUE_PRESENT_RESULT)->Result == result && Last(events,MGG_TRACE_PRESENT_RETURN) && !Recovery(events),"deferred not success");
    }
    {
        Fixture f; auto control = Enable(); Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == 0,"readback begin");
        auto before = Context(); auto anchor = Abi<MGG_AndroidTrace_Anchor>(); anchor.Context = before;
        anchor.TraceEpoch = control.TraceEpoch; anchor.CallerRequestIdLo = 789; anchor.CallerStopwatchFrequency = 1000000000;
        auto op = Abi<MGG_AndroidTrace_Control>(); Require(MGG_AndroidTrace_BeginOperation(&anchor,&op) == 0,"anchor begin");
        uint8_t data[64]; MGG_GraphicsDevice_GetBackBufferData(&f.Device,0,0,4,4,data,16,4);
        anchor.Context = Context(); anchor.OperationId = op.OperationId;
        Require(MGG_AndroidTrace_EndOperation(&anchor) == 0,"anchor end after resumed recording");
        MGG_GraphicsDevice_Present(&f.Device,0,0);
        auto& frame = f.Device.frames[0]; Require(MGVK_TryAcquireSwap(&f.Device,frame),"wait final payload");
        auto events = Read(control.TraceEpoch); Require(Recovery(events),"exact flush copy final retirement");
        auto* final = Last(events,MGG_TRACE_SUBMIT_RESULT,MGG_TRACE_PRESENT_FINAL);
        Require(final && final->DrawCommandCount == 0 && final->WaitSemaphoreCount == 0,"resumed empty final independent scene unproven");
        auto* copy = Last(events,MGG_TRACE_SUBMIT_RESULT,MGG_TRACE_READBACK_COPY);
        Require(copy && copy->CallerRequestIdLo == 789 && copy->ImageIndex == 0,"readback parent provenance");
        for (int mutation = 0; mutation < 12; ++mutation)
        {
            auto bad = events; for (auto& event : bad)
            {
                if (event.EventKind != MGG_TRACE_QUEUE_PRESENT_RESULT) { continue; }
                if (mutation == 0) { ++event.DeviceId; } if (mutation == 1) { ++event.SessionId; }
                if (mutation == 2) { ++event.TraceEpoch; } if (mutation == 3) { ++event.SwapchainGeneration; }
                if (mutation == 4) { ++event.FrameSequence; } if (mutation == 5) { ++event.ImageIndex; }
                if (mutation == 6) { ++event.AcquireAttemptId; } if (mutation == 7) { ++event.AppliedSurfaceGeneration; }
                if (mutation == 8) { event.Sequence = 1; } if (mutation == 9) { event.Flags &= ~MGG_TRACE_RESULT_VALID; }
                if (mutation == 10) { event.Result = VK_ERROR_OUT_OF_DATE_KHR; } if (mutation == 11) { event.EventKind = MGG_TRACE_PRESENT_RETURN; }
            }
            Require(!Recovery(bad),"invalid join rejected");
        }
        auto missing = events; missing.erase(std::remove_if(missing.begin(),missing.end(),[](auto& e) { return e.WorkKind == MGG_TRACE_READBACK_RENDER_FLUSH; }),missing.end());
        for (size_t i = 0; i < missing.size(); ++i) { missing[i].Sequence = i + 1; }
        Require(!Recovery(missing),"copy alone cannot recover");
        for (int mutation = 0; mutation < 8; ++mutation)
        {
            auto bad = events;
            for (auto& event : bad)
            {
                if (event.EventKind == MGG_TRACE_FENCE_WAIT_RESULT && event.WorkKind == MGG_TRACE_PRESENT_FINAL)
                {
                    if (mutation == 0) { ++event.FenceInstanceId; } if (mutation == 1) { ++event.FencePayloadId; }
                    if (mutation == 2) { ++event.SubmitId; } if (mutation == 3) { event.Result = VK_TIMEOUT; }
                    if (mutation == 4) { event.Flags &= ~MGG_TRACE_RESULT_VALID; }
                    if (mutation == 5) { event.WorkKind = MGG_TRACE_READBACK_COPY; }
                    if (mutation == 6) { ++event.CallerRequestIdLo; } if (mutation == 7) { ++event.NativeSurfaceInstanceId; }
                }
            }
            Require(!Recovery(bad),"fence payload/request false join rejected");
        }
        Require(!Recovery(events,MGG_TRACE_OVERFLOW_INVALID),"overflow metadata cannot recover");
    }
    {
        Fixture f; auto control = Enable(); MGG_Texture texture{}; texture.info.extent = {4,4,1};
        texture.info.mipLevels = texture.info.arrayLayers = 1; texture.image = Handle<VkImage>(80);
        texture.layouts[0] = texture.optimal_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        uint8_t bytes[64]{};
        MGG_Texture_SetData(&f.Device,&texture,0,0,0,0,0,4,4,1,bytes,64);
        statusResult = VK_NOT_READY; MGVK_CleanupPendingTransfers(&f.Device); Require(f.Device.pendingTransfers.size() == 1,"transfer not ready");
        statusResult = VK_SUCCESS; MGVK_CleanupPendingTransfers(&f.Device); Require(f.Device.pendingTransfers.empty(),"transfer retired");
        submitResult = VK_ERROR_DEVICE_LOST;
        MGG_Texture_SetData(&f.Device,&texture,0,0,0,0,0,4,4,1,bytes,64);
        waitResult = VK_TIMEOUT; MGVK_WaitPendingTransfers(&f.Device,nullptr);
        MGG_GraphicsDevice_Present(&f.Device,0,0); auto events = Read(control.TraceEpoch);
        Require(Last(events,MGG_TRACE_SUBMIT_RESULT,MGG_TRACE_TEXTURE_OR_BUFFER_TRANSFER)->Result == VK_ERROR_DEVICE_LOST &&
            Last(events,MGG_TRACE_FENCE_WAIT_RESULT)->Result == VK_TIMEOUT && !Recovery(events),"unchecked async failures retained, no false present");
        std::vector<uint64_t> instances;
        for (auto& event : events) { if (event.EventKind == MGG_TRACE_FENCE_CREATE_RESULT && event.FenceInstanceId) { instances.push_back(event.FenceInstanceId); } }
        Require(instances.size() == 2 && instances[0] != instances[1],"reused raw fence new native instance");
        submitResult = VK_SUCCESS; waitResult = VK_SUCCESS;
        MGG_Texture_SetData(&f.Device,&texture,0,0,0,0,0,4,4,1,bytes,64);
        statusResult = VK_ERROR_DEVICE_LOST; MGVK_CleanupPendingTransfers(&f.Device);
        Require(f.Device.pendingTransfers.empty(),"original failed-status cleanup preserved");
        events = Read(control.TraceEpoch);
        Require(Last(events,MGG_TRACE_FENCE_STATUS_RESULT)->Result == VK_ERROR_DEVICE_LOST && !Recovery(events),"failed status never retirement success");
    }
    {
        Fixture f; auto control = Enable(); auto context = Context(); std::atomic<bool> good{true};
        std::atomic<int> phase{0}, ready{0}, enabledReads{0}, staleReads{0}, replacementAttempts{0};
        std::vector<std::thread> writers;
        auto event = MGVK_TraceContext(&f.Device);
        for (int thread = 0; thread < 4; ++thread)
        {
            writers.emplace_back([&,event] {
                for (int i = 0; i < 100; ++i) { MGVK_TraceRecord(event,MGG_TRACE_MANAGED_CALLER_ANCHOR); }
                ++ready;
                while (phase.load() == 0) { std::this_thread::yield(); }
                while (phase.load() < 3)
                {
                    MGVK_TraceRecord(event,MGG_TRACE_MANAGED_CALLER_ANCHOR);
                    if (phase.load() == 2) { ++replacementAttempts; }
                    std::this_thread::yield();
                }
            });
        }
        std::thread reader([&] {
            auto request = Abi<MGG_AndroidTrace_ReadRequest>(); request.TraceEpoch = control.TraceEpoch;
            request.EventBytes = 320; request.Capacity = 256; MGG_AndroidTrace_Event page[256];
            while (phase.load() < 3)
            {
                auto result = Abi<MGG_AndroidTrace_ReadResult>();
                int status = MGG_AndroidTrace_Read(&request,&result,page);
                if (status == MGG_TRACE_OK)
                {
                    if (!result.Sealed && result.Count >= 405) { ++enabledReads; }
                    for (uint32_t i = 0; i < result.Copied; ++i)
                    {
                        if (page[i].TraceEpoch != control.TraceEpoch || page[i].Size != 320 ||
                            page[i].Version != 1 || page[i].Sequence != i + 1 || !page[i].MonotonicNs) { good = false; }
                    }
                }
                else if (status == MGG_TRACE_STALE_EPOCH) { ++staleReads; }
                else { good = false; }
                std::this_thread::yield();
            }
        });
        auto await = [](auto predicate) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!predicate() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
            return predicate();
        };
        bool observedEnabled = await([&] { return ready.load() == 4 && enabledReads.load() > 0; });
        phase = 1;
        int disabled = MGG_AndroidTrace_Disable(&control);
        auto newer = Enable();
        phase = 2;
        bool observedReplacement = await([&] { return replacementAttempts.load() >= 400 && staleReads.load() > 0; });
        phase = 3;
        for (auto& writer : writers) { writer.join(); } reader.join();
        Require(observedEnabled && observedReplacement && good && disabled == 0,"proved enabled writes/reads and live replacement race");
        Require(newer.TraceEpoch != control.TraceEpoch && Read(newer.TraceEpoch).size() == 5,"stale live workers cannot publish into replacement");
        std::cout << "RACE enabledWrites=400 enabledReads=" << enabledReads << " staleReads=" << staleReads << " replacementWriterAttempts=" << replacementAttempts << " replacementCount=5\n";
        uint64_t revision; MGP_Android_PublishLifecycle(1,2,MGP_ANDROID_SURFACE_READY,&revision);
        auto request = Abi<MGG_AndroidTrace_EnableRequest>(); request.Context = context; request.Diagnostic = 1; request.CallerRunNonceLo = 1;
        Require(MGG_AndroidTrace_Enable(&request,&control) == MGG_TRACE_STALE_CONTEXT,"lifecycle invalidates token");
        MGP_Android_PublishLifecycle(1,2,MGP_ANDROID_SURFACE_READY,&revision);
    }
    {
        Fixture f; auto control = Enable(); auto event = MGVK_TraceContext(&f.Device);
        for (uint32_t i = 5; i < MGG_ANDROID_TRACE_CAPACITY; ++i) { MGVK_TraceRecord(event,MGG_TRACE_MANAGED_CALLER_ANCHOR); }
        auto events = Read(control.TraceEpoch); Require(events.size() == 32768,"exact capacity");
        MGVK_TraceRecord(event,MGG_TRACE_MANAGED_CALLER_ANCHOR);
        auto overflow = Read(control.TraceEpoch,MGG_TRACE_OVERFLOW_INVALID); Require(overflow.size() == 32768 && overflow.back().Sequence == 32768,"overflow no wrap");
    }
    {
        Fixture f; auto control = Enable();
        for (int frame = 0; frame < 3; ++frame)
        {
            int token = MGG_GraphicsDevice_BeginFrame(&f.Device); Require(token == frame % 2,"persistent fence begin");
            MGG_GraphicsDevice_Present(&f.Device,token,0);
        }
        auto events = Read(control.TraceEpoch); std::vector<MGG_AndroidTrace_Event> submits;
        for (auto& event : events) { if (event.EventKind == MGG_TRACE_SUBMIT_RESULT && event.WorkKind == MGG_TRACE_PRESENT_FINAL) { submits.push_back(event); } }
        Require(submits.size() == 3 && submits[0].FenceInstanceId == submits[2].FenceInstanceId &&
            submits[0].FencePayloadId != submits[2].FencePayloadId,"persistent raw fence keeps instance, new payload");
        auto* reset = Last(events,MGG_TRACE_FENCE_RESET_RESULT);
        Require(reset && reset->FrameSequence == submits[2].FrameSequence && reset->AcquireAttemptId == submits[2].AcquireAttemptId &&
            reset->FencePayloadId == submits[2].FencePayloadId && !reset->SubmitId,"reset belongs to upcoming signal cycle/current acquired frame");
        auto* retired = Last(events,MGG_TRACE_FENCE_WAIT_RESULT);
        Require(retired && retired->FencePayloadId == submits[0].FencePayloadId && retired->SubmitId == submits[0].SubmitId &&
            retired->FrameSequence == submits[0].FrameSequence,"wait retains exact prior signal cycle");
    }
    for (int cross = 0; cross < 3; ++cross)
    {
        Fixture f; auto control = Enable(); Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == 0,"parent native frame");
        auto anchor = Abi<MGG_AndroidTrace_Anchor>(); anchor.Context = Context(); anchor.TraceEpoch = control.TraceEpoch;
        anchor.CallerRequestIdLo = 456; anchor.CallerStopwatchFrequency = 1000000000;
        auto operation = Abi<MGG_AndroidTrace_Control>(); Require(MGG_AndroidTrace_BeginOperation(&anchor,&operation) == 0,"parent admission");
        anchor.OperationId = operation.OperationId;
        if (cross == 0)
        {
            auto wrong = anchor; ++wrong.OperationId;
            Require(MGG_AndroidTrace_EndOperation(&wrong) == MGG_TRACE_STALE_CONTEXT && MGVK_TraceParent.OperationId == operation.OperationId,"wrong end keeps valid parent");
            wrong = anchor; ++wrong.Context.NativeFrameCounter;
            Require(MGG_AndroidTrace_EndOperation(&wrong) == MGG_TRACE_STALE_CONTEXT && MGVK_TraceParent.OperationId == operation.OperationId,"wrong context keeps valid parent");
            Require(MGG_AndroidTrace_EndOperation(&anchor) == 0,"valid end still accepted");
        }
        if (cross == 1)
        {
            MGG_GraphicsDevice_Present(&f.Device,0,0);
            Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == 1,"next acquired frame");
            auto current = MGVK_TraceContext(&f.Device,&f.Device.frames[1]);
            Require(current.ParentOperationId == 0 && current.CallerRequestIdLo == 0 && !MGVK_TraceParent.OperationId,"cross-frame parent cannot inherit");
            anchor.Context = Context(); Require(MGG_AndroidTrace_EndOperation(&anchor) == MGG_TRACE_STALE_CONTEXT,"fresh context cannot end old frame parent");
            Read(control.TraceEpoch,MGG_TRACE_STALE_CONTEXT);
        }
        if (cross == 2)
        {
            uint64_t revision; auto generation = anchor.Context.HostSurfaceGeneration;
            MGP_Android_PublishLifecycle(1,generation + 1,MGP_ANDROID_SURFACE_READY,&revision);
            anchor.Context = Context();
            Require(MGG_AndroidTrace_EndOperation(&anchor) == MGG_TRACE_STALE_CONTEXT && !MGVK_TraceParent.OperationId,"lifecycle parent invalidated");
            Read(control.TraceEpoch,MGG_TRACE_STALE_CONTEXT);
        }
    }
    {
        Fixture f; auto control = Enable(); auto event = MGVK_TraceContext(&f.Device);
        auto before = allocationCalls.load();
        for (int i = 0; i < 1000; ++i) { MGVK_TraceRecord(event,MGG_TRACE_MANAGED_CALLER_ANCHOR); }
        auto request = Abi<MGG_AndroidTrace_ReadRequest>(); request.TraceEpoch = control.TraceEpoch;
        request.EventBytes = 320; request.Capacity = 1; auto result = Abi<MGG_AndroidTrace_ReadResult>(); MGG_AndroidTrace_Event output;
        Require(MGG_AndroidTrace_Read(&request,&result,&output) == 0,"no allocation read");
        Require(MGG_AndroidTrace_Disable(&control) == 0,"no allocation disable");
        Require(allocationCalls.load() == before,"collector append/read/disable allocate nothing");
        std::atomic<int32_t> foreign{0}; std::thread thread([&] { auto context = Abi<MGG_AndroidTrace_Context>(); foreign = MGG_AndroidTrace_GetCurrentContext(&context); }); thread.join();
        Require(foreign != 0,"foreign native thread cannot obtain current device");
        MGVK_TraceSealDevice(&f.Device); Read(control.TraceEpoch);
    }
    {
        alignas(Fixture) unsigned char bytes[sizeof(Fixture)];
        auto* first = new(bytes) Fixture;
        auto old = Enable(); const uint64_t oldDevice = first->Device.traceDeviceId;
        first->~Fixture(); Require(!MGVK_TraceEnabled.load(),"device end seals"); Read(old.TraceEpoch);
        auto* second = new(bytes) Fixture;
        auto current = Enable(); Require(second->Device.traceDeviceId != oldDevice && current.TraceEpoch != old.TraceEpoch,"same pointer new device/epoch");
        auto request = Abi<MGG_AndroidTrace_ReadRequest>(); request.TraceEpoch = old.TraceEpoch; request.EventBytes = 320;
        auto result = Abi<MGG_AndroidTrace_ReadResult>();
        Require(MGG_AndroidTrace_Read(&request,&result,nullptr) == MGG_TRACE_STALE_EPOCH,"old device read token stale");
        request.TraceEpoch = current.TraceEpoch; request.EventBytes = 319;
        Require(MGG_AndroidTrace_Read(&request,&result,nullptr) == MGG_TRACE_INVALID_ARGUMENT,"read event size");
        request.EventBytes = 320; request.Capacity = 257;
        Require(MGG_AndroidTrace_Read(&request,&result,nullptr) == MGG_TRACE_INVALID_ARGUMENT,"bounded read");
        second->~Fixture();
    }
    {
        Fixture first; Fixture second; auto context = Abi<MGG_AndroidTrace_Context>();
        Require(MGG_AndroidTrace_GetCurrentContext(&context) == MGG_TRACE_AMBIGUOUS_DEVICE,"multiple native devices are ambiguous");
    }
    for (int fault = 0; fault < 4; ++fault)
    {
        Fixture f;
        const uint64_t saved = MGVK_TraceNextId.load();
        auto request = Abi<MGG_AndroidTrace_EnableRequest>(); request.Context = Context(); request.Diagnostic = 1; request.CallerRunNonceLo = 1;
        auto control = Abi<MGG_AndroidTrace_Control>();
        if (fault == 0) { MGVK_TraceIdExhausted = true; }
        if (fault == 1) { MGVK_TraceNextId = UINT64_MAX; }
        if (fault == 2) { MGVK_TraceNextId = UINT64_MAX - 1; }
        if (fault == 3) { f.Device.traceDeviceId = 0; request.Context.DeviceId = 0; }
        Require(MGG_AndroidTrace_Enable(&request,&control) == MGG_TRACE_RESOURCE_LIMIT_INVALID && !MGVK_TraceEnabled.load(),"exhaustion/zero IDs never enable success");
        MGVK_TraceNextId = saved; MGVK_TraceIdExhausted = false;
    }
    {
        Fixture f; auto control = Enable(); auto event = MGVK_TraceContext(&f.Device);
        { std::lock_guard<std::mutex> lock(MGVK_TraceMutex); MGVK_TraceStorageOwner->Attempted = UINT64_MAX; }
        MGVK_TraceRecord(event,MGG_TRACE_MANAGED_CALLER_ANCHOR);
        Require(!MGVK_TraceEnabled.load(),"sequence exhaustion seals"); Read(control.TraceEpoch,MGG_TRACE_RESOURCE_LIMIT_INVALID);
    }
    std::cout << "trace cases: ABI/defaultoff/measurement/clock+coldfailure; acquire4+resume/suboptimal; typed skips4; fatal11; deferred3; actual backbuffer flush/copy/final; test-only schema joins22; async transfer/reused fence/persistent payload cycles; concurrent writers/read/disable/re-enable; overflow; exact parent frame/lifecycle/wrongEnd; no allocation; ID+sequence exhaustion PASS\n";
}
#else
extern "C" void __real___assert_fail(const char*,const char*,unsigned int,const char*) __attribute__((noreturn));
extern "C" void __wrap___assert_fail(const char* e,const char* f,unsigned int l,const char* fn) { __real___assert_fail(e,f,l,fn); }
#endif
static void Transcript()
{
    for (VkResult result : {VK_TIMEOUT,VK_NOT_READY,VK_ERROR_OUT_OF_DATE_KHR,VK_ERROR_SURFACE_LOST_KHR,VK_SUCCESS})
    {
        Fixture f; acquireResult = result; int token = MGG_GraphicsDevice_BeginFrame(&f.Device);
        if (token >= 0) { MGG_GraphicsDevice_Present(&f.Device,token,0); }
        std::cout << "CASE " << result << " token=" << token << " index=" << f.Device.frames[0].image_index <<
            " pending=" << f.Device.frames[0].imageAcquiredPending << " recording=" << f.Device.frames[0].is_recording <<
            " rendering=" << f.Device.frames[0].is_rendering << " frame=" << f.Device.frame << '\n';
        for (const auto& call : calls) { std::cout << call << '\n'; }
    }
    {
        Fixture f; Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == 0,"transcript begin");
        MGVK_FlushCommands(&f.Device,f.Device.frames[0]);
        MGVK_BeginCommandBuffer(f.Device.frames[0].commandBuffer); f.Device.frames[0].is_recording = true;
        auto copy = MGVK_BeginNewCommandBuffer(&f.Device); MGVK_ExecuteAndFreeCommandBuffer(&f.Device,copy);
        MGG_GraphicsDevice_Present(&f.Device,0,0); MGVK_TryAcquireSwap(&f.Device,f.Device.frames[0]);
        std::cout << "CASE flush-copy-final frame=" << f.Device.frame << " pending=" << f.Device.frames[0].imageAcquiredPending << '\n';
        for (const auto& call : calls) { std::cout << call << '\n'; }
    }
    for (VkResult result : {VK_ERROR_OUT_OF_DATE_KHR,VK_ERROR_SURFACE_LOST_KHR,VK_SUBOPTIMAL_KHR})
    {
        Fixture f; Require(MGG_GraphicsDevice_BeginFrame(&f.Device) == 0,"transcript deferred begin");
        presentResult = result; MGG_GraphicsDevice_Present(&f.Device,0,0);
        std::cout << "CASE deferred=" << result << " recreate=" << f.Device.androidNeedsSwapchain << " lost=" << f.Device.androidSurfaceLost << " frame=" << f.Device.frame << '\n';
        for (const auto& call : calls) { std::cout << call << '\n'; }
    }
}
static int32_t Run(void*)
{
    try
    {
        Install();
#ifndef MGG_TRACE_BASELINE
        TraceTests();
#endif
        Transcript();
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
int main()
{
    Require(MGP_Android_RegisterRunCallback(1,Run,nullptr) == 0,"register");
    uint64_t revision; Require(MGP_Android_PublishLifecycle(1,1,MGP_ANDROID_SURFACE_READY,&revision) == 0,"ready");
    char program[] = "trace-tests", session[] = "--mgp-android-session=1"; char* argv[] = {program,session};
    int result = SDL_main(2,argv); Require(MGP_Android_ClearRunCallback(1) == 0,"clear"); return result;
}
