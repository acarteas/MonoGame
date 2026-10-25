// Exercises the production host and the very consumer used by the Vulkan backend.
#include "../surface_lifecycle.h"
#include "api_MGP.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

static void Check(bool value) { if (!value) { std::abort(); } }
static_assert(sizeof(MGP_Android_Lifecycle) == 48);
static_assert(sizeof(MGP_AndroidLifecycle) == 48);
static_assert(sizeof(MGP_Event) == 60);
static_assert(offsetof(MGP_Event, AndroidLifecycle) == 12);
static_assert(static_cast<int>(MGEventType::AndroidLifecycle) == 18);
static int quits;
extern "C" void __wrap_SDL_Quit() { ++quits; }

struct Harness
{
    std::mutex mutex;
    std::condition_variable condition;
    std::queue<std::function<void()>> commands;
    bool stopping = false;
    std::thread::id gameThread;
    int retirements = 0;
    MGP_Android_SurfaceConsumer consumer;

    void OnGame(std::function<void()> command)
    {
        auto task = std::make_shared<std::packaged_task<void()>>(command);
        auto completion = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex);
            commands.emplace([task] { (*task)(); });
        }
        condition.notify_all();
        Check(completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        completion.get();
    }
    bool Apply(bool success = true)
    {
        MGP_Android_Lifecycle snapshot{};
        Check(MGP_Android_GetLifecycle(consumer.SessionId, &snapshot) == 0);
        return consumer.Apply(snapshot, [&] {
            Check(std::this_thread::get_id() == gameThread);

            // A GPU/retirement callback must not inherit the host lock.
            auto concurrentRead = std::async(std::launch::async, [&] {
                MGP_Android_Lifecycle copy;
                return MGP_Android_GetLifecycle(consumer.SessionId, &copy);
            });
            Check(concurrentRead.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
            Check(concurrentRead.get() == 0);
            ++retirements;
            return success;
        });
    }
};

static int32_t MGP_ANDROID_CALL Run(void* data)
{
    auto& harness = *static_cast<Harness*>(data);
    harness.gameThread = std::this_thread::get_id();
    MGP_Android_Lifecycle current;
    Check(MGP_Android_GetLifecycle(0, &current) == 0 && current.SessionId == 501);
    harness.consumer.SessionId = current.SessionId;
    for (;;)
    {
        std::function<void()> command;
        {
            std::unique_lock<std::mutex> lock(harness.mutex);
            Check(harness.condition.wait_for(lock, std::chrono::seconds(5), [&] {
                return harness.stopping || !harness.commands.empty();
            }));
            if (harness.commands.empty() && harness.stopping) { break; }
            command = std::move(harness.commands.front());
            harness.commands.pop();
        }
        command();
    }
    return -7;
}

int main()
{
    Check(SDL_Init(SDL_INIT_EVENTS) == 0);
    for (uint32_t type : {uint32_t(SDL_KEYDOWN), uint32_t(SDL_MOUSEMOTION), uint32_t(SDL_CONTROLLERBUTTONDOWN),
        uint32_t(SDL_CONTROLLERAXISMOTION), uint32_t(SDL_FINGERDOWN), uint32_t(SDL_CONTROLLERDEVICEADDED),
        uint32_t(SDL_CONTROLLERDEVICEREMOVED), uint32_t(SDL_WINDOWEVENT), uint32_t(SDL_APP_DIDENTERBACKGROUND), uint32_t(SDL_USEREVENT)})
    {
        SDL_Event event{};
        event.type = type;
        Check(SDL_PushEvent(&event) == 1);
    }
    MGP_Android_DiscardPendingInput();
    int retained = 0;
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) == 1)
    {
        Check(MGP_Android_KeepPendingEvent(nullptr, &event) == 1);
        Check(event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED ||
            event.type == SDL_WINDOWEVENT || event.type == SDL_APP_DIDENTERBACKGROUND || event.type == SDL_USEREVENT);
        ++retained;
    }
    Check(retained == 5);
    Harness harness;
    Check(MGP_Android_RegisterRunCallback(501, Run, &harness) == 0);
    std::thread game([&] {
        char program[] = "test", argument[] = "--mgp-android-session=501";
        char* arguments[] = {program, argument};
        Check(SDL_main(2, arguments) == -7);
    });
    harness.OnGame([] {});
    MGP_Android_Lifecycle snapshot;
    Check(MGP_Android_GetLifecycle(0, &snapshot) == MGP_ANDROID_STALE_SESSION);
    uint64_t revision;
    Check(MGP_Android_PublishLifecycle(501, 1, MGP_ANDROID_SURFACE_READY, &revision) == 0);
    harness.OnGame([&] { Check(harness.Apply()); });
    Check(harness.retirements == 1);
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        Check(MGP_Android_PublishLifecycle(501, 1, MGP_ANDROID_SURFACE_READY | MGP_ANDROID_PAUSED, &revision) == 0);
        Check(MGP_Android_WaitRetired(501, revision, 2) == MGP_ANDROID_BUSY);
        Check(MGP_Android_AcknowledgeRetired(501, revision) != 0); // UI is not the consumer.
        harness.OnGame([&] { Check(!harness.Apply()); });
        Check(MGP_Android_WaitRetired(501, revision, 0) == 0);
        Check(MGP_Android_PublishLifecycle(501, 1, MGP_ANDROID_SURFACE_READY, &revision) == 0);
        harness.OnGame([&] { Check(harness.Apply()); });
    }

    // The same native SDL window may stay alive throughout these generations.
    Check(MGP_Android_PublishLifecycle(501, 1, 0, &revision) == 0);
    harness.OnGame([&] { Check(!harness.Apply()); });
    Check(MGP_Android_WaitRetired(501, revision, 0) == 0);
    uint64_t completedRetirement = revision;
    Check(MGP_Android_PublishLifecycle(501, 2, MGP_ANDROID_SURFACE_READY, &revision) == 0);
    harness.OnGame([&] { Check(harness.Apply() && harness.consumer.Generation == 2); });
    Check(MGP_Android_PublishLifecycle(500, 3, 1, &revision) == MGP_ANDROID_STALE_SESSION);
    Check(MGP_Android_PublishLifecycle(501, 1, 1, &revision) == MGP_ANDROID_STALE_SESSION);
    Check(MGP_Android_PublishLifecycle(501, 2, 0, &revision) == 0);
    harness.OnGame([&] { Check(!harness.Apply(false)); });
    Check(MGP_Android_WaitRetired(501, revision, 2) == MGP_ANDROID_BUSY);
    uint64_t unacknowledged = revision;
    auto waiter = std::async(std::launch::async, [=] {
        return MGP_Android_WaitRetired(501, unacknowledged, 1500);
    });
    Check(MGP_Android_PublishLifecycle(501, 2, MGP_ANDROID_QUARANTINED | MGP_ANDROID_EXITING, &revision) == 0);
    Check(waiter.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready);
    Check(waiter.get() == MGP_ANDROID_BUSY);
    Check(MGP_Android_WaitRetired(501, completedRetirement, 0) == MGP_ANDROID_BUSY);
    {
        std::lock_guard<std::mutex> lock(harness.mutex);
        harness.stopping = true;
    }
    harness.condition.notify_all();
    game.join();
    Check(quits == 0); // Callback failure with unsafe disposal never releases SDL's window.
    Check(MGP_Android_WaitRetired(501, revision, 2) == MGP_ANDROID_BUSY);
    Check(MGP_Android_ClearRunCallback(501) == MGP_ANDROID_BUSY);
    Check(MGP_Android_RegisterRunCallback(502, Run, &harness) == MGP_ANDROID_BUSY);
    Check(MGP_Android_PublishLifecycle(501, 2, 1, &revision) == 0);
    Check(MGP_Android_GetLifecycle(501, &snapshot) == 0 && (snapshot.Flags & MGP_ANDROID_QUARANTINED) != 0);
    Check(MGP_Android_WaitRetired(501, revision, 1) == MGP_ANDROID_BUSY);
    std::cout << "PASS actual host/consumer generation, pause, retirement ack, finite wait, stale owner and completed quarantine\n";
}
