#pragma once

#include <d3d11.h>
#include <d3d10misc.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace d3d {

using Microsoft::WRL::ComPtr;

struct CropRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct WindowRenderContext {
    HWND hwnd = nullptr;
    int clientW = 0;
    int clientH = 0;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> frameTex;
    ComPtr<ID3D11ShaderResourceView> frameSrv;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11RasterizerState> raster;
    bool gpuOk = false;
};

class Engine {
public:
    static Engine& Instance();

    int Init(HWND sourceHwnd, int hwProbe, const CropRect& crop);
    int SetSource(HWND sourceHwnd, const CropRect& crop);
    int SetRenderSize(int frameW, int frameH);
    void Shutdown();
    void ShutdownForDllDetach();
    void ResetCaptureUnlocked();
    int GetCaptureWidth() const;
    int GetCaptureHeight() const;
    int GetCaptureImageByteCount() const;
    int GetDevicePtr() const;
    int GetContextPtr() const;
    int GetTexturePtr() const;
    int CaptureToTexture();
    int CaptureToImageBytes(void* buffer, int bufferLen);
    int RenderToWindow(HWND hwnd, const void* frameData, int frameLen);
    int RenderBlackToWindow(HWND hwnd);
    int ReleaseLastFrame();
    const char* GetEncoderInfo();
    const char* GetLastError();
    void SetLastErrorMessage(const std::string& msg);

private:
    Engine() = default;

    mutable std::mutex mtx_;
    bool mfStarted_ = false;
    bool captureReady_ = false;
    bool renderOnly_ = false;
    bool useDuplication_ = false;

    HWND sourceHwnd_ = nullptr;
    CropRect crop_{};
    int captureW_ = 0;
    int captureH_ = 0;
    int frameBytes_ = 0;

    std::string encoderInfo_ = "CPU";
    std::string lastError_ = "OK";

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> captureTex_;
    ComPtr<ID3D11Texture2D> stagingTex_;
    std::vector<std::uint8_t> cpuFrame_;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem capItem_{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool_{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession capSession_{ nullptr };
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice_{ nullptr };
    winrt::Windows::Graphics::SizeInt32 poolSize_{};

    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<IDXGIAdapter1> dxgiAdapter_;
    ComPtr<IDXGIOutput1> dxgiOutput_;

    std::unordered_map<HWND, WindowRenderContext> renderers_;

    void EnsureThreadCom();
    void EnsureMfStarted();
    static HWND NormalizeSourceHwnd(HWND hwnd);
    bool TryCreateWinRtDevice(winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice& out);
    static bool IsWgcSupported();
    void SetError(const char* msg);
    void SetError(const std::string& msg);
    int ProbeHardwareEncoder(int hwProbe);
    bool ValidateCrop(int fullW, int fullH, const CropRect& crop);
    void ApplyCropSize(int fullW, int fullH);
    void ApplyCropFromSource(int fullW, int fullH);
    void ReleaseCaptureSession();
    void ReleaseDuplication();
    int CreateD3dDevice();
    int SetupWgcCapture();
    int SetupDuplicationCapture();
    int RebuildCapture();
    int RebuildCaptureSession();
    bool TryAcquireNewFrame();
    bool AcquireWgcFrame();
    bool AcquireDuplicationFrame();
    int CopyTextureToCpu();
    int EnsureRenderDevice();
    WindowRenderContext& GetOrCreateRenderer(HWND hwnd);
    void DestroyAllRenderers();
    bool EnsureGpuRenderer(WindowRenderContext& rc, int texW, int texH);
    bool RenderGpu(WindowRenderContext& rc, ID3D11ShaderResourceView* srv, int texW, int texH);
    bool RenderCaptureTexToWindow(WindowRenderContext& rc);
    bool RenderGdi(HWND hwnd, const void* pixels, int texW, int texH);
};

char* ThreadLocalString(const std::string& src);

}  // namespace d3d
