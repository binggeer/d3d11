#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef D3D11_EXPORTS
#define D3D_API extern "C" __declspec(dllexport)
#else
#define D3D_API extern "C" __declspec(dllimport)
#endif

D3D_API int __stdcall D3D_Init(int sourceHwnd, int hwProbe, int cropX, int cropY, int cropW, int cropH);
D3D_API int __stdcall D3D_SetSource(int sourceHwnd, int cropX, int cropY, int cropW, int cropH);
D3D_API int __stdcall D3D_SetRenderSize(int frameW, int frameH);
D3D_API void __stdcall D3D_Shutdown();
D3D_API int __stdcall D3D_GetCaptureWidth();
D3D_API int __stdcall D3D_GetCaptureHeight();
D3D_API int __stdcall D3D_GetCaptureImageByteCount();
D3D_API int __stdcall D3D_GetDevicePtr();
D3D_API int __stdcall D3D_GetContextPtr();
D3D_API int __stdcall D3D_GetTexturePtr();
D3D_API int __stdcall D3D_CaptureToTexture();
D3D_API int __stdcall D3D_CaptureToImageBytes(void* buffer, int bufferLen);
D3D_API int __stdcall D3D_RenderToWindow(int hwnd, void* frameData, int frameLen);
D3D_API int __stdcall D3D_RenderBlackToWindow(int hwnd);
D3D_API const char* __stdcall D3D_GetEncoderInfo();
D3D_API const char* __stdcall D3D_GetLastError();
D3D_API int __stdcall D3D_ReleaseLastFrame();
