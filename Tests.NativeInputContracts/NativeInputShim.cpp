// Focused ABI boundary for tests of the actual compiled Native framework.
#include "api_MGP.h"
#include "api_MGG.h"
#include "api_MGA.h"
#include <cstdio>
#include <cstdint>

static int creates, destroys, vibrations, graphicsDestroys, forbiddenDraws;
static bool desktop;
static uint32_t lifecycleFlags = 1;
static uint64_t lifecycleRevision, inputRetireRevision;
MG_EXPORT void Test_SetLifecycle(uint32_t flags)
{
    lifecycleFlags = flags;
    ++lifecycleRevision;
    if (flags != 1) { inputRetireRevision = lifecycleRevision; }
}
static void (*destroyObserver)();
static char identifier[32];

MG_EXPORT void Test_Configure(int isDesktop, void (*observer)())
{
    desktop = isDesktop;
    destroyObserver = observer;
}
MG_EXPORT int Test_Count(int counter)
{
    return counter == 0 ? creates : counter == 1 ? destroys : counter == 2 ? vibrations : counter == 3 ? graphicsDestroys : forbiddenDraws;
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

MG_EXPORT int MGP_Android_GetLifecycle(uint64_t session, MGP_AndroidLifecycle* snapshot)
{
    *snapshot = {};
    snapshot->SessionId = session == 0 ? 1 : session;
    snapshot->Generation = 1;
    snapshot->Flags = lifecycleFlags;
    snapshot->Revision = lifecycleRevision;
    snapshot->RetireRevision = inputRetireRevision;
    return 0;
}

mgint MGG_GraphicsDevice_BeginFrame(MGG_GraphicsDevice*) { return -1; }
void MGG_GraphicsDevice_Clear(MGG_GraphicsDevice*, MGClearOptions, Vector4&, mgfloat, mgint) { ++forbiddenDraws; }
void MGG_GraphicsDevice_Draw(MGG_GraphicsDevice*, MGPrimitiveType, mgint, mgint) { ++forbiddenDraws; }

MG_EXPORT int MGP_Android_AcknowledgeRetired(uint64_t, uint64_t) { return 0; }
mgbyte MGP_Platform_BeforeDraw(MGP_Platform*) { return lifecycleFlags == 1; }
