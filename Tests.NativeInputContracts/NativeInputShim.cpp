// Focused ABI boundary for tests of the actual compiled Native framework.
#include "api_MGP.h"
#include "api_MGG.h"
#include "api_MGA.h"
#include <cstdio>

static int creates, destroys, vibrations, graphicsDestroys;
static bool desktop;
static void (*destroyObserver)();
static char identifier[32];

MG_EXPORT void Test_Configure(int isDesktop, void (*observer)())
{
    desktop = isDesktop;
    destroyObserver = observer;
}
MG_EXPORT int Test_Count(int counter)
{
    return counter == 0 ? creates : counter == 1 ? destroys : counter == 2 ? vibrations : graphicsDestroys;
}
MGMonoGamePlatform MGP_Platform_GetPlatform() { return desktop ? MGMonoGamePlatform::DesktopVK : MGMonoGamePlatform::Android; }
MGGraphicsBackend MGP_Platform_GetGraphicsBackend() { return MGGraphicsBackend::Vulkan; }
MGP_Platform* MGP_Platform_Create(MGGameRunBehavior& behavior)
{
    ++creates;
    behavior = MGGameRunBehavior::Synchronous;
    return reinterpret_cast<MGP_Platform*>(0x100);
}
void MGP_Platform_Destroy(MGP_Platform*)
{
    if (destroyObserver)
    {
        destroyObserver();
    }
    ++destroys;
}
MGP_Window* MGP_Window_Create(MGP_Platform*, mgint&, mgint&, const char*) { return reinterpret_cast<MGP_Window*>(0x200); }
#ifndef FAIL_DESTROY
void MGP_Window_Destroy(MGP_Window*) { }
#endif
void* MGP_Window_GetNativeHandle(MGP_Window*) { return reinterpret_cast<void*>(0x300); }
void MGP_Window_SetIconBitmap(MGP_Window*, mgbyte*, mgint) { }
#ifndef FAIL_CONSTRUCT
void MGP_Mouse_SetVisible(MGP_Platform*, mgbyte) { }
#endif
mgint MGP_GamePad_GetMaxSupported() { return 16; }
void MGP_GamePad_GetCaps(MGP_Platform*, mgint, MGP_ControllerCaps* caps)
{
    std::snprintf(identifier, sizeof(identifier), "controller-session-%d", creates);
    *caps = {};
    caps->Identifier = identifier;
    caps->DisplayName = identifier;
    caps->GamePadType = MGGamePadType::GamePad;
    caps->InputFlags = 1u << 0;
    caps->HasLeftVibrationMotor = true;
}
mgbyte MGP_GamePad_SetVibration(MGP_Platform*, mgint, mgfloat, mgfloat, mgfloat, mgfloat) { ++vibrations; return 1; }
MGG_GraphicsSystem* MGG_GraphicsSystem_Create() { return reinterpret_cast<MGG_GraphicsSystem*>(0x400); }
void MGG_GraphicsSystem_Destroy(MGG_GraphicsSystem*) { ++graphicsDestroys; }
mgint MGA_System_GetMaxInstances() { return 16; }
