// Tests use production host.cpp and the pinned vendored SDL event queue.
#include "api_MGP_Android.h"
#include <SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
void Check(bool condition, const char* expression, int line)
{
    if (!condition)
    {
        std::cerr << "FAILED line " << line << ": " << expression << '\n';
        std::abort();
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

class Gate
{
    std::mutex mutex;
    std::condition_variable condition;
    bool open = false;
public:
    void Wait()
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(condition.wait_for(lock, std::chrono::seconds(5), [&] { return open; }));
    }
    void Open()
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = true;
        condition.notify_all();
    }
};

enum class Mode { Normal, Throw, FailureResult };
struct Run
{
    uint64_t session;
    Mode mode;
    Gate cleaning;
    Gate allowCleanup;
    std::thread::id callbackThread;
    std::atomic<bool> callbackFinished{false};
    std::atomic<int> cleanups{0};
    explicit Run(uint64_t id, Mode value) : session(id), mode(value) { }
};
Run* current = nullptr;

int Enter(uint64_t session)
{
    std::string argument = MGP_ANDROID_SESSION_ARGUMENT + std::to_string(session);
    char program[] = "session-reuse-tests";
    char* arguments[] = {program, argument.data(), nullptr};
    return SDL_main(2, arguments);
}

void Push(Uint32 type)
{
    SDL_Event event{};
    event.type = type;
    CHECK(SDL_PushEvent(&event) == 1);
}

int32_t MGP_ANDROID_CALL Callback(void* data)
{
    auto& run = *static_cast<Run*>(data);
    run.callbackThread = std::this_thread::get_id();
    struct Finally
    {
        Run& run;
        ~Finally() { run.callbackFinished = true; }
    } finally{run};

    // No old subsystem/queue is allowed to leak into a replacement session.
    CHECK(SDL_WasInit(0) == 0);
    CHECK(SDL_Init(SDL_INIT_EVENTS) == 0);
    SDL_Event event{};
    while (SDL_PollEvent(&event))
    {
        CHECK(event.type != SDL_QUIT && event.type != SDL_APP_TERMINATING);
    }

    // A real cancellation for this new session must still be delivered intact.
    Push(SDL_QUIT);
    CHECK(SDL_PollEvent(&event) == 1 && event.type == SDL_QUIT);

    // Model the old session's final quit injection, not a new-admission flush.
    Push(SDL_QUIT);
    Push(SDL_APP_TERMINATING);
    if (run.mode == Mode::Throw)
    {
        throw std::runtime_error("callback failed after initializing SDL");
    }

    return run.mode == Mode::FailureResult ? -47 : 1487;
}
}

extern "C" void __real_SDL_Quit(void);
extern "C" void __wrap_SDL_Quit(void)
{
    auto& run = *current;
    CHECK(run.callbackFinished);
    CHECK(std::this_thread::get_id() == run.callbackThread);
    CHECK(MGP_Android_ClearRunCallback(run.session) == MGP_ANDROID_BUSY);
    ++run.cleanups;
    run.cleaning.Open();
    run.allowCleanup.Wait();

    // This is the real SDL_Quit from the pinned vendored static SDL build.
    __real_SDL_Quit();
    CHECK(SDL_WasInit(0) == 0);
}

int main()
{
    SDL_version version;
    SDL_GetVersion(&version);
    CHECK(version.major == SDL_MAJOR_VERSION && version.minor == SDL_MINOR_VERSION && version.patch == SDL_PATCHLEVEL);
    uint64_t session = 1;
    for (auto mode : {Mode::Normal, Mode::Normal, Mode::Throw, Mode::FailureResult, Mode::Normal})
    {
        Run run(session, mode);
        current = &run;
        CHECK(MGP_Android_RegisterRunCallback(session, Callback, &run) == MGP_ANDROID_OK);
        int result = 0;
        std::thread game([&] { result = Enter(session); });
        run.cleaning.Wait();
        CHECK(MGP_Android_ClearRunCallback(session) == MGP_ANDROID_BUSY);
        CHECK(MGP_Android_RegisterRunCallback(session + 1, Callback, &run) == MGP_ANDROID_BUSY);
        CHECK(run.cleanups == 1);
        CHECK(SDL_HasEvent(SDL_QUIT) && SDL_HasEvent(SDL_APP_TERMINATING));
        run.allowCleanup.Open();
        game.join();
        CHECK(result == (mode == Mode::Throw ? MGP_ANDROID_CALLBACK_EXCEPTION : mode == Mode::FailureResult ? -47 : 1487));
        CHECK(SDL_WasInit(0) == 0);
        CHECK(MGP_Android_ClearRunCallback(session) == MGP_ANDROID_OK);

        // Java's old late quit cannot revive the event queue after cleanup.
        for (auto type : {SDL_QUIT, SDL_APP_TERMINATING})
        {
            SDL_Event late{};
            late.type = type;
            CHECK(SDL_PushEvent(&late) < 0);
        }

        CHECK(SDL_WasInit(0) == 0);
        ++session;
    }

    std::cout << "Pinned SDL " << unsigned(version.major) << '.' << unsigned(version.minor) << '.' << unsigned(version.patch)
              << ": repeated-session queue, fresh cancellation, cleanup order/thread, active-clear and failure contracts passed\n";
}
