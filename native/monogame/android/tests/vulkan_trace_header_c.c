#include "api_MGG_AndroidTrace.h"
#include <stddef.h>
_Static_assert(sizeof(MGG_AndroidTrace_Context) == 160, "context");
_Static_assert(sizeof(MGG_AndroidTrace_Event) == 320, "event");
_Static_assert(offsetof(MGG_AndroidTrace_Event, EventKind) == 264, "kind");
_Static_assert(offsetof(MGG_AndroidTrace_Event, SkipReason) == 308, "skip");
_Static_assert(sizeof(MGG_AndroidTrace_EnableRequest) == 200, "enable");
_Static_assert(sizeof(MGG_AndroidTrace_ReadRequest) == 32, "read");
_Static_assert(sizeof(MGG_AndroidTrace_ReadResult) == 72, "result");
_Static_assert(sizeof(MGG_AndroidTrace_Anchor) == 240, "anchor");
_Static_assert(sizeof(MGG_AndroidTrace_Control) == 24, "control");
_Static_assert(sizeof(MGG_AndroidTrace_Clock) == 64, "clock");
int vulkan_trace_c_header(void) { return MGG_ANDROID_TRACE_VERSION; }
