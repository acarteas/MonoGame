// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

#include "api_MGP_Android.h"

#if defined(__ANDROID__) || defined(MGP_ANDROID_HOST_TEST)
#include <cstring>
#include <limits>
#include <mutex>
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
    if (state == RunState::Running)
    {
        return MGP_ANDROID_BUSY;
    }
    callback = nullptr;
    callbackData = nullptr;
    ownerSession = 0;
    state = RunState::Empty;
    return MGP_ANDROID_OK;
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
    try
    {
        SDL_Quit();
    }
    catch (...)
    {
        result = MGP_ANDROID_CALLBACK_EXCEPTION;
    }

    {
        std::lock_guard<std::mutex> lock(hostMutex);
        state = RunState::Completed;
    }
    return result;
}
#endif
