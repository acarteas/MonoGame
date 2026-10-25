// Included only by the Android Debug renderer. No IO or Vulkan calls in collector.
#include <atomic>
#include <memory>
#include <new>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stddef.h>

static_assert(sizeof(MGG_AndroidTrace_Context) == 160, "context ABI");
static_assert(sizeof(MGG_AndroidTrace_Event) == 320, "event ABI");
static_assert(offsetof(MGG_AndroidTrace_Event, EventKind) == 264, "event ABI");
static_assert(offsetof(MGG_AndroidTrace_Event, SkipReason) == 308, "skip ABI");
static_assert(sizeof(MGG_AndroidTrace_EnableRequest) == 200, "enable ABI");
static_assert(sizeof(MGG_AndroidTrace_ReadRequest) == 32, "read ABI");
static_assert(sizeof(MGG_AndroidTrace_ReadResult) == 72, "read result ABI");
static_assert(sizeof(MGG_AndroidTrace_Anchor) == 240, "anchor ABI");
static_assert(sizeof(MGG_AndroidTrace_Control) == 24, "control ABI");
static_assert(sizeof(MGG_AndroidTrace_Clock) == 64, "clock ABI");

static std::atomic<bool> MGVK_TraceEnabled{false};
static std::atomic<uint64_t> MGVK_TraceNextId{1};
static std::atomic<bool> MGVK_TraceIdExhausted{false};
static std::mutex MGVK_TraceMutex;
static thread_local MGG_AndroidTrace_Event MGVK_TraceParent{};
static thread_local uint64_t MGVK_TraceReadbackRestart = 0;
static thread_local bool MGVK_TraceParentInvalidated = false;

struct MGVK_TraceResource
{
    uint64_t Handle = 0, Id = 0, Recording = 0, PendingPayload = 0;
    MGG_AndroidTrace_Event Submission{};
};
struct MGVK_TraceStorage
{
    MGG_AndroidTrace_Event Events[MGG_ANDROID_TRACE_CAPACITY]{};
    MGVK_TraceResource Commands[512]{}, Fences[1024]{};
    MGG_AndroidTrace_Event Base{};
    MGG_GraphicsDevice* Device = nullptr;
    uint64_t Epoch = 0, Attempted = 0, Overflow = 0;
    uint64_t CallerRunNonceLo = 0, CallerRunNonceHi = 0;
    uint32_t Count = 0;
    int32_t Invalid = 0;
    bool Sealed = false;
};
static std::unique_ptr<MGVK_TraceStorage> MGVK_TraceStorageOwner;

static uint64_t MGVK_TraceTid()
{
    return static_cast<uint64_t>(syscall(SYS_gettid));
}
static uint64_t MGVK_TraceId()
{
    uint64_t value = MGVK_TraceNextId.load(std::memory_order_relaxed);
    while (value != UINT64_MAX && value != 0)
    {
        if (MGVK_TraceNextId.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
        {
            return value;
        }
    }
    MGVK_TraceIdExhausted.store(true, std::memory_order_relaxed);
    return 0;
}
static bool MGVK_TraceTime(clockid_t id, uint64_t& value)
{
    timespec time{};
    if (clock_gettime(id, &time) != 0)
    {
        return false;
    }
    value = static_cast<uint64_t>(time.tv_sec) * UINT64_C(1000000000) + time.tv_nsec;
    return true;
}
static bool MGVK_TraceMatches(MGG_GraphicsDevice* device)
{
    auto* storage = MGVK_TraceStorageOwner.get();
    return MGVK_TraceEnabled.load(std::memory_order_relaxed) && storage && !storage->Sealed &&
        storage->Device == device && storage->Base.DeviceId == device->traceDeviceId &&
        storage->Base.SessionId == device->androidSurface.SessionId;
}
static void MGVK_TraceAppend(MGG_AndroidTrace_Event event)
{
    auto& storage = *MGVK_TraceStorageOwner;
    event.Size = sizeof(event);
    event.Version = MGG_ANDROID_TRACE_VERSION;
    event.TraceEpoch = storage.Epoch;
    if (storage.Attempted == UINT64_MAX || MGVK_TraceIdExhausted.load(std::memory_order_relaxed))
    {
        storage.Invalid = MGG_TRACE_RESOURCE_LIMIT_INVALID;
        storage.Sealed = true;
        MGVK_TraceEnabled.store(false, std::memory_order_release);
        return;
    }
    event.Sequence = ++storage.Attempted;
    event.NativeTid = MGVK_TraceTid();
    if (!MGVK_TraceTime(CLOCK_MONOTONIC, event.MonotonicNs))
    {
        storage.Invalid = MGG_TRACE_CLOCK_FAILURE;
    }
    if (storage.Count == MGG_ANDROID_TRACE_CAPACITY)
    {
        ++storage.Overflow;
        storage.Invalid = MGG_TRACE_OVERFLOW_INVALID;
        return;
    }
    storage.Events[storage.Count++] = event;
}
static MGG_AndroidTrace_Event MGVK_TraceNativeBase(MGG_GraphicsDevice* device)
{
    MGG_AndroidTrace_Event event{};
    event.DeviceId = device->traceDeviceId;
    event.SessionId = device->androidSurface.SessionId;
    event.QueueId = device->traceQueueId;
    event.NativeSurfaceInstanceId = device->surface ? device->traceSurfaceId : 0;
    event.SwapchainGeneration = device->traceSwapchainGeneration;
    event.AppliedSurfaceGeneration = device->androidSurface.Generation;
    event.ImageIndex = UINT32_MAX;
    event.Flags = MGG_TRACE_LIVE_DEVICE;
    event.NativeFrameCounter = device->frame;
    if (device->swapchain != VK_NULL_HANDLE)
    {
        event.Flags |= MGG_TRACE_HAS_SWAPCHAIN;
    }
    return event;
}
static void MGVK_TraceRefresh(MGG_GraphicsDevice* device, const MGP_Android_Lifecycle* lifecycle = nullptr)
{
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    if (!MGVK_TraceMatches(device))
    {
        return;
    }
    auto event = MGVK_TraceNativeBase(device);
    auto& storage = *MGVK_TraceStorageOwner;
    event.HostSurfaceGeneration = storage.Base.HostSurfaceGeneration;
    event.LifecycleRevision = storage.Base.LifecycleRevision;
    event.RetireRevision = storage.Base.RetireRevision;
    event.LifecycleFlags = storage.Base.LifecycleFlags;
    if (lifecycle)
    {
        event.HostSurfaceGeneration = lifecycle->Generation;
        event.LifecycleRevision = lifecycle->Revision;
        event.RetireRevision = lifecycle->RetireRevision;
        event.LifecycleFlags = lifecycle->Flags;
    }
    if (event.LifecycleFlags == MGP_ANDROID_SURFACE_READY && event.AppliedSurfaceGeneration != 0 &&
        event.AppliedSurfaceGeneration == event.HostSurfaceGeneration &&
        event.RetireRevision <= device->androidSurface.RetiredRevision)
    {
        event.Flags |= MGG_TRACE_LIFECYCLE_READY;
    }
    storage.Base = event;
}
static MGVK_TraceResource* MGVK_TraceFind(MGVK_TraceResource* slots, size_t count, uint64_t handle, bool create)
{
    MGVK_TraceResource* empty = nullptr;
    for (size_t i = 0; i < count; ++i)
    {
        if (slots[i].Handle == handle && handle != 0)
        {
            return &slots[i];
        }
        if (!slots[i].Handle && !empty)
        {
            empty = &slots[i];
        }
    }
    if (create && empty && handle)
    {
        *empty = {};
        empty->Handle = handle;
        empty->Id = MGVK_TraceId();
        return empty;
    }
    if (create)
    {
        MGVK_TraceStorageOwner->Invalid = MGG_TRACE_RESOURCE_LIMIT_INVALID;
    }
    return nullptr;
}
template<class Handle> static uint64_t MGVK_TraceBits(Handle handle)
{
    return static_cast<uint64_t>((uintptr_t)handle);
}
static bool MGVK_TraceParentBinding(MGG_GraphicsDevice* device, const MGP_Android_Lifecycle& lifecycle)
{
    const auto& parent = MGVK_TraceParent;
    if (!parent.OperationId) { return true; }
    if (device->traceOwnerTid != MGVK_TraceTid() || device->frames.empty()) { return false; }
    const auto& frame = device->frames[device->frameIndex];
    return parent.DeviceId == device->traceDeviceId && parent.SessionId == device->androidSurface.SessionId &&
        parent.NativeTid == device->traceOwnerTid && parent.HostSurfaceGeneration == lifecycle.Generation &&
        parent.LifecycleRevision == lifecycle.Revision && parent.RetireRevision == lifecycle.RetireRevision &&
        lifecycle.Flags == MGP_ANDROID_SURFACE_READY && parent.AppliedSurfaceGeneration == device->androidSurface.Generation &&
        parent.SwapchainGeneration == device->traceSwapchainGeneration && parent.NativeSurfaceInstanceId == (device->surface ? device->traceSurfaceId : 0) &&
        parent.FrameSequence != 0 && parent.FrameSequence == frame.trace.FrameSequence &&
        parent.AcquireAttemptId == frame.trace.AcquireAttemptId && parent.ImageIndex == frame.image_index &&
        parent.NativeFrameCounter == device->frame && parent.CommandBufferId == frame.trace.CommandBufferId &&
        (parent.CommandRecordingId == frame.trace.CommandRecordingId ||
            (MGVK_TraceReadbackRestart && frame.trace.CommandRecordingId == MGVK_TraceReadbackRestart));
}
static MGG_AndroidTrace_Event MGVK_TraceContext(MGG_GraphicsDevice* device, MGVK_Frame* frame = nullptr)
{
    bool staleParent = false;
    if (MGVK_TraceParent.OperationId && MGVK_TraceParent.TraceEpoch &&
        MGVK_TraceParent.DeviceId == device->traceDeviceId)
    {
        MGP_Android_Lifecycle lifecycle{};
        staleParent = MGP_Android_GetLifecycle(device->androidSurface.SessionId, &lifecycle) != MGP_ANDROID_OK ||
            !MGVK_TraceParentBinding(device, lifecycle);
    }
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    MGG_AndroidTrace_Event event{};
    if (!MGVK_TraceMatches(device))
    {
        return event;
    }
    event = MGVK_TraceStorageOwner->Base;
    event.TraceEpoch = MGVK_TraceStorageOwner->Epoch;
    event.NativeTid = MGVK_TraceTid();
    if (staleParent)
    {
        MGVK_TraceStorageOwner->Invalid = MGG_TRACE_STALE_CONTEXT;
        MGVK_TraceParent = {};
        MGVK_TraceReadbackRestart = 0;
        MGVK_TraceParentInvalidated = true;
    }
    if (frame)
    {
        event.FrameSequence = frame->trace.FrameSequence;
        event.AcquireAttemptId = frame->trace.AcquireAttemptId;
        event.CommandBufferId = frame->trace.CommandBufferId;
        event.CommandRecordingId = frame->trace.CommandRecordingId;
        event.NativeFrameCounter = device->frame;
        event.DrawCommandCount = frame->trace.DrawCommandCount;
        event.Flags |= frame->trace.Flags & MGG_TRACE_PARTIAL_AT_ENABLE;
        if (frame->image_index != UINT32_MAX)
        {
            event.ImageIndex = frame->image_index;
            event.Flags |= MGG_TRACE_IMAGE_VALID;
        }
        if (frame->is_recording)
        {
            event.Flags |= MGG_TRACE_RECORDING;
        }
    }
    if (MGVK_TraceParent.TraceEpoch == event.TraceEpoch && MGVK_TraceParent.DeviceId == event.DeviceId &&
        MGVK_TraceParent.SessionId == event.SessionId)
    {
        event.ParentOperationId = MGVK_TraceParent.OperationId;
        event.CallerRequestIdLo = MGVK_TraceParent.CallerRequestIdLo;
        event.CallerRequestIdHi = MGVK_TraceParent.CallerRequestIdHi;
        event.WorkKind = MGVK_TraceParent.WorkKind;
        if (!frame && (event.WorkKind == MGG_TRACE_READBACK_COPY || event.WorkKind == MGG_TRACE_READBACK_RENDER_FLUSH))
        {
            event.FrameSequence = MGVK_TraceParent.FrameSequence;
            event.AcquireAttemptId = MGVK_TraceParent.AcquireAttemptId;
            event.ImageIndex = MGVK_TraceParent.ImageIndex;
            event.Flags |= MGVK_TraceParent.Flags & (MGG_TRACE_IMAGE_VALID | MGG_TRACE_PARTIAL_AT_ENABLE);
            event.NativeFrameCounter = MGVK_TraceParent.NativeFrameCounter;
        }
    }
    return event;
}
static void MGVK_TraceRecord(MGG_AndroidTrace_Event event, uint32_t kind,
    bool valid = false, int32_t result = 0, uint32_t skip = MGG_TRACE_SKIP_NONE)
{
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    auto* storage = MGVK_TraceStorageOwner.get();
    if (!MGVK_TraceEnabled.load(std::memory_order_relaxed) || !storage || storage->Sealed ||
        !event.TraceEpoch || event.TraceEpoch != storage->Epoch ||
        event.DeviceId != storage->Base.DeviceId || event.SessionId != storage->Base.SessionId)
    {
        return;
    }
    event.EventKind = kind;
    event.Result = result;
    event.Flags = (event.Flags & ~MGG_TRACE_RESULT_VALID) | (valid ? MGG_TRACE_RESULT_VALID : 0);
    event.SkipReason = skip;
    MGVK_TraceAppend(event);
}
static void MGVK_TraceResourceEvent(MGG_GraphicsDevice* device, VkCommandBuffer command, VkFence fence,
    uint32_t kind, VkResult result, bool adopted = false)
{
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    if (!MGVK_TraceMatches(device))
    {
        return;
    }
    auto& storage = *MGVK_TraceStorageOwner;
    auto event = storage.Base;
    event.EventKind = kind;
    event.Result = result;
    event.Flags |= adopted ? MGG_TRACE_ADOPTED : MGG_TRACE_RESULT_VALID;
    if (command)
    {
        auto* resource = MGVK_TraceFind(storage.Commands, 512, MGVK_TraceBits(command), result == VK_SUCCESS);
        event.RawHandleDiagnostic = MGVK_TraceBits(command);
        if (resource)
        {
            if (kind == MGG_TRACE_COMMAND_BEGIN_RESULT && result == VK_SUCCESS)
            {
                resource->Recording = MGVK_TraceId();
            }
            event.CommandBufferId = resource->Id;
            event.CommandRecordingId = resource->Recording;
        }
    }
    if (fence)
    {
        auto* resource = MGVK_TraceFind(storage.Fences, 1024, MGVK_TraceBits(fence), result == VK_SUCCESS);
        event.RawHandleDiagnostic = MGVK_TraceBits(fence);
        if (resource)
        {
            event.FenceInstanceId = resource->Id;
        }
    }
    MGVK_TraceAppend(event);
}
static void MGVK_TraceFreeResource(MGG_GraphicsDevice* device, VkCommandBuffer command, VkFence fence)
{
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    if (!MGVK_TraceMatches(device))
    {
        return;
    }
    auto& storage = *MGVK_TraceStorageOwner;
    auto* resource = command ? MGVK_TraceFind(storage.Commands, 512, MGVK_TraceBits(command), false) :
        MGVK_TraceFind(storage.Fences, 1024, MGVK_TraceBits(fence), false);
    if (resource)
    {
        if (fence)
        {
            auto event = resource->Submission;
            event.TraceEpoch = storage.Epoch;
            event.DeviceId = storage.Base.DeviceId;
            event.SessionId = storage.Base.SessionId;
            event.FenceInstanceId = resource->Id;
            event.EventKind = MGG_TRACE_FENCE_DESTROY;
            event.Flags &= ~MGG_TRACE_RESULT_VALID;
            MGVK_TraceAppend(event);
        }
        *resource = {};
    }
}
static void MGVK_TraceCommandResult(VkCommandBuffer command, uint32_t kind, VkResult result)
{
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    auto* storage = MGVK_TraceStorageOwner.get();
    if (!MGVK_TraceEnabled.load(std::memory_order_relaxed) || !storage || storage->Sealed)
    {
        return;
    }
    auto* resource = MGVK_TraceFind(storage->Commands, 512, MGVK_TraceBits(command), false);
    if (!resource)
    {
        return;
    }
    if (kind == MGG_TRACE_COMMAND_BEGIN_RESULT && result == VK_SUCCESS)
    {
        resource->Recording = MGVK_TraceId();
    }
    auto event = storage->Base;
    event.EventKind = kind;
    event.Flags |= MGG_TRACE_RESULT_VALID;
    event.Result = result;
    event.CommandBufferId = resource->Id;
    event.CommandRecordingId = resource->Recording;
    MGVK_TraceAppend(event);
}
static void MGVK_TraceFrameCommand(MGG_GraphicsDevice* device, MGVK_Frame& frame)
{
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    if (!MGVK_TraceMatches(device))
    {
        return;
    }
    auto* resource = MGVK_TraceFind(MGVK_TraceStorageOwner->Commands, 512, MGVK_TraceBits(frame.commandBuffer), false);
    if (resource)
    {
        frame.trace.CommandBufferId = resource->Id;
        frame.trace.CommandRecordingId = resource->Recording;
        frame.trace.DrawCommandCount = 0;
    }
}
static MGG_AndroidTrace_Event MGVK_TraceSubmission(MGG_GraphicsDevice* device,
    VkCommandBuffer command, VkFence fence, MGVK_Frame* frame, uint32_t work,
    uint32_t waits, uint32_t signals)
{
    auto event = MGVK_TraceContext(device, frame);
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    if (!MGVK_TraceMatches(device) || event.TraceEpoch != MGVK_TraceStorageOwner->Epoch)
    {
        return {};
    }
    auto& storage = *MGVK_TraceStorageOwner;
    auto* cmd = MGVK_TraceFind(storage.Commands, 512, MGVK_TraceBits(command), false);
    auto* completed = MGVK_TraceFind(storage.Fences, 1024, MGVK_TraceBits(fence), false);
    if (!cmd || !completed)
    {
        storage.Invalid = MGG_TRACE_RESOURCE_LIMIT_INVALID;
    }
    event.SubmitId = MGVK_TraceId();
    event.FencePayloadId = completed && completed->PendingPayload ? completed->PendingPayload : MGVK_TraceId();
    event.FenceInstanceId = completed ? completed->Id : 0;
    event.CommandBufferId = cmd ? cmd->Id : 0;
    event.CommandRecordingId = cmd ? cmd->Recording : 0;
    event.WorkKind = work;
    event.CommandBufferCount = 1;
    event.WaitSemaphoreCount = waits;
    event.SignalSemaphoreCount = signals;
    event.EventKind = MGG_TRACE_SUBMIT_BEGIN;
    MGVK_TraceAppend(event);
    return event;
}
static void MGVK_TraceSubmitted(MGG_GraphicsDevice* device, VkFence fence,
    MGG_AndroidTrace_Event event, VkResult result)
{
    MGVK_TraceRecord(event, MGG_TRACE_SUBMIT_RESULT, true, result);
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    if (!MGVK_TraceMatches(device) || event.TraceEpoch != MGVK_TraceStorageOwner->Epoch)
    {
        return;
    }
    auto* resource = MGVK_TraceFind(MGVK_TraceStorageOwner->Fences, 1024, MGVK_TraceBits(fence), false);
    if (resource)
    {
        event.Flags = (event.Flags & ~MGG_TRACE_RESULT_VALID) | (result == VK_SUCCESS ? MGG_TRACE_RESULT_VALID : 0);
        event.Result = result;
        resource->Submission = event;
        resource->PendingPayload = 0;
    }
}
static void MGVK_TraceFence(MGG_GraphicsDevice* device, VkFence fence, uint32_t kind,
    bool valid = false, VkResult result = VK_SUCCESS)
{
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    if (!MGVK_TraceMatches(device))
    {
        return;
    }
    auto& storage = *MGVK_TraceStorageOwner;
    auto* resource = MGVK_TraceFind(storage.Fences, 1024, MGVK_TraceBits(fence), false);
    auto event = resource && resource->Submission.DeviceId ? resource->Submission : storage.Base;
    event.TraceEpoch = storage.Epoch;
    event.EventKind = kind;
    event.FenceInstanceId = resource ? resource->Id : 0;
    event.RawHandleDiagnostic = MGVK_TraceBits(fence);
    event.Flags = (event.Flags & ~MGG_TRACE_RESULT_VALID) | (valid ? MGG_TRACE_RESULT_VALID : 0);
    event.Result = result;
    event.TimeoutNs = kind == MGG_TRACE_FENCE_WAIT_BEGIN || kind == MGG_TRACE_FENCE_WAIT_RESULT ? UINT64_MAX : 0;
    MGVK_TraceAppend(event);
}
static void MGVK_TraceResetFence(MGG_GraphicsDevice* device, MGVK_Frame& frame, VkResult result)
{
    auto event = MGVK_TraceContext(device, &frame);
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    if (!MGVK_TraceMatches(device) || event.TraceEpoch != MGVK_TraceStorageOwner->Epoch) { return; }
    auto* resource = MGVK_TraceFind(MGVK_TraceStorageOwner->Fences, 1024, MGVK_TraceBits(frame.completedFence), false);
    event.EventKind = MGG_TRACE_FENCE_RESET_RESULT;
    event.Flags |= MGG_TRACE_RESULT_VALID;
    event.Result = result;
    event.RawHandleDiagnostic = MGVK_TraceBits(frame.completedFence);
    event.WorkKind = MGG_TRACE_PRESENT_FINAL;
    if (resource)
    {
        event.FenceInstanceId = resource->Id;
        if (result == VK_SUCCESS)
        {
            resource->PendingPayload = MGVK_TraceId();
            resource->Submission = {};
        }
        event.FencePayloadId = resource->PendingPayload;
    }
    else { MGVK_TraceStorageOwner->Invalid = MGG_TRACE_RESOURCE_LIMIT_INVALID; }
    MGVK_TraceAppend(event);
}
struct MGVK_TraceScope
{
    MGG_AndroidTrace_Event Previous;
    MGVK_TraceScope(MGG_AndroidTrace_Event event) : Previous(MGVK_TraceParent)
    {
        MGVK_TraceParent = event;
    }
    ~MGVK_TraceScope() { MGVK_TraceParent = MGVK_TraceParentInvalidated ? MGG_AndroidTrace_Event{} : Previous; }
};
static uint32_t MGVK_TraceWork(MGG_GraphicsDevice* device, uint32_t requested)
{
    auto context = MGVK_TraceContext(device);
    return context.WorkKind == requested ? requested : MGG_TRACE_TEXTURE_OR_BUFFER_TRANSFER;
}
static void MGVK_TraceRegisterDevice(MGG_GraphicsDevice* device)
{
    device->traceDeviceId = MGVK_TraceId();
    device->traceOwnerTid = MGVK_TraceTid();
    device->traceQueueId = device->queue ? MGVK_TraceId() : 0;
}
static void MGVK_TraceSealDevice(MGG_GraphicsDevice* device)
{
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    if (!MGVK_TraceMatches(device))
    {
        return;
    }
    auto event = MGVK_TraceStorageOwner->Base;
    event.EventKind = MGG_TRACE_DEVICE_DESTROY_END;
    event.Flags &= ~(MGG_TRACE_LIVE_DEVICE | MGG_TRACE_HAS_SWAPCHAIN | MGG_TRACE_RECORDING | MGG_TRACE_IMAGE_VALID);
    MGVK_TraceAppend(event);
    MGVK_TraceStorageOwner->Device = nullptr;
    MGVK_TraceStorageOwner->Sealed = true;
    MGVK_TraceEnabled.store(false, std::memory_order_release);
}
template<class T> static int32_t MGVK_TraceHeader(const T* value)
{
    if (!value || value->Size != sizeof(T))
    {
        return MGG_TRACE_INVALID_ARGUMENT;
    }
    return value->Version == MGG_ANDROID_TRACE_VERSION ? MGG_TRACE_OK : MGG_TRACE_UNSUPPORTED_VERSION;
}

// Called with the registry mutex held. Only owner thread reads mutable device fields.
static int32_t MGVK_TraceCurrent(MGG_AndroidTrace_Context& context, MGG_GraphicsDevice*& selected)
{
    MGP_Android_Lifecycle lifecycle{};
    if (MGP_Android_GetLifecycle(0, &lifecycle) != MGP_ANDROID_OK)
    {
        return MGG_TRACE_DEVICE_UNAVAILABLE;
    }
    const uint64_t tid = MGVK_TraceTid();
    bool foreign = false;
    for (auto* device : MGVK_SnapshotDevices)
    {
        if (device->androidSurface.SessionId != lifecycle.SessionId)
        {
            continue;
        }
        if (device->traceOwnerTid != tid)
        {
            foreign = true;
            continue;
        }
        if (selected)
        {
            return MGG_TRACE_AMBIGUOUS_DEVICE;
        }
        selected = device;
    }
    if (!selected)
    {
        return foreign ? MGG_TRACE_WRONG_THREAD : MGG_TRACE_DEVICE_UNAVAILABLE;
    }
    auto* device = selected;
    context = {};
    context.Size = sizeof(context);
    context.Version = MGG_ANDROID_TRACE_VERSION;
    context.DeviceId = device->traceDeviceId;
    context.SessionId = lifecycle.SessionId;
    context.OwnerNativeTid = tid;
    context.HostSurfaceGeneration = lifecycle.Generation;
    context.AppliedSurfaceGeneration = device->androidSurface.Generation;
    context.LifecycleRevision = lifecycle.Revision;
    context.RetireRevision = lifecycle.RetireRevision;
    context.BindingRevision = lifecycle.Revision;
    context.QueueId = device->traceQueueId;
    context.NativeSurfaceInstanceId = device->surface ? device->traceSurfaceId : 0;
    context.SwapchainGeneration = device->traceSwapchainGeneration;
    context.NativeFrameCounter = device->frame;
    context.ImageIndex = UINT32_MAX;
    context.Flags = MGG_TRACE_LIVE_DEVICE;
    if (device->swapchain)
    {
        context.Flags |= MGG_TRACE_HAS_SWAPCHAIN;
    }
    if (lifecycle.Generation != 0 && lifecycle.Flags == MGP_ANDROID_SURFACE_READY &&
        lifecycle.Generation == device->androidSurface.Generation &&
        lifecycle.RetireRevision <= device->androidSurface.RetiredRevision)
    {
        context.Flags |= MGG_TRACE_LIFECYCLE_READY;
    }
    if (!device->frames.empty())
    {
        auto& frame = device->frames[device->frameIndex];
        context.FrameSequence = frame.trace.FrameSequence;
        context.AcquireAttemptId = frame.trace.AcquireAttemptId;
        context.CommandBufferId = frame.trace.CommandBufferId;
        context.CommandRecordingId = frame.trace.CommandRecordingId;
        context.Flags |= frame.trace.Flags & MGG_TRACE_PARTIAL_AT_ENABLE;
        if (frame.is_recording)
        {
            context.Flags |= MGG_TRACE_RECORDING;
        }
        if (frame.image_index != UINT32_MAX)
        {
            context.ImageIndex = frame.image_index;
            context.Flags |= MGG_TRACE_IMAGE_VALID;
        }
    }
    std::lock_guard<std::mutex> traceLock(MGVK_TraceMutex);
    if (MGVK_TraceMatches(device))
    {
        context.TraceEpoch = MGVK_TraceStorageOwner->Epoch;
        context.Flags |= MGG_TRACE_ENABLED;
    }
    return MGG_TRACE_OK;
}
static bool MGVK_TraceSameBinding(const MGG_AndroidTrace_Context& a, const MGG_AndroidTrace_Context& b)
{
    return a.DeviceId == b.DeviceId && a.SessionId == b.SessionId && a.OwnerNativeTid == b.OwnerNativeTid &&
        a.HostSurfaceGeneration == b.HostSurfaceGeneration && a.AppliedSurfaceGeneration == b.AppliedSurfaceGeneration &&
        a.BindingRevision == b.BindingRevision && a.LifecycleRevision == b.LifecycleRevision &&
        a.RetireRevision == b.RetireRevision && a.NativeSurfaceInstanceId == b.NativeSurfaceInstanceId &&
        a.SwapchainGeneration == b.SwapchainGeneration && a.FrameSequence == b.FrameSequence &&
        a.AcquireAttemptId == b.AcquireAttemptId && a.ImageIndex == b.ImageIndex &&
        a.CommandRecordingId == b.CommandRecordingId && a.CommandBufferId == b.CommandBufferId &&
        a.NativeFrameCounter == b.NativeFrameCounter && a.QueueId == b.QueueId &&
        a.TraceEpoch == b.TraceEpoch && a.Flags == b.Flags && a.ParentOperationId == b.ParentOperationId;
}
uint32_t MGG_AndroidTrace_GetApiVersion() { return MGG_ANDROID_TRACE_VERSION; }
int32_t MGG_AndroidTrace_GetCurrentContext(MGG_AndroidTrace_Context* context)
{
    int32_t status = MGVK_TraceHeader(context);
    if (status != MGG_TRACE_OK)
    {
        return status;
    }
    std::lock_guard<std::mutex> lock(MGVK_SnapshotMutex);
    MGG_GraphicsDevice* device = nullptr;
    MGG_AndroidTrace_Context result{};
    status = MGVK_TraceCurrent(result, device);
    if (status == MGG_TRACE_OK)
    {
        *context = result;
    }
    return status;
}
int32_t MGG_AndroidTrace_Enable(const MGG_AndroidTrace_EnableRequest* request, MGG_AndroidTrace_Control* control)
{
    int32_t status = MGVK_TraceHeader(request);
    if (status == MGG_TRACE_OK) { status = MGVK_TraceHeader(control); }
    if (status == MGG_TRACE_OK) { status = MGVK_TraceHeader(&request->Context); }
    if (status != MGG_TRACE_OK) { return status; }
    if (request->Measurement) { return MGG_TRACE_MEASUREMENT_FORBIDDEN; }
    if (request->Diagnostic != 1 || request->Reserved0 || request->Reserved1 ||
        (!request->CallerRunNonceLo && !request->CallerRunNonceHi)) { return MGG_TRACE_INVALID_ARGUMENT; }
    std::lock_guard<std::mutex> lock(MGVK_SnapshotMutex);
    MGG_GraphicsDevice* device = nullptr;
    MGG_AndroidTrace_Context context{};
    status = MGVK_TraceCurrent(context, device);
    if (status != MGG_TRACE_OK) { return status; }
    if (!MGVK_TraceSameBinding(request->Context, context)) { return MGG_TRACE_STALE_CONTEXT; }

    MGP_Android_Lifecycle observedLifecycle{};
    if (MGP_Android_GetLifecycle(device->androidSurface.SessionId, &observedLifecycle) != MGP_ANDROID_OK ||
        observedLifecycle.Generation != context.HostSurfaceGeneration || observedLifecycle.Revision != context.LifecycleRevision ||
        observedLifecycle.RetireRevision != context.RetireRevision)
    { return MGG_TRACE_STALE_CONTEXT; }

    // Cold enable snapshots pending transfers in the existing transfer->collector order.
    std::lock_guard<std::recursive_mutex> transferLock(device->transferMutex);
    std::lock_guard<std::mutex> traceLock(MGVK_TraceMutex);
    if (MGVK_TraceEnabled.load(std::memory_order_relaxed)) { return MGG_TRACE_BUSY; }
    if (MGVK_TraceIdExhausted.load(std::memory_order_relaxed) || !device->traceDeviceId || !device->traceQueueId)
    { return MGG_TRACE_RESOURCE_LIMIT_INVALID; }
    std::unique_ptr<MGVK_TraceStorage> storage(new(std::nothrow) MGVK_TraceStorage);
    if (!storage) { return MGG_TRACE_RESOURCE_LIMIT_INVALID; }
    storage->Device = device;
    storage->Epoch = MGVK_TraceId();
    storage->Base = MGVK_TraceNativeBase(device);
    storage->Base.TraceEpoch = storage->Epoch;
    storage->Base.HostSurfaceGeneration = context.HostSurfaceGeneration;
    storage->Base.LifecycleRevision = context.LifecycleRevision;
    storage->Base.RetireRevision = context.RetireRevision;
    storage->Base.LifecycleFlags = observedLifecycle.Flags;
    storage->Base.Flags |= context.Flags & MGG_TRACE_LIFECYCLE_READY;
    storage->CallerRunNonceLo = request->CallerRunNonceLo;
    storage->CallerRunNonceHi = request->CallerRunNonceHi;
    MGVK_TraceStorageOwner = std::move(storage);
    for (auto& frame : device->frames)
    {
        frame.trace = {};
        frame.trace.Flags = frame.is_recording ? MGG_TRACE_PARTIAL_AT_ENABLE : 0;
        auto* command = MGVK_TraceFind(MGVK_TraceStorageOwner->Commands, 512, MGVK_TraceBits(frame.commandBuffer), true);
        MGVK_TraceFind(MGVK_TraceStorageOwner->Fences, 1024, MGVK_TraceBits(frame.completedFence), true);
        frame.trace.CommandBufferId = command ? command->Id : 0;
        auto adopted = MGVK_TraceStorageOwner->Base;
        adopted.EventKind = MGG_TRACE_RESOURCE_ADOPTED;
        adopted.Flags |= MGG_TRACE_ADOPTED | frame.trace.Flags;
        adopted.CommandBufferId = command ? command->Id : 0;
        adopted.RawHandleDiagnostic = MGVK_TraceBits(frame.commandBuffer);
        MGVK_TraceAppend(adopted);
        adopted.CommandBufferId = 0;
        auto* fence = MGVK_TraceFind(MGVK_TraceStorageOwner->Fences, 1024, MGVK_TraceBits(frame.completedFence), false);
        adopted.FenceInstanceId = fence ? fence->Id : 0;
        adopted.RawHandleDiagnostic = MGVK_TraceBits(frame.completedFence);
        MGVK_TraceAppend(adopted);
    }
    for (const auto& pending : device->pendingTransfers)
    {
        auto* command = MGVK_TraceFind(MGVK_TraceStorageOwner->Commands, 512, MGVK_TraceBits(pending.cmd), true);
        auto* fence = MGVK_TraceFind(MGVK_TraceStorageOwner->Fences, 1024, MGVK_TraceBits(pending.done), true);
        auto adopted = MGVK_TraceStorageOwner->Base;
        adopted.EventKind = MGG_TRACE_RESOURCE_ADOPTED;
        adopted.Flags |= MGG_TRACE_ADOPTED;
        adopted.WorkKind = MGG_TRACE_TEXTURE_OR_BUFFER_TRANSFER;
        adopted.CommandBufferId = command ? command->Id : 0;
        adopted.FenceInstanceId = fence ? fence->Id : 0;
        if (fence) { fence->Submission = adopted; }
        MGVK_TraceAppend(adopted);
    }
    auto event = MGVK_TraceStorageOwner->Base;
    event.EventKind = MGG_TRACE_TRACE_ENABLED;
    event.CallerRequestIdLo = request->CallerRunNonceLo;
    event.CallerRequestIdHi = request->CallerRunNonceHi;
    event.Flags |= MGG_TRACE_CALLER_ANCHOR;
    MGVK_TraceAppend(event);
    if (MGVK_TraceStorageOwner->Sealed || MGVK_TraceStorageOwner->Invalid || !MGVK_TraceStorageOwner->Epoch ||
        MGVK_TraceIdExhausted.load(std::memory_order_relaxed))
    {
        if (!MGVK_TraceStorageOwner->Invalid)
        { MGVK_TraceStorageOwner->Invalid = MGG_TRACE_RESOURCE_LIMIT_INVALID; }
        MGVK_TraceStorageOwner->Sealed = true;
        MGVK_TraceEnabled.store(false, std::memory_order_release);
        return MGVK_TraceStorageOwner->Invalid;
    }
    control->TraceEpoch = MGVK_TraceStorageOwner->Epoch;
    control->OperationId = 0;
    MGVK_TraceParent = {};
    MGVK_TraceReadbackRestart = 0;
    MGVK_TraceParentInvalidated = false;
    MGVK_TraceEnabled.store(true, std::memory_order_release);
    return MGG_TRACE_OK;
}
int32_t MGG_AndroidTrace_Read(const MGG_AndroidTrace_ReadRequest* request,
    MGG_AndroidTrace_ReadResult* result, MGG_AndroidTrace_Event* events)
{
    int32_t status = MGVK_TraceHeader(request);
    if (status == MGG_TRACE_OK) { status = MGVK_TraceHeader(result); }
    if (status != MGG_TRACE_OK) { return status; }
    if (!request->TraceEpoch || request->Capacity > MGG_ANDROID_TRACE_READ_MAX ||
        request->EventBytes != sizeof(MGG_AndroidTrace_Event) || (request->Capacity && !events)) { return MGG_TRACE_INVALID_ARGUMENT; }
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    auto* storage = MGVK_TraceStorageOwner.get();
    if (!storage) { return MGG_TRACE_NOT_ENABLED; }
    if (storage->Epoch != request->TraceEpoch) { return MGG_TRACE_STALE_EPOCH; }
    if (request->AfterSequence > storage->Attempted) { return MGG_TRACE_INVALID_ARGUMENT; }
    MGG_AndroidTrace_ReadResult output{};
    output.Size = sizeof(output); output.Version = MGG_ANDROID_TRACE_VERSION;
    output.TraceEpoch = storage->Epoch; output.FirstSequence = storage->Count ? storage->Events[0].Sequence : 0;
    output.LastSequence = storage->Count ? storage->Events[storage->Count - 1].Sequence : 0;
    output.AttemptedSequence = storage->Attempted; output.OverflowCount = storage->Overflow;
    output.Capacity = MGG_ANDROID_TRACE_CAPACITY; output.Count = storage->Count;
    output.Sealed = storage->Sealed; output.InvalidStatus = storage->Invalid;
    const uint64_t start = request->AfterSequence;
    for (uint64_t i = start; i < storage->Count && output.Copied < request->Capacity; ++i)
    {
        events[output.Copied++] = storage->Events[i];
    }
    *result = output;
    return storage->Invalid;
}
int32_t MGG_AndroidTrace_Disable(MGG_AndroidTrace_Control* control)
{
    int32_t status = MGVK_TraceHeader(control);
    if (status != MGG_TRACE_OK) { return status; }
    if (!control->TraceEpoch || control->OperationId) { return MGG_TRACE_INVALID_ARGUMENT; }
    std::lock_guard<std::mutex> lock(MGVK_TraceMutex);
    auto* storage = MGVK_TraceStorageOwner.get();
    if (!storage) { return MGG_TRACE_NOT_ENABLED; }
    if (storage->Epoch != control->TraceEpoch) { return MGG_TRACE_STALE_EPOCH; }
    if (!storage->Sealed)
    {
        auto event = storage->Base;
        event.EventKind = MGG_TRACE_TRACE_SEALED;
        MGVK_TraceAppend(event);
        storage->Sealed = true;
        MGVK_TraceEnabled.store(false, std::memory_order_release);
    }
    return storage->Invalid;
}
int32_t MGG_AndroidTrace_GetClock(MGG_AndroidTrace_Clock* output)
{
    int32_t status = MGVK_TraceHeader(output);
    if (status != MGG_TRACE_OK) { return status; }
    MGG_AndroidTrace_Clock clock{};
    clock.Size = sizeof(clock); clock.Version = MGG_ANDROID_TRACE_VERSION;
    clock.NativePid = getpid(); clock.NativeTid = MGVK_TraceTid();
    timespec monoResolution{}, realResolution{};
    if (!MGVK_TraceTime(CLOCK_MONOTONIC, clock.MonotonicBeforeNs) ||
        !MGVK_TraceTime(CLOCK_REALTIME, clock.RealtimeNs) || !MGVK_TraceTime(CLOCK_MONOTONIC, clock.MonotonicAfterNs) ||
        clock_getres(CLOCK_MONOTONIC, &monoResolution) != 0 || clock_getres(CLOCK_REALTIME, &realResolution) != 0)
    { return MGG_TRACE_CLOCK_FAILURE; }
    clock.MonotonicResolutionNs = monoResolution.tv_sec * UINT64_C(1000000000) + monoResolution.tv_nsec;
    clock.RealtimeResolutionNs = realResolution.tv_sec * UINT64_C(1000000000) + realResolution.tv_nsec;
    *output = clock;
    return MGG_TRACE_OK;
}
static int32_t MGVK_TraceValidateAnchor(const MGG_AndroidTrace_Anchor* anchor, MGG_GraphicsDevice*& device)
{
    int32_t status = MGVK_TraceHeader(anchor);
    if (status == MGG_TRACE_OK) { status = MGVK_TraceHeader(&anchor->Context); }
    if (status != MGG_TRACE_OK) { return status; }
    if (anchor->Reserved || (!anchor->CallerRequestIdLo && !anchor->CallerRequestIdHi) || !anchor->CallerStopwatchFrequency)
    { return MGG_TRACE_INVALID_ARGUMENT; }
    MGG_AndroidTrace_Context current{};
    status = MGVK_TraceCurrent(current, device);
    if (status != MGG_TRACE_OK) { return status; }
    if (!MGVK_TraceSameBinding(anchor->Context, current) ||
        (current.Flags & (MGG_TRACE_LIFECYCLE_READY | MGG_TRACE_IMAGE_VALID | MGG_TRACE_RECORDING)) !=
            (MGG_TRACE_LIFECYCLE_READY | MGG_TRACE_IMAGE_VALID | MGG_TRACE_RECORDING)) { return MGG_TRACE_STALE_CONTEXT; }
    if (!current.FrameSequence || !current.AcquireAttemptId || (current.Flags & MGG_TRACE_PARTIAL_AT_ENABLE)) { return MGG_TRACE_STALE_CONTEXT; }
    if (!anchor->TraceEpoch || current.TraceEpoch != anchor->TraceEpoch) { return MGG_TRACE_STALE_EPOCH; }
    return MGG_TRACE_OK;
}
#if defined(MGG_ANDROID_TRACE_HOST_TEST)

// Deterministic host-only barriers; no callback or hook exists in runtime builds.
static void (*MGVK_TraceControlTestBarrier)(uint32_t stage) = nullptr;
static void MGVK_TraceControlCheckpoint(uint32_t stage)
{
    if (MGVK_TraceControlTestBarrier) { MGVK_TraceControlTestBarrier(stage); }
}
#else
#define MGVK_TraceControlCheckpoint(stage)
#endif

// Caller holds collector lock. Success means the exact anchor was committed.
static int32_t MGVK_TraceCommitAnchor(MGG_GraphicsDevice* device, MGG_AndroidTrace_Event event, uint64_t epoch)
{
    auto* storage = MGVK_TraceStorageOwner.get();
    if (!storage) { return MGG_TRACE_NOT_ENABLED; }
    if (!epoch || storage->Epoch != epoch || event.TraceEpoch != epoch) { return MGG_TRACE_STALE_EPOCH; }
    if (!MGVK_TraceMatches(device)) { return MGG_TRACE_NOT_ENABLED; }
    if (storage->Invalid) { return storage->Invalid; }
    if (!event.DeviceId || event.DeviceId != device->traceDeviceId || event.DeviceId != storage->Base.DeviceId ||
        !event.SessionId || event.SessionId != storage->Base.SessionId || !event.OperationId)
    { return MGG_TRACE_STALE_CONTEXT; }
    event.EventKind = MGG_TRACE_MANAGED_CALLER_ANCHOR;
    event.Flags = (event.Flags & ~MGG_TRACE_RESULT_VALID) | MGG_TRACE_CALLER_ANCHOR;
    event.Result = 0;
    const uint32_t before = storage->Count;
    MGVK_TraceAppend(event);
    if (storage->Invalid) { return storage->Invalid; }
    if (storage->Sealed || storage->Count != before + 1 || !MGVK_TraceEnabled.load(std::memory_order_relaxed))
    { return MGG_TRACE_NOT_ENABLED; }
    return MGG_TRACE_OK;
}
int32_t MGG_AndroidTrace_BeginOperation(const MGG_AndroidTrace_Anchor* anchor, MGG_AndroidTrace_Control* control)
{
    int32_t status = MGVK_TraceHeader(control);
    if (status != MGG_TRACE_OK) { return status; }
    std::lock_guard<std::mutex> registryLock(MGVK_SnapshotMutex);
    MGG_GraphicsDevice* device = nullptr;
    status = MGVK_TraceValidateAnchor(anchor, device);
    if (status != MGG_TRACE_OK) { return status; }
    if (anchor->OperationId || (MGVK_TraceParent.OperationId && MGVK_TraceParent.TraceEpoch == anchor->TraceEpoch)) { return MGG_TRACE_BUSY; }
    auto event = MGVK_TraceContext(device, &device->frames[device->frameIndex]);
    event.OperationId = MGVK_TraceId(); event.WorkKind = MGG_TRACE_WORK_NONE;
    event.Flags |= MGG_TRACE_CALLER_ANCHOR;
    event.CallerRequestIdLo = anchor->CallerRequestIdLo; event.CallerRequestIdHi = anchor->CallerRequestIdHi;
    event.CallerStopwatchTick = anchor->CallerStopwatchTick; event.CallerStopwatchFrequency = anchor->CallerStopwatchFrequency;
    event.CallerFrame = anchor->CallerFrame; event.CallerGeneration = anchor->CallerGeneration;
    event.CallerAnchorKind = anchor->CallerAnchorKind;
    MGVK_TraceControlCheckpoint(1);
    std::lock_guard<std::mutex> traceLock(MGVK_TraceMutex);
    status = MGVK_TraceCommitAnchor(device, event, anchor->TraceEpoch);
    if (status != MGG_TRACE_OK) { return status; }
    MGVK_TraceControlCheckpoint(3);
    MGVK_TraceParent = event;
    MGVK_TraceReadbackRestart = 0;
    MGVK_TraceParentInvalidated = false;
    control->TraceEpoch = event.TraceEpoch; control->OperationId = event.OperationId;
    return MGG_TRACE_OK;
}
int32_t MGG_AndroidTrace_EndOperation(const MGG_AndroidTrace_Anchor* anchor)
{
    int32_t status = MGVK_TraceHeader(anchor);
    if (status != MGG_TRACE_OK) { return status; }
    if (!anchor->OperationId || anchor->OperationId != MGVK_TraceParent.OperationId ||
        anchor->TraceEpoch != MGVK_TraceParent.TraceEpoch || anchor->CallerRequestIdLo != MGVK_TraceParent.CallerRequestIdLo ||
        anchor->CallerRequestIdHi != MGVK_TraceParent.CallerRequestIdHi)
    {

        // A wrong end request cannot clear another valid admitted operation.
        return MGG_TRACE_STALE_CONTEXT;
    }
    std::lock_guard<std::mutex> registryLock(MGVK_SnapshotMutex);
    MGG_GraphicsDevice* device = nullptr;
    status = MGVK_TraceValidateAnchor(anchor, device);
    MGP_Android_Lifecycle lifecycle{};
    if (status == MGG_TRACE_OK && (MGP_Android_GetLifecycle(device->androidSurface.SessionId, &lifecycle) != MGP_ANDROID_OK ||
        !MGVK_TraceParentBinding(device, lifecycle)))
    { status = MGG_TRACE_STALE_CONTEXT; }
    if (status != MGG_TRACE_OK)
    {
        if (!device || (MGP_Android_GetLifecycle(device->androidSurface.SessionId, &lifecycle) == MGP_ANDROID_OK &&
            MGVK_TraceParentBinding(device, lifecycle)))
        { return status; }
        MGVK_TraceParent = {};
        MGVK_TraceReadbackRestart = 0;
        MGVK_TraceParentInvalidated = true;
        std::lock_guard<std::mutex> traceLock(MGVK_TraceMutex);
        if (MGVK_TraceStorageOwner && MGVK_TraceStorageOwner->Epoch == anchor->TraceEpoch)
        { MGVK_TraceStorageOwner->Invalid = MGG_TRACE_STALE_CONTEXT; }
        return status;
    }
    auto event = MGVK_TraceParent;
    event.CallerAnchorKind = anchor->CallerAnchorKind;
    event.CallerStopwatchTick = anchor->CallerStopwatchTick;
    MGVK_TraceControlCheckpoint(2);
    std::lock_guard<std::mutex> traceLock(MGVK_TraceMutex);
    status = MGVK_TraceCommitAnchor(device, event, anchor->TraceEpoch);
    if (status != MGG_TRACE_OK) { return status; }
    MGVK_TraceControlCheckpoint(4);
    MGVK_TraceParent = {};
    MGVK_TraceReadbackRestart = 0;
    return MGG_TRACE_OK;
}
