// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"
#include "d3d_internal.h"

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        // 避免卸载时与工作线程抢锁死锁；正常退出应显式 D3D_Shutdown
        d3d::Engine::Instance().ShutdownForDllDetach();
        break;
    default:
        break;
    }
    return TRUE;
}

