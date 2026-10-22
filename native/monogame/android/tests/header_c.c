// Compile the public ABI as C as well as C++ and check fixed-width signatures.
#include "api_MGP_Android.h"

_Static_assert(sizeof(uint64_t) == 8, "session width");
_Static_assert(sizeof(int32_t) == 4, "status/callback result width");

uint32_t check_c_header(void)
{
    uint32_t (MGP_ANDROID_CALL *version)(void) = MGP_Android_GetHostApiVersion;
    int32_t (MGP_ANDROID_CALL *registration)(uint64_t, MGP_Android_RunCallback, void*) =
        MGP_Android_RegisterRunCallback;
    int32_t (MGP_ANDROID_CALL *clear)(uint64_t) = MGP_Android_ClearRunCallback;
    int (MGP_ANDROID_CALL *entry)(int, char**) = SDL_main;
    (void)registration;
    (void)clear;
    (void)entry;
    return version();
}
