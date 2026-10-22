// Tests link the actual production host.cpp, never a model of its state logic.
#include "api_MGP_Android.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" uint32_t check_c_header(void);

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

int Enter(uint64_t session)
{
    std::string argument = MGP_ANDROID_SESSION_ARGUMENT + std::to_string(session);
    char program[] = "host-tests";
    char* arguments[] = {program, argument.data(), nullptr};
    return SDL_main(2, arguments);
}

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

struct BlockingRun
{
    Gate entered;
    Gate release;
    std::atomic<int> calls{0};
    std::thread::id thread;
    uint64_t session;
    explicit BlockingRun(uint64_t value) : session(value) {}
};

int32_t MGP_ANDROID_CALL Block(void* data)
{
    auto& run = *static_cast<BlockingRun*>(data);
    ++run.calls;
    run.thread = std::this_thread::get_id();

    // No lock is held while running user code: even reentrant API calls finish.
    CHECK(MGP_Android_ClearRunCallback(run.session) == MGP_ANDROID_BUSY);
    CHECK(MGP_Android_RegisterRunCallback(run.session + 1, Block, data) == MGP_ANDROID_BUSY);
    CHECK(Enter(run.session) == MGP_ANDROID_BUSY);
    run.entered.Open();
    run.release.Wait();
    return -123456789;
}

int32_t MGP_ANDROID_CALL Count(void* data)
{
    ++*static_cast<int*>(data);
    return 123456789;
}

int32_t MGP_ANDROID_CALL NullData(void* data)
{
    CHECK(data == nullptr);
    return std::numeric_limits<int32_t>::min();
}

int32_t MGP_ANDROID_CALL Throw(void*)
{
    throw std::runtime_error("native callback failure");
}

void InvalidArguments()
{
    CHECK(check_c_header() == MGP_ANDROID_HOST_API_VERSION);
    CHECK(MGP_Android_RegisterRunCallback(0, NullData, nullptr) == MGP_ANDROID_INVALID_ARGUMENT);
    CHECK(MGP_Android_RegisterRunCallback(1, nullptr, nullptr) == MGP_ANDROID_INVALID_ARGUMENT);
    CHECK(MGP_Android_ClearRunCallback(0) == MGP_ANDROID_INVALID_ARGUMENT);
    CHECK(SDL_main(0, nullptr) == MGP_ANDROID_INVALID_ARGUMENT);
    CHECK(SDL_main(2, nullptr) == MGP_ANDROID_INVALID_ARGUMENT);
    char* nullArgument[] = {nullptr, nullptr};
    CHECK(SDL_main(2, nullArgument) == MGP_ANDROID_INVALID_ARGUMENT);
    for (const char* malformed : {"", "1", "--mgp-android-session=", "--mgp-android-session=0",
         "--mgp-android-session=01", "--mgp-android-session=-1", "--mgp-android-session=+1",
         "--mgp-android-session= 1", "--mgp-android-session=1 ", "--mgp-android-session=1x",
         "--mgp-android-session=18446744073709551616", "--mgp-android-session=999999999999999999999"})
    {
        std::string argument(malformed);
        char* arguments[] = {nullptr, argument.data()};
        CHECK(SDL_main(2, arguments) == MGP_ANDROID_INVALID_ARGUMENT);
        CHECK(SDL_main(1, arguments) == MGP_ANDROID_INVALID_ARGUMENT);
        CHECK(SDL_main(3, arguments) == MGP_ANDROID_INVALID_ARGUMENT);
    }
    CHECK(Enter(1) == MGP_ANDROID_NOT_REGISTERED);
    std::cout << "PASS fixed-width C/C++ ABI, null/zero/invalid entry arguments\n";
}

void Lifecycle(uint64_t session)
{
    int firstCalls = 0;
    CHECK(MGP_Android_RegisterRunCallback(session, Count, &firstCalls) == MGP_ANDROID_OK);
    CHECK(MGP_Android_RegisterRunCallback(session, Count, &firstCalls) == MGP_ANDROID_ALREADY_REGISTERED);
    CHECK(MGP_Android_RegisterRunCallback(session - 1, Count, &firstCalls) == MGP_ANDROID_STALE_SESSION);
    CHECK(MGP_Android_RegisterRunCallback(session + 1, Count, &firstCalls) == MGP_ANDROID_BUSY);
    CHECK(MGP_Android_ClearRunCallback(session + 1) == MGP_ANDROID_STALE_SESSION);
    CHECK(Enter(session + 1) == MGP_ANDROID_STALE_SESSION);
    CHECK(MGP_Android_ClearRunCallback(session) == MGP_ANDROID_OK);
    CHECK(Enter(session) == MGP_ANDROID_STALE_SESSION);
    CHECK(firstCalls == 0); // Cancellation never dereferences the old userData.
    CHECK(MGP_Android_ClearRunCallback(session) == MGP_ANDROID_STALE_SESSION);
    CHECK(MGP_Android_RegisterRunCallback(session, Count, &firstCalls) == MGP_ANDROID_STALE_SESSION);

    int nextCalls = 0;
    CHECK(MGP_Android_RegisterRunCallback(session + 1, Count, &nextCalls) == MGP_ANDROID_OK);
    CHECK(Enter(session) == MGP_ANDROID_STALE_SESSION); // Delayed previous SDL thread.
    CHECK(MGP_Android_ClearRunCallback(session) == MGP_ANDROID_STALE_SESSION);
    CHECK(Enter(session + 1) == 123456789);
    CHECK(Enter(session + 1) == MGP_ANDROID_ALREADY_RUN);
    CHECK(firstCalls == 0 && nextCalls == 1);
    CHECK(MGP_Android_RegisterRunCallback(session + 2, NullData, nullptr) == MGP_ANDROID_BUSY);
    CHECK(MGP_Android_ClearRunCallback(session + 1) == MGP_ANDROID_OK);
}

void Concurrency(uint64_t session)
{
    constexpr int competitors = 16;
    BlockingRun run(session);
    Gate start;
    std::vector<std::thread> registrars;
    std::vector<int32_t> registration(competitors);
    for (int index = 0; index < competitors; ++index)
        registrars.emplace_back([&, index] {
            start.Wait();
            registration[index] = MGP_Android_RegisterRunCallback(session, Block, &run);
        });
    start.Open();
    for (auto& thread : registrars)
        thread.join();
    int accepted = 0;
    for (int32_t result : registration)
    {
        CHECK(result == MGP_ANDROID_OK || result == MGP_ANDROID_ALREADY_REGISTERED);
        accepted += result == MGP_ANDROID_OK;
    }
    CHECK(accepted == 1);

    int gameResult = 0;
    std::thread game([&] { gameResult = Enter(session); });
    run.entered.Wait();
    CHECK(run.thread == game.get_id() && run.thread != std::this_thread::get_id());
    std::vector<std::thread> entrants;
    for (int index = 0; index < competitors; ++index)
        entrants.emplace_back([&] {
            CHECK(Enter(session) == MGP_ANDROID_BUSY);
            CHECK(Enter(session - 1) == MGP_ANDROID_STALE_SESSION);
            CHECK(MGP_Android_ClearRunCallback(session) == MGP_ANDROID_BUSY);
            CHECK(MGP_Android_ClearRunCallback(session + 1) == MGP_ANDROID_STALE_SESSION);
            CHECK(MGP_Android_RegisterRunCallback(session + 1, Block, &run) == MGP_ANDROID_BUSY);
        });
    for (auto& thread : entrants)
        thread.join();
    CHECK(run.calls == 1);
    run.release.Open();
    game.join();
    CHECK(gameResult == -123456789 && run.calls == 1);
    CHECK(Enter(session) == MGP_ANDROID_ALREADY_RUN);
    CHECK(MGP_Android_ClearRunCallback(session) == MGP_ANDROID_OK);
    CHECK(MGP_Android_RegisterRunCallback(session, Block, &run) == MGP_ANDROID_STALE_SESSION);

    // State and IDs remain usable after completion, with no cross-session data.
    CHECK(MGP_Android_RegisterRunCallback(session + 1, NullData, nullptr) == MGP_ANDROID_OK);
    CHECK(Enter(session) == MGP_ANDROID_STALE_SESSION);
    CHECK(Enter(session + 1) == std::numeric_limits<int32_t>::min());
    CHECK(MGP_Android_ClearRunCallback(session + 1) == MGP_ANDROID_OK);
}
}

int main()
{
    InvalidArguments();
    for (uint64_t iteration = 0; iteration < 20; ++iteration)
    {
        Lifecycle(2 + iteration * 10);
        Concurrency(4 + iteration * 10);
    }
    std::cout << "PASS cancellation, stale/duplicate/cross-session rejection, next-session handoff\n";
    std::cout << "PASS 20 deterministic concurrent register/entry/active-clear rounds; callback runs once on caller thread\n";
    CHECK(MGP_Android_RegisterRunCallback(1000, Throw, nullptr) == MGP_ANDROID_OK);
    CHECK(Enter(1000) == MGP_ANDROID_CALLBACK_EXCEPTION);
    CHECK(Enter(1000) == MGP_ANDROID_ALREADY_RUN);
    CHECK(MGP_Android_ClearRunCallback(1000) == MGP_ANDROID_OK);
    const uint64_t lastSession = std::numeric_limits<uint64_t>::max();
    CHECK(MGP_Android_RegisterRunCallback(lastSession, NullData, nullptr) == MGP_ANDROID_OK);
    CHECK(Enter(lastSession) == std::numeric_limits<int32_t>::min());
    CHECK(MGP_Android_ClearRunCallback(lastSession) == MGP_ANDROID_OK);
    CHECK(MGP_Android_RegisterRunCallback(lastSession, NullData, nullptr) == MGP_ANDROID_STALE_SESSION);
    CHECK(MGP_Android_RegisterRunCallback(1, NullData, nullptr) == MGP_ANDROID_STALE_SESSION);
    std::cout << "PASS native exception completion and UINT64_MAX exhaustion without ID wrap\n";
}
