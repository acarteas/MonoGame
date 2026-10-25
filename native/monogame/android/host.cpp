// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

#include "api_MGP_Android.h"

#if defined(__ANDROID__) || defined(MGP_ANDROID_HOST_TEST)
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <SDL.h>

namespace
{
enum class RunState { Empty, Registered, Running, Completed };

std::mutex hostMutex;
uint64_t newestSession = 0;
uint64_t ownerSession = 0;
MGP_Android_RunCallback callback = nullptr;
void* callbackData = nullptr;
RunState state = RunState::Empty;
MGP_Android_Lifecycle lifecycle{};
std::condition_variable retired;
std::thread::id gameThread;


bool ParseSession(int argc, char** argv, uint64_t& session)
{
    if (argc != 2 || argv == nullptr || argv[1] == nullptr)
    {
        return false;
    }
    constexpr char prefix[] = MGP_ANDROID_SESSION_ARGUMENT;
    if (std::strncmp(argv[1], prefix, sizeof(prefix) - 1) != 0)
    {
        return false;
    }
    const char* digit = argv[1] + sizeof(prefix) - 1;
    if (*digit < '1' || *digit > '9')
    {
        return false;
    }
    session = 0;
    for (; *digit != '\0'; ++digit)
    {
        if (*digit < '0' || *digit > '9')
        {
            return false;
        }
        const uint64_t value = static_cast<uint64_t>(*digit - '0');
        if (session > (std::numeric_limits<uint64_t>::max() - value) / 10)
        {
            return false;
        }
        session = session * 10 + value;
    }
    return true;
}
}

uint32_t MGP_ANDROID_CALL MGP_Android_GetHostApiVersion(void)
{
    return MGP_ANDROID_HOST_API_VERSION;
}

int32_t MGP_ANDROID_CALL MGP_Android_RegisterRunCallback(
    uint64_t sessionId, MGP_Android_RunCallback run, void* userData)
{
    if (sessionId == 0 || run == nullptr)
    {
        return MGP_ANDROID_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(hostMutex);
    if (sessionId <= newestSession)
    {
        return sessionId == ownerSession ? MGP_ANDROID_ALREADY_REGISTERED : MGP_ANDROID_STALE_SESSION;
    }
    if (state != RunState::Empty)
    {
        return MGP_ANDROID_BUSY;
    }
    newestSession = ownerSession = sessionId;
    lifecycle = {};
    lifecycle.SessionId = sessionId;
    lifecycle.Flags = MGP_ANDROID_PAUSED;
    callback = run;
    callbackData = userData;
    state = RunState::Registered;
    return MGP_ANDROID_OK;
}

int32_t MGP_ANDROID_CALL MGP_Android_ClearRunCallback(uint64_t sessionId)
{
    if (sessionId == 0)
    {
        return MGP_ANDROID_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(hostMutex);
    if (sessionId != ownerSession)
    {
        return MGP_ANDROID_STALE_SESSION;
    }
    if (state == RunState::Running || (lifecycle.Flags & MGP_ANDROID_QUARANTINED) != 0)
    {
        return MGP_ANDROID_BUSY;
    }
    callback = nullptr;
    callbackData = nullptr;
    ownerSession = 0;
    state = RunState::Empty;
    lifecycle = {};
    retired.notify_all();
    return MGP_ANDROID_OK;
}

int32_t MGP_ANDROID_CALL MGP_Android_PublishLifecycle(uint64_t sessionId,
    uint64_t generation, uint32_t flags, uint64_t* revision)
{
    if (sessionId == 0 || revision == nullptr || (flags & ~UINT32_C(15)) != 0 ||
        ((flags & MGP_ANDROID_SURFACE_READY) != 0 && generation == 0))
    {
        return MGP_ANDROID_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(hostMutex);
    if (sessionId != ownerSession || generation < lifecycle.Generation)
    {
        return MGP_ANDROID_STALE_SESSION;
    }
    if (lifecycle.Revision == UINT64_MAX)
    {
        return MGP_ANDROID_BUSY;
    }
    flags |= lifecycle.Flags & (MGP_ANDROID_EXITING | MGP_ANDROID_QUARANTINED);
    lifecycle.Generation = generation;
    lifecycle.Flags = flags;
    if ((flags & MGP_ANDROID_QUARANTINED) != 0)
    {
        retired.notify_all();
    }
    *revision = ++lifecycle.Revision;
    if ((flags & MGP_ANDROID_SURFACE_READY) == 0 || (flags & ~MGP_ANDROID_SURFACE_READY) != 0)
    {
        lifecycle.RetireRevision = lifecycle.Revision;
    }

    // Before SDL_main entry or after its callback+SDL shutdown there is no GPU
    // consumer. During Running only game-thread resource retirement can ack.
    if (state != RunState::Running && (flags & MGP_ANDROID_QUARANTINED) == 0)
    {
        lifecycle.AcknowledgedRevision = lifecycle.RetireRevision;
        retired.notify_all();
    }
    return MGP_ANDROID_OK;
}

int32_t MGP_ANDROID_CALL MGP_Android_GetLifecycle(uint64_t sessionId, MGP_Android_Lifecycle* snapshot)
{
    if (snapshot == nullptr)
    {
        return MGP_ANDROID_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(hostMutex);
    *snapshot = {};
    if (sessionId == 0 && (state != RunState::Running || std::this_thread::get_id() != gameThread))
    {
        return MGP_ANDROID_STALE_SESSION;
    }
    if (ownerSession == 0 || (sessionId != 0 && sessionId != ownerSession))
    {
        return MGP_ANDROID_STALE_SESSION;
    }
    *snapshot = lifecycle;
    return MGP_ANDROID_OK;
}

int32_t MGP_ANDROID_CALL MGP_Android_AcknowledgeRetired(uint64_t sessionId, uint64_t revision)
{
    std::lock_guard<std::mutex> lock(hostMutex);
    if (sessionId == 0 || sessionId != ownerSession)
    {
        return MGP_ANDROID_STALE_SESSION;
    }
    if (state != RunState::Running || std::this_thread::get_id() != gameThread ||
        (lifecycle.Flags & MGP_ANDROID_QUARANTINED) != 0 || revision > lifecycle.RetireRevision)
    {
        return MGP_ANDROID_INVALID_ARGUMENT;
    }
    if (revision > lifecycle.AcknowledgedRevision)
    {
        lifecycle.AcknowledgedRevision = revision;
    }
    retired.notify_all();
    return MGP_ANDROID_OK;
}

int32_t MGP_ANDROID_CALL MGP_Android_WaitRetired(uint64_t sessionId, uint64_t revision, uint32_t timeoutMilliseconds)
{
    if (sessionId == 0 || timeoutMilliseconds > 2000)
    {
        return MGP_ANDROID_INVALID_ARGUMENT;
    }
    std::unique_lock<std::mutex> lock(hostMutex);
    if (sessionId != ownerSession)
    {
        return MGP_ANDROID_STALE_SESSION;
    }
    retired.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), [&] {
        return sessionId != ownerSession || (lifecycle.Flags & MGP_ANDROID_QUARANTINED) != 0 ||
            lifecycle.AcknowledgedRevision >= revision;
    });
    if (sessionId != ownerSession)
    {
        return MGP_ANDROID_STALE_SESSION;
    }
    return (lifecycle.Flags & MGP_ANDROID_QUARANTINED) == 0 && lifecycle.AcknowledgedRevision >= revision
        ? MGP_ANDROID_OK : MGP_ANDROID_BUSY;
}

int MGP_ANDROID_CALL SDL_main(int argc, char** argv)
{
    static_assert(sizeof(int) == sizeof(int32_t), "SDL_main requires a 32-bit int");
    uint64_t session;
    if (!ParseSession(argc, argv, session))
    {
        return MGP_ANDROID_INVALID_ARGUMENT;
    }
    MGP_Android_RunCallback run;
    void* userData;
    {
        std::lock_guard<std::mutex> lock(hostMutex);
        if (session != ownerSession)
        {
            return newestSession == 0 ? MGP_ANDROID_NOT_REGISTERED : MGP_ANDROID_STALE_SESSION;
        }
        if (state == RunState::Running)
        {
            return MGP_ANDROID_BUSY;
        }
        if (state == RunState::Completed)
        {
            return MGP_ANDROID_ALREADY_RUN;
        }
        state = RunState::Running;
        gameThread = std::this_thread::get_id();
        run = callback;
        userData = callbackData;
    }
    int32_t result;
    try
    {
        result = run(userData);
    }
    catch (...)
    {
        result = MGP_ANDROID_CALLBACK_EXCEPTION;
    }

    // The callback has finished Game.Run and Game.Dispose on this SDL thread.
    // Retire SDL's subsystems/event queue before Java's later nativeQuit destroys
    // its Android JNI mutex/semaphores. No previous-session quit survives reuse.
    bool quarantine;
    {
        std::lock_guard<std::mutex> lock(hostMutex);
        quarantine = (lifecycle.Flags & MGP_ANDROID_QUARANTINED) != 0;
    }
    if (!quarantine)
    {
        try
        {
            SDL_Quit();
        }
        catch (...)
        {
            result = MGP_ANDROID_CALLBACK_EXCEPTION;
            std::lock_guard<std::mutex> lock(hostMutex);
            lifecycle.Flags |= MGP_ANDROID_QUARANTINED | MGP_ANDROID_EXITING;
        }
    }

    {
        std::lock_guard<std::mutex> lock(hostMutex);
        state = RunState::Completed;
        if ((lifecycle.Flags & MGP_ANDROID_QUARANTINED) == 0)
        {
            lifecycle.AcknowledgedRevision = lifecycle.RetireRevision;
        }
        retired.notify_all();
    }
    return result;
}
#endif
