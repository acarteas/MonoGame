// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

#pragma once

// This host ABI belongs only to the Android Native runtime. The test define
// exposes the very same implementation to the standalone host test target.
#if defined(__ANDROID__) || defined(MGP_ANDROID_HOST_TEST)
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#include "api_common.h"

// Android's C calling convention is Cdecl; only 32-bit x86 needs an attribute.
#if defined(__i386__)
#define MGP_ANDROID_CALL __attribute__((cdecl))
#else
#define MGP_ANDROID_CALL
#endif

#define MGP_ANDROID_HOST_API_VERSION UINT32_C(2)
#define MGP_ANDROID_SESSION_ARGUMENT "--mgp-android-session="

// All ABI status results are int32_t, independent of native enum size.
enum MGP_Android_HostStatus
{
    MGP_ANDROID_OK = 0,
    MGP_ANDROID_INVALID_ARGUMENT = -1,
    MGP_ANDROID_STALE_SESSION = -2,
    MGP_ANDROID_ALREADY_REGISTERED = -3,
    MGP_ANDROID_BUSY = -4,
    MGP_ANDROID_NOT_REGISTERED = -5,
    MGP_ANDROID_ALREADY_RUN = -6,
    MGP_ANDROID_CALLBACK_EXCEPTION = -7
};

typedef int32_t (MGP_ANDROID_CALL *MGP_Android_RunCallback)(void* userData);

MG_EXPORT uint32_t MGP_ANDROID_CALL MGP_Android_GetHostApiVersion(void);

// Session IDs must be nonzero and strictly increase for the lifetime of the
// loaded library (including cancelled sessions). Never wrap or reuse an ID.
// Null callbacks/zero IDs return INVALID_ARGUMENT. Re-registering the current
// owner returns ALREADY_REGISTERED; an older/cleared ID returns STALE_SESSION.
// A newer ID while an owner exists returns BUSY, without consuming that ID.
// userData may be null. The caller roots the delegate and userData until a
// matching Clear returns OK. Registration does not call user code.
MG_EXPORT int32_t MGP_ANDROID_CALL MGP_Android_RegisterRunCallback(
    uint64_t sessionId, MGP_Android_RunCallback run, void* userData);

// Only the owning ID can clear (otherwise STALE_SESSION; zero is invalid).
// Clear before entry cancels it; Clear after completion releases it. While
// running, Clear returns BUSY and the caller MUST retain all callback state.
// Even after callback return, a new session requires a successful owner Clear.
MG_EXPORT int32_t MGP_ANDROID_CALL MGP_Android_ClearRunCallback(uint64_t sessionId);

// A single session's observed SurfaceHolder state. Fixed layout on both ABIs.
// Generation increases only for a real surfaceCreated callback, never for resize.
// RetireRevision closes draw admission immediately; only the game thread may ack
// after retiring all swapchain/surface GPU work. No UI pointer crosses this ABI.
#define MGP_ANDROID_SURFACE_READY UINT32_C(1)
#define MGP_ANDROID_PAUSED UINT32_C(2)
#define MGP_ANDROID_EXITING UINT32_C(4)
#define MGP_ANDROID_QUARANTINED UINT32_C(8)
typedef struct MGP_Android_Lifecycle
{
    uint64_t SessionId;
    uint64_t Generation;
    uint64_t Revision;
    uint64_t RetireRevision;
    uint64_t AcknowledgedRevision;
    uint32_t Flags;
    uint32_t Reserved;
} MGP_Android_Lifecycle;

// Publish never calls SDL, Vulkan or managed code. Exiting/quarantine are sticky.
MG_EXPORT int32_t MGP_ANDROID_CALL MGP_Android_PublishLifecycle(uint64_t sessionId,
    uint64_t generation, uint32_t flags, uint64_t* revision);

// sessionId=0 is a game-thread query of the current owner. Other IDs must match.
MG_EXPORT int32_t MGP_ANDROID_CALL MGP_Android_GetLifecycle(uint64_t sessionId,
    MGP_Android_Lifecycle* snapshot);

// Acks cover exactly the caller's retired snapshot, never a later request.
// While Running this is rejected off the owning SDL_main thread. Consumers bind
// the session once at construction; every later read and ack passes that ID.
MG_EXPORT int32_t MGP_ANDROID_CALL MGP_Android_AcknowledgeRetired(uint64_t sessionId,
    uint64_t revision);

// Finite wait; releases hostMutex while asleep. BUSY means NO permission to
// release SDL's native window. The caller must quarantine instead of falling through.
MG_EXPORT int32_t MGP_ANDROID_CALL MGP_Android_WaitRetired(uint64_t sessionId,
    uint64_t revision, uint32_t timeoutMilliseconds);

#ifdef __cplusplus
struct MGG_GraphicsDevice;

// Game-thread checkpoint. prepare=0 retires pending work without acquiring;
// return indicates a valid recording frame, not just a ready application state.
MG_EXPORT mgbyte MGG_GraphicsDevice_ProcessAndroidLifecycle(MGG_GraphicsDevice* device, mgbyte prepare);
#endif

// SDL2 nativeRunMain calls this exact Cdecl signature on its game thread.
// The managed activity must return exactly one GetArguments() string:
// MGP_ANDROID_SESSION_ARGUMENT followed by its canonical decimal session ID
// (no sign, whitespace or leading zeroes). SDL supplies argv[0], so argc is 2.
// Missing/malformed arguments return INVALID_ARGUMENT. A nonmatching ID returns
// STALE_SESSION (or NOT_REGISTERED when no session has ever been registered).
// Concurrent entry returns BUSY; repeated completed entry returns ALREADY_RUN.
// The callback runs synchronously at most once, with no new thread or runtime.
// After callback/disposal returns, SDL_main calls SDL_Quit on that same thread
// before completing the registration; Clear stays BUSY throughout this cleanup.
// Java may destroy Android JNI state only after joining this SDL game thread.
// SDL_main returns its int32_t result unchanged, or CALLBACK_EXCEPTION for a
// native C++ exception. Managed callbacks MUST catch managed exceptions and
// return an error: no managed exception may cross this C ABI. Status values
// and callback result codes share this return channel; choose app codes wisely.
MG_EXPORT int MGP_ANDROID_CALL SDL_main(int argc, char** argv);
#endif
