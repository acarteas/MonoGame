// MonoGame - Copyright (C) MonoGame Foundation, Inc
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.

#include "api_MGP.h"
#include "mg_common.h"
#include <SDL.h>
#include <SDL_vulkan.h>

// Select only the runtime's Android branches after host headers have been read.
// This is a contract test with SDL boundary doubles, not an Android device test.
#if TEST_ANDROID
#define __ANDROID__ 1
#endif
#include "../native/monogame/sdl/MGP_sdl.cpp"

static int drawableWidth = 2340, drawableHeight = 1080;
static int windowWidth = 800, windowHeight = 480;
static int desktopCalls = 0;
static int drawableQueries = 0;
static SDL_Event nextEvent;
static bool eventPending = false;
static Uint32 windowFlags = 0;
static int inputFilters = 0;
#if TEST_ANDROID
static MGP_Android_Lifecycle lifecycle{500, 1, 0, 0, 0, MGP_ANDROID_SURFACE_READY, 0};
int32_t MGP_ANDROID_CALL MGP_Android_GetLifecycle(uint64_t sessionId, MGP_Android_Lifecycle* snapshot)
{
    assert(sessionId == 500);
    *snapshot = lifecycle;
    return MGP_ANDROID_OK;
}
#endif

extern "C" {
SDL_Window* SDL_CreateWindow(const char*, int, int, int, int, Uint32) { return reinterpret_cast<SDL_Window*>(1); }
Uint32 SDL_GetWindowID(SDL_Window*) { return 42; }
void SDL_DestroyWindow(SDL_Window*) { }
void SDL_Vulkan_GetDrawableSize(SDL_Window*, int* width, int* height)
{
    ++drawableQueries;
    *width = drawableWidth;
    *height = drawableHeight;
}
void SDL_GetWindowSize(SDL_Window*, int* width, int* height) { *width = windowWidth; *height = windowHeight; }
Uint32 SDL_GetWindowFlags(SDL_Window*) { return windowFlags; }
void SDL_GetWindowPosition(SDL_Window*, int* x, int* y) { ++desktopCalls; *x = 100; *y = 200; }
void SDL_SetWindowPosition(SDL_Window*, int, int) { ++desktopCalls; }
void SDL_SetWindowResizable(SDL_Window*, SDL_bool) { ++desktopCalls; }
void SDL_SetWindowBordered(SDL_Window*, SDL_bool) { ++desktopCalls; }
void SDL_SetWindowTitle(SDL_Window*, const char*) { ++desktopCalls; }
void SDL_SetWindowSize(SDL_Window*, int width, int height) { ++desktopCalls; windowWidth = width; windowHeight = height; }
void SDL_WarpMouseInWindow(SDL_Window*, int, int) { ++desktopCalls; }
void SDL_FilterEvents(SDL_EventFilter filter, void* data)
{
    ++inputFilters;
    if (eventPending && !filter(data, &nextEvent)) { eventPending = false; }
}
int SDL_PollEvent(SDL_Event* event)
{
    if (!eventPending) { return 0; }
    *event = nextEvent;
    eventPending = false;
    return 1;
}
}

int main()
{
    static_assert(sizeof(mgint) == 4);
    static_assert(static_cast<int>(MGMonoGamePlatform::Android) == 0);
    static_assert(static_cast<int>(MGMonoGamePlatform::WindowsDX12) == 7);
    static_assert(static_cast<int>(MGMonoGamePlatform::DesktopVK) == 12);
    static_assert(static_cast<int>(MGMonoGamePlatform::NintendoSwitch2) == 13);
    static_assert(static_cast<int>(MGGraphicsBackend::Vulkan) == 2);
    static_assert(static_cast<int>(MGGraphicsBackend::DirectX12) == 4);
#if TEST_ANDROID
    assert(MGP_Platform_GetPlatform() == MGMonoGamePlatform::Android);
#else
    assert(MGP_Platform_GetPlatform() == MGMonoGamePlatform::DesktopVK);
#endif
    assert(MGP_Platform_GetGraphicsBackend() == MGGraphicsBackend::Vulkan);

    MGP_Platform platform;
#if TEST_ANDROID
    platform.sessionId = lifecycle.SessionId;
#endif
    int width = 800, height = 480;
    auto window = MGP_Window_Create(&platform, width, height, "Contract test", nullptr);
#if TEST_ANDROID
    assert(width == 2340 && height == 1080);
    assert(drawableQueries == 1);
#else
    assert(width == 800 && height == 480);
    assert(drawableQueries == 0);
#endif
    MGP_Window_SetClientSize(window, 320, 200);
    MGP_Window_SetPosition(window, 5, 10);
    MGP_Window_SetAllowUserResizing(window, true);
    MGP_Window_SetIsBorderless(window, true);
    MGP_Window_SetTitle(window, "Updated");
    MGP_Mouse_WarpPosition(window, 20, 30);
    int x = -1, y = -1;
    MGP_Window_GetPosition(window, x, y);
    MGP_Window_GetClientSize(window, width, height);
#if TEST_ANDROID
    MGP_Window_SetIconBitmap(window, nullptr, 0);
    assert(desktopCalls == 0);
    assert(x == 0 && y == 0);
    assert(width == 2340 && height == 1080);
    assert(!MGP_Window_GetAllowUserResizing(window));
    assert(MGP_Window_GetIsBorderless(window));
#else
    assert(desktopCalls == 7);
    assert(x == 100 && y == 200);
    assert(width == 320 && height == 200);
    windowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS;
    assert(MGP_Window_GetAllowUserResizing(window));
    assert(MGP_Window_GetIsBorderless(window));
#endif

    // Logical resize payloads deliberately differ from the drawable pixels.
    drawableWidth = 2160;
    drawableHeight = 1000;
    for (auto kind : { SDL_WINDOWEVENT_RESIZED, SDL_WINDOWEVENT_SIZE_CHANGED })
    {
        nextEvent = {};
        nextEvent.type = SDL_WINDOWEVENT;
        nextEvent.window.windowID = 42;
        nextEvent.window.event = kind;
        nextEvent.window.data1 = 720;
        nextEvent.window.data2 = 333;
        eventPending = true;
        MGP_Event event;
        assert(MGP_Platform_PollEvent(&platform, event));
        assert(event.Type == MGEventType::WindowResized);
        assert(event.Window.Window == window);
#if TEST_ANDROID
        assert(event.Window.Data1 == 2160 && event.Window.Data2 == 1000);
#else
        assert(event.Window.Data1 == 720 && event.Window.Data2 == 333);
#endif
    }
#if TEST_ANDROID
    assert(MGP_Platform_BeforeDraw(&platform));
    lifecycle.Flags = MGP_ANDROID_SURFACE_READY | MGP_ANDROID_PAUSED;
    assert(!MGP_Platform_BeforeDraw(&platform));
    lifecycle.Flags = 0;
    assert(!MGP_Platform_BeforeDraw(&platform));
    lifecycle.Flags = MGP_ANDROID_SURFACE_READY;
    assert(MGP_Platform_BeforeDraw(&platform));
    for (auto kind : {SDL_KEYDOWN, SDL_MOUSEBUTTONDOWN, SDL_CONTROLLERAXISMOTION})
    {
        nextEvent = {};
        nextEvent.type = kind;
        eventPending = true;
        lifecycle.RetireRevision = ++lifecycle.Revision;
        MGP_Event text{};
        text.Type = MGEventType::TextInput;
        platform.queued_events.push(text);
        MGP_Event moved{};
        moved.Type = MGEventType::WindowMoved;
        platform.queued_events.push(moved);
        MGP_Event event;
        assert(MGP_Platform_PollEvent(&platform, event) && event.Type == MGEventType::AndroidLifecycle);
        assert(!eventPending && platform.queued_events.size() == 1);
        int filters = inputFilters;
        assert(MGP_Platform_PollEvent(&platform, event) && event.Type == MGEventType::WindowMoved);
        assert(inputFilters == filters);
    }

    // A retirement boundary preserves unrelated native queued events. The next
    // unchanged ready checkpoint must forward newly received input normally.
    nextEvent = {};
    nextEvent.type = SDL_WINDOWEVENT;
    nextEvent.window.windowID = 42;
    nextEvent.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
    eventPending = true;
    lifecycle.RetireRevision = ++lifecycle.Revision;
    MGP_Event event;
    assert(MGP_Platform_PollEvent(&platform, event) && event.Type == MGEventType::AndroidLifecycle);
    assert(eventPending);
    assert(MGP_Platform_PollEvent(&platform, event) && event.Type == MGEventType::WindowResized);
    nextEvent = {};
    nextEvent.type = SDL_MOUSEBUTTONDOWN;
    nextEvent.button.windowID = 42;
    nextEvent.button.button = SDL_BUTTON_LEFT;
    eventPending = true;
    int filters = inputFilters;
    assert(MGP_Platform_PollEvent(&platform, event) && event.Type == MGEventType::MouseButtonDown);
    assert(inputFilters == filters);
#else
    assert(inputFilters == 0);
#endif
    MGP_Window_Destroy(window);
    puts(TEST_ANDROID ? "Android SDL window contracts passed (host doubles)" : "Desktop Vulkan SDL window contracts passed (host doubles)");
}
