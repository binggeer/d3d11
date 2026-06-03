#include "pch.h"
#include "d3d_exports.h"
#include "d3d_internal.h"

#include <cstdio>

namespace {

constexpr int kErrInternal = -99;

template <typename Fn>
int GuardInt(Fn&& fn) {
    try {
        return fn();
    } catch (const winrt::hresult_error& e) {
        char buf[64]{};
        sprintf_s(buf, "HRESULT 0x%08X", static_cast<unsigned>(e.code().value));
        d3d::Engine::Instance().SetLastErrorMessage(buf);
        return kErrInternal;
    } catch (...) {
        d3d::Engine::Instance().SetLastErrorMessage("native exception");
        return kErrInternal;
    }
}

template <typename Fn>
void GuardVoid(Fn&& fn) {
    try {
        fn();
    } catch (const winrt::hresult_error& e) {
        char buf[64]{};
        sprintf_s(buf, "HRESULT 0x%08X", static_cast<unsigned>(e.code().value));
        d3d::Engine::Instance().SetLastErrorMessage(buf);
    } catch (...) {
        d3d::Engine::Instance().SetLastErrorMessage("native exception");
    }
}

HWND ToHwnd(int hwnd) {
    return reinterpret_cast<HWND>(static_cast<INT_PTR>(hwnd));
}

}  // namespace

D3D_API int __stdcall D3D_Init(int sourceHwnd, int hwProbe, int cropX, int cropY, int cropW, int cropH) {
    const d3d::CropRect crop{ cropX, cropY, cropW, cropH };
    return GuardInt([&] {
        return d3d::Engine::Instance().Init(ToHwnd(sourceHwnd), hwProbe, crop);
    });
}

D3D_API int __stdcall D3D_SetSource(int sourceHwnd, int cropX, int cropY, int cropW, int cropH) {
    const d3d::CropRect crop{ cropX, cropY, cropW, cropH };
    return GuardInt([&] {
        return d3d::Engine::Instance().SetSource(ToHwnd(sourceHwnd), crop);
    });
}

D3D_API int __stdcall D3D_SetRenderSize(int frameW, int frameH) {
    return GuardInt([&] { return d3d::Engine::Instance().SetRenderSize(frameW, frameH); });
}

D3D_API void __stdcall D3D_Shutdown() {
    GuardVoid([&] { d3d::Engine::Instance().Shutdown(); });
}

D3D_API int __stdcall D3D_GetCaptureWidth() {
    return GuardInt([&] { return d3d::Engine::Instance().GetCaptureWidth(); });
}

D3D_API int __stdcall D3D_GetCaptureHeight() {
    return GuardInt([&] { return d3d::Engine::Instance().GetCaptureHeight(); });
}

D3D_API int __stdcall D3D_GetCaptureImageByteCount() {
    return GuardInt([&] { return d3d::Engine::Instance().GetCaptureImageByteCount(); });
}

D3D_API int __stdcall D3D_GetDevicePtr() {
    return GuardInt([&] { return d3d::Engine::Instance().GetDevicePtr(); });
}

D3D_API int __stdcall D3D_GetContextPtr() {
    return GuardInt([&] { return d3d::Engine::Instance().GetContextPtr(); });
}

D3D_API int __stdcall D3D_GetTexturePtr() {
    return GuardInt([&] { return d3d::Engine::Instance().GetTexturePtr(); });
}

D3D_API int __stdcall D3D_CaptureToTexture() {
    return GuardInt([&] { return d3d::Engine::Instance().CaptureToTexture(); });
}

D3D_API int __stdcall D3D_CaptureToImageBytes(void* buffer, int bufferLen) {
    return GuardInt([&] { return d3d::Engine::Instance().CaptureToImageBytes(buffer, bufferLen); });
}

D3D_API int __stdcall D3D_RenderToWindow(int hwnd, void* frameData, int frameLen) {
    return GuardInt([&] {
        return d3d::Engine::Instance().RenderToWindow(ToHwnd(hwnd), frameData, frameLen);
    });
}

D3D_API int __stdcall D3D_RenderBlackToWindow(int hwnd) {
    return GuardInt([&] { return d3d::Engine::Instance().RenderBlackToWindow(ToHwnd(hwnd)); });
}

D3D_API const char* __stdcall D3D_GetEncoderInfo() {
    try {
        return d3d::Engine::Instance().GetEncoderInfo();
    } catch (...) {
        return "CPU";
    }
}

D3D_API const char* __stdcall D3D_GetLastError() {
    try {
        return d3d::Engine::Instance().GetLastError();
    } catch (...) {
        return "error";
    }
}

D3D_API int __stdcall D3D_ReleaseLastFrame() {
    return GuardInt([&] { return d3d::Engine::Instance().ReleaseLastFrame(); });
}
