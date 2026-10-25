#pragma once
#include "api_MGP_Android.h"
#include <SDL.h>

// Used by the real Vulkan consumer and the host lifetime tests. The callback is
// game-thread GPU retirement, invoked without any host lock. A failed retirement
// leaves the request unacknowledged so the UI must retain/quarantine its window.
struct MGP_Android_SurfaceConsumer
{
    uint64_t SessionId = 0;
    uint64_t Generation = 0;
    uint64_t RetiredRevision = 0;

    template<class Retire>
    bool Apply(const MGP_Android_Lifecycle& snapshot, Retire retire)
    {
        if (SessionId == 0 || snapshot.SessionId != SessionId || snapshot.Reserved != 0 ||
            snapshot.Generation < Generation || (snapshot.Flags & MGP_ANDROID_QUARANTINED) != 0)
        {
            return false;
        }
        if (snapshot.Generation != Generation || snapshot.RetireRevision > RetiredRevision)
        {
            if (!retire())
            {
                return false;
            }
            if (MGP_Android_AcknowledgeRetired(SessionId, snapshot.RetireRevision) != MGP_ANDROID_OK)
            {
                return false;
            }
            Generation = snapshot.Generation;
            RetiredRevision = snapshot.RetireRevision;
        }
        return snapshot.Generation != 0 && snapshot.Flags == MGP_ANDROID_SURFACE_READY;
    }
};

// A consumed retirement discards pending held-input changes before forwarding
// events again. Preserve device identity, window/lifecycle, and unrelated events.
inline int SDLCALL MGP_Android_KeepPendingEvent(void*, SDL_Event* event)
{
    switch (event->type)
    {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_TEXTEDITING:
    case SDL_TEXTINPUT:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
    case SDL_CONTROLLERAXISMOTION:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        return 0;
    default:
        return 1;
    }
}

inline void MGP_Android_DiscardPendingInput()
{
    SDL_FilterEvents(MGP_Android_KeepPendingEvent, nullptr);
}
