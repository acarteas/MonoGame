// Separately versioned, opt-in Android Debug Vulkan diagnostics. Never scanout proof.
#pragma once
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#include "api_common.h"

#define MGG_ANDROID_TRACE_VERSION UINT32_C(1)
#define MGG_ANDROID_TRACE_CAPACITY UINT32_C(32768)
#define MGG_ANDROID_TRACE_EVENT_BYTES UINT32_C(320)
#define MGG_ANDROID_TRACE_READ_MAX UINT32_C(256)

// No bool or native enum crosses this ABI. All enums below name fixed-width fields.
enum MGG_AndroidTrace_Status
{
    MGG_TRACE_OK = 0, MGG_TRACE_INVALID_ARGUMENT = -1,
    MGG_TRACE_UNSUPPORTED_VERSION = -2, MGG_TRACE_NOT_ENABLED = -3,
    MGG_TRACE_STALE_EPOCH = -4, MGG_TRACE_WRONG_THREAD = -5,
    MGG_TRACE_DEVICE_UNAVAILABLE = -6, MGG_TRACE_AMBIGUOUS_DEVICE = -7,
    MGG_TRACE_STALE_CONTEXT = -8, MGG_TRACE_MEASUREMENT_FORBIDDEN = -9,
    MGG_TRACE_BUSY = -10, MGG_TRACE_OVERFLOW_INVALID = -11,
    MGG_TRACE_RESOURCE_LIMIT_INVALID = -12, MGG_TRACE_CLOCK_FAILURE = -13
};
enum MGG_AndroidTrace_Flags
{
    MGG_TRACE_RESULT_VALID = 1, MGG_TRACE_IMAGE_VALID = 2,
    MGG_TRACE_RECORDING = 4, MGG_TRACE_LIFECYCLE_READY = 8,
    MGG_TRACE_HAS_SWAPCHAIN = 16, MGG_TRACE_LIVE_DEVICE = 32,
    MGG_TRACE_ENABLED = 64, MGG_TRACE_PARTIAL_AT_ENABLE = 128,
    MGG_TRACE_CALLER_ANCHOR = 256, MGG_TRACE_ADOPTED = 512,
    MGG_TRACE_NATIVE_RETURN_VALID = 1024
};
enum MGG_AndroidTrace_WorkKind
{
    MGG_TRACE_WORK_NONE = 0, MGG_TRACE_PRESENT_FINAL = 1,
    MGG_TRACE_READBACK_RENDER_FLUSH = 2, MGG_TRACE_READBACK_COPY = 3,
    MGG_TRACE_TEXTURE_OR_BUFFER_TRANSFER = 4, MGG_TRACE_BACKBUFFER_READBACK = 5
};
enum MGG_AndroidTrace_SkipReason
{
    MGG_TRACE_SKIP_NONE = 0, MGG_TRACE_SKIP_LIFECYCLE = 1,
    MGG_TRACE_SKIP_NO_SWAPCHAIN = 2, MGG_TRACE_SKIP_NO_FRAMES = 3,
    MGG_TRACE_SKIP_NO_RECORDING = 4, MGG_TRACE_SKIP_ACQUIRE_REJECTED = 5,
    MGG_TRACE_SKIP_NO_VALID_FRAME = 6
};
enum MGG_AndroidTrace_EventKind
{
    MGG_TRACE_TRACE_ENABLED = 1,
    MGG_TRACE_TRACE_SEALED = 2,
    MGG_TRACE_RESOURCE_ADOPTED = 3,
    MGG_TRACE_DEVICE_DESTROY_BEGIN = 4,
    MGG_TRACE_LIFECYCLE_OBSERVED = 5,
    MGG_TRACE_LIFECYCLE_SKIP = 6,
    MGG_TRACE_RETIRE_BEGIN = 7,
    MGG_TRACE_DEVICE_IDLE_RESULT = 8,
    MGG_TRACE_RETIRE_END = 9,
    MGG_TRACE_SURFACE_CREATE_RESULT = 10,
    MGG_TRACE_SWAPCHAIN_RECREATE_BEGIN = 11,
    MGG_TRACE_SWAPCHAIN_CREATE_RESULT = 12,
    MGG_TRACE_SWAPCHAIN_RECREATE_END = 13,
    MGG_TRACE_COMMAND_RESET_RESULT = 14,
    MGG_TRACE_COMMAND_BEGIN_RESULT = 15,
    MGG_TRACE_COMMAND_END_RESULT = 16,
    MGG_TRACE_FENCE_CREATE_RESULT = 17,
    MGG_TRACE_FENCE_RESET_RESULT = 18,
    MGG_TRACE_FENCE_DESTROY = 19,
    MGG_TRACE_ACQUIRE_BEGIN = 20,
    MGG_TRACE_ACQUIRE_END = 21,
    MGG_TRACE_BEGIN_FRAME_RETURN = 22,
    MGG_TRACE_PRESENT_BEGIN = 23,
    MGG_TRACE_PRESENT_SKIP = 24,
    MGG_TRACE_SUBMIT_BEGIN = 25,
    MGG_TRACE_SUBMIT_RESULT = 26,
    MGG_TRACE_QUEUE_PRESENT_BEGIN = 27,
    MGG_TRACE_QUEUE_PRESENT_RESULT = 28,
    MGG_TRACE_FENCE_WAIT_BEGIN = 29,
    MGG_TRACE_FENCE_WAIT_RESULT = 30,
    MGG_TRACE_FENCE_STATUS_RESULT = 31,
    MGG_TRACE_BACKBUFFER_READBACK_BEGIN = 32,
    MGG_TRACE_BACKBUFFER_READBACK_END = 33,
    MGG_TRACE_MANAGED_CALLER_ANCHOR = 34,
    MGG_TRACE_PRESENT_RETURN = 35,
    MGG_TRACE_COMMAND_ALLOCATE_RESULT = 36,
    MGG_TRACE_DEVICE_DESTROY_END = 37
};

typedef struct MGG_AndroidTrace_Context
{
    uint32_t Size, Version;
    uint64_t DeviceId;
    uint64_t SessionId;
    uint64_t OwnerNativeTid;
    uint64_t HostSurfaceGeneration;
    uint64_t AppliedSurfaceGeneration;
    uint64_t LifecycleRevision;
    uint64_t RetireRevision;
    uint64_t QueueId;
    uint64_t NativeSurfaceInstanceId;
    uint64_t SwapchainGeneration;
    uint64_t FrameSequence;
    uint64_t AcquireAttemptId;
    uint64_t CommandBufferId;
    uint64_t CommandRecordingId;
    uint64_t TraceEpoch;
    uint64_t BindingRevision;
    uint64_t NativeFrameCounter;
    uint64_t ParentOperationId;
    uint32_t ImageIndex, Flags;
} MGG_AndroidTrace_Context;

// RESULT_VALID=false means no VkResult exists; Result=0 must never imply success.
// RawHandleDiagnostic is supplemental only. Caller* values are unverified managed input.
typedef struct MGG_AndroidTrace_Event
{
    uint32_t Size, Version;
    uint64_t Sequence;
    uint64_t MonotonicNs;
    uint64_t TraceEpoch;
    uint64_t DeviceId;
    uint64_t SessionId;
    uint64_t NativeTid;
    uint64_t HostSurfaceGeneration;
    uint64_t AppliedSurfaceGeneration;
    uint64_t LifecycleRevision;
    uint64_t RetireRevision;
    uint64_t NativeSurfaceInstanceId;
    uint64_t SwapchainGeneration;
    uint64_t QueueId;
    uint64_t FrameSequence;
    uint64_t AcquireAttemptId;
    uint64_t CommandBufferId;
    uint64_t CommandRecordingId;
    uint64_t ParentOperationId;
    uint64_t OperationId;
    uint64_t SubmitId;
    uint64_t FenceInstanceId;
    uint64_t FencePayloadId;
    uint64_t TimeoutNs;
    uint64_t RawHandleDiagnostic;
    uint64_t CallerRequestIdLo;
    uint64_t CallerRequestIdHi;
    uint64_t CallerStopwatchTick;
    uint64_t CallerStopwatchFrequency;
    uint64_t CallerFrame;
    uint64_t CallerGeneration;
    uint64_t NativeFrameCounter;
    uint64_t NativeReturnValue;
    uint32_t EventKind;
    uint32_t WorkKind;
    int32_t Result;
    uint32_t Flags;
    uint32_t ImageIndex;
    uint32_t WaitSemaphoreCount;
    uint32_t SignalSemaphoreCount;
    uint32_t DrawCommandCount;
    uint32_t CommandBufferCount;
    uint32_t CallerAnchorKind;
    uint32_t LifecycleFlags;
    uint32_t SkipReason;
    uint32_t Reserved1;
    uint32_t Reserved2;
} MGG_AndroidTrace_Event;

typedef struct MGG_AndroidTrace_EnableRequest
{
    uint32_t Size, Version;
    MGG_AndroidTrace_Context Context;
    uint64_t CallerRunNonceLo, CallerRunNonceHi;
    uint32_t Diagnostic, Measurement, Reserved0, Reserved1;
} MGG_AndroidTrace_EnableRequest;

typedef struct MGG_AndroidTrace_ReadRequest
{
    uint32_t Size, Version;
    uint64_t TraceEpoch, AfterSequence;
    uint32_t Capacity, EventBytes;
} MGG_AndroidTrace_ReadRequest;

typedef struct MGG_AndroidTrace_ReadResult
{
    uint32_t Size, Version;
    uint64_t TraceEpoch, FirstSequence, LastSequence, AttemptedSequence, OverflowCount;
    uint32_t Capacity, Count, Copied, Sealed;
    int32_t InvalidStatus;
    uint32_t Reserved;
} MGG_AndroidTrace_ReadResult;

typedef struct MGG_AndroidTrace_Control
{
    uint32_t Size, Version;
    uint64_t TraceEpoch, OperationId;
} MGG_AndroidTrace_Control;

typedef struct MGG_AndroidTrace_Anchor
{
    uint32_t Size, Version;
    MGG_AndroidTrace_Context Context;
    uint64_t TraceEpoch, OperationId, CallerRequestIdLo, CallerRequestIdHi;
    uint64_t CallerStopwatchTick, CallerStopwatchFrequency, CallerFrame, CallerGeneration;
    uint32_t CallerAnchorKind, Reserved;
} MGG_AndroidTrace_Anchor;

typedef struct MGG_AndroidTrace_Clock
{
    uint32_t Size, Version;
    uint64_t NativePid, NativeTid, MonotonicBeforeNs, RealtimeNs, MonotonicAfterNs;
    uint64_t MonotonicResolutionNs, RealtimeResolutionNs;
} MGG_AndroidTrace_Clock;

// Only Android Debug exports these symbols. Desktop and Android Release do not.
// Context/Enable/BeginOperation/EndOperation require the native owning game TID.
// Read/Disable use an opaque epoch and never dereference a destroyed device.
// Disable seals, preserving readable records; next Enable replaces sealed storage.
// Measurement is a caller declaration; consumers must enforce their real mode.
#if (defined(__ANDROID__) && defined(DEBUG)) || defined(MGG_ANDROID_TRACE_HOST_TEST)
MG_EXPORT uint32_t MGG_AndroidTrace_GetApiVersion(void);
MG_EXPORT int32_t MGG_AndroidTrace_GetCurrentContext(MGG_AndroidTrace_Context* context);
MG_EXPORT int32_t MGG_AndroidTrace_Enable(const MGG_AndroidTrace_EnableRequest* request, MGG_AndroidTrace_Control* control);
MG_EXPORT int32_t MGG_AndroidTrace_Read(const MGG_AndroidTrace_ReadRequest* request, MGG_AndroidTrace_ReadResult* result, MGG_AndroidTrace_Event* events);
MG_EXPORT int32_t MGG_AndroidTrace_Disable(MGG_AndroidTrace_Control* control);
MG_EXPORT int32_t MGG_AndroidTrace_GetClock(MGG_AndroidTrace_Clock* clock);
MG_EXPORT int32_t MGG_AndroidTrace_BeginOperation(const MGG_AndroidTrace_Anchor* anchor, MGG_AndroidTrace_Control* control);
MG_EXPORT int32_t MGG_AndroidTrace_EndOperation(const MGG_AndroidTrace_Anchor* anchor);
#endif
