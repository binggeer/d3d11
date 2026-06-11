#include "pch.h"
#include "d3d_internal.h"

#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#include <cwchar>
#include <objbase.h>
#include <roapi.h>
#ifndef RO_E_ALREADYINITIALIZED
#define RO_E_ALREADYINITIALIZED static_cast<HRESULT>(0x80000026L)
#endif
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace d3d {

namespace {

constexpr int kErrNotInit = -1;
constexpr int kErrDevice = -3;
constexpr int kErrSize = -5;
constexpr int kErrTexture = -6;
constexpr int kErrNoOutput = -9;
constexpr int kErrDuplicate = -10;
constexpr int kErrCrop = -11;
constexpr int kErrCapture = -12;
constexpr int kErrRender = -13;
constexpr int kMaxFrameDimension = 16384;

bool ComputeFrameBytes(int width, int height, int& bytesOut) {
    if (width <= 0 || height <= 0 || width > kMaxFrameDimension || height > kMaxFrameDimension)
        return false;
    const auto bytes = static_cast<int64_t>(width) * static_cast<int64_t>(height) * 4;
    if (bytes <= 0 || bytes > INT_MAX)
        return false;
    bytesOut = static_cast<int>(bytes);
    return true;
}

void SanitizeCropRect(CropRect& crop) {
    if (crop.x < 0)
        crop.x = 0;
    if (crop.y < 0)
        crop.y = 0;
    if (crop.w < 0)
        crop.w = 0;
    if (crop.h < 0)
        crop.h = 0;
}

const char* kVsSrc = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

const char* kPsSrc = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex0.Sample(samp0, uv);
}
)";

HRESULT CompileShader(const char* src, const char* entry, const char* profile, ComPtr<ID3DBlob>& blob) {
    ComPtr<ID3DBlob> err;
    return D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, profile,
        D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, blob.GetAddressOf(), err.GetAddressOf());
}

RECT ClientRect(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return rc;
}

bool IsRectEmpty(const RECT& rc) {
    return rc.right <= rc.left || rc.bottom <= rc.top;
}

bool WcsContainsI(const wchar_t* hay, const wchar_t* needle) {
    if (!hay || !needle || !*needle)
        return false;
    for (const wchar_t* p = hay; *p; ++p) {
        if (_wcsnicmp(p, needle, wcslen(needle)) == 0)
            return true;
    }
    return false;
}

// 0=NVIDIA 1=AMD 2=Intel 3=generic HW
int ClassifyHwEncoderVendor(const wchar_t* friendlyName) {
    if (!friendlyName || !*friendlyName)
        return -1;
    if (WcsContainsI(friendlyName, L"nvidia") || WcsContainsI(friendlyName, L"nvenc") ||
        WcsContainsI(friendlyName, L"geforce") || WcsContainsI(friendlyName, L"quadro"))
        return 0;
    if (WcsContainsI(friendlyName, L"amd") || WcsContainsI(friendlyName, L"amf") ||
        WcsContainsI(friendlyName, L"radeon") ||
        WcsContainsI(friendlyName, L"advanced micro"))
        return 1;
    if (WcsContainsI(friendlyName, L"intel") || WcsContainsI(friendlyName, L"quick sync") ||
        WcsContainsI(friendlyName, L"quicksync") || WcsContainsI(friendlyName, L"qsv") ||
        WcsContainsI(friendlyName, L"iris") || WcsContainsI(friendlyName, L"uhd graphics"))
        return 2;
    return 3;
}

const char* VendorEncoderLabel(int vendorClass) {
    switch (vendorClass) {
    case 0:
        return "HW(NVIDIA)";
    case 1:
        return "HW(AMD)";
    case 2:
        return "HW(Intel)";
    case 3:
        return "HW(h264)";
    default:
        return nullptr;
    }
}

bool WgcBorderToggleSupported() {
    try {
        return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
    } catch (...) {
        return false;
    }
}

void TryDisableWgcCaptureBorder(winrt::Windows::Graphics::Capture::GraphicsCaptureSession& session) {
    if (!WgcBorderToggleSupported())
        return;
    try {
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsMethodPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureAccess", L"RequestAccessAsync")) {
            try {
                winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
                    winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
                    .get();
            } catch (...) {
            }
        }
        // Set even if RequestAccess denied; honored when system policy allows (Win32CaptureSample).
        session.IsBorderRequired(false);
    } catch (...) {
    }
}

bool EnumHardwareEncoders(IMFActivate*** outActs, UINT32* outCount) {
    *outActs = nullptr;
    *outCount = 0;
    const MFT_REGISTER_TYPE_INFO input = { MFMediaType_Video, MFVideoFormat_NV12 };
    const MFT_REGISTER_TYPE_INFO output = { MFMediaType_Video, MFVideoFormat_H264 };
    const DWORD flags[] = {
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        MFT_ENUM_FLAG_HARDWARE,
    };
    for (DWORD flag : flags) {
        const HRESULT hr = MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER, flag, &input, &output, outActs, outCount);
        if (SUCCEEDED(hr) && *outCount > 0)
            return true;
        if (*outActs) {
            for (UINT32 i = 0; i < *outCount; ++i)
                (*outActs)[i]->Release();
            CoTaskMemFree(*outActs);
            *outActs = nullptr;
            *outCount = 0;
        }
    }
    return false;
}

// DXGI 辅助：无 MFT 名称时按显卡厂商兜底（仅说明用，不代表一定有可用编码器）
int ProbeEncoderVendorByDxgi() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return -1;

    bool hasNvidia = false;
    bool hasAmd = false;
    bool hasIntel = false;

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        switch (desc.VendorId) {
        case 0x10DE:
            hasNvidia = true;
            break;
        case 0x1002:
            hasAmd = true;
            break;
        case 0x8086:
            hasIntel = true;
            break;
        default:
            break;
        }
    }

    if (hasNvidia)
        return 0;
    if (hasAmd)
        return 1;
    if (hasIntel)
        return 2;
    return -1;
}

}  // namespace

char* ThreadLocalString(const std::string& src) {
    thread_local std::string buf;
    buf = src;
    return buf.data();
}

Engine& Engine::Instance() {
    static Engine inst;
    return inst;
}

Engine::Engine() {
    InitializeCriticalSection(&mtx_);
}

Engine::~Engine() {
    DeleteCriticalSection(&mtx_);
}

HWND Engine::NormalizeSourceHwnd(HWND hwnd) {
    if (hwnd == nullptr)
        return nullptr;
    if (IsWindow(hwnd))
        return hwnd;
    return nullptr;
}

void Engine::EnsureThreadCom() {
    thread_local bool tlsReady = false;
    if (tlsReady)
        return;

    // C++/WinRT requires init_apartment; raw CoInitializeEx/RoInitialize can AV on WGC calls.
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
        } catch (...) {
            const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (coHr != S_OK && coHr != S_FALSE && coHr != RPC_E_CHANGED_MODE)
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            const HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
            if (FAILED(roHr) && roHr != RO_E_ALREADYINITIALIZED)
                RoInitialize(RO_INIT_SINGLETHREADED);
        }
    }

    tlsReady = true;
}

void Engine::EnsureMfStarted() {
    if (mfStarted_)
        return;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        SetError("MFStartup failed");
    else
        mfStarted_ = true;
}

bool Engine::IsWgcSupported() {
    try {
        return winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

bool Engine::TryCreateWinRtDevice(
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice& out) {
    out = nullptr;
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) {
        SetError("IDXGIDevice");
        return false;
    }
    winrt::com_ptr<::IInspectable> inspectable;
    const HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
    if (FAILED(hr)) {
        SetError("CreateDirect3D11DeviceFromDXGIDevice");
        return false;
    }
    out = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    return true;
}

void Engine::SetLastErrorMessage(const std::string& msg) {
    EngineLock lock(mtx_);
    lastError_ = msg;
}

void Engine::SetError(const char* msg) {
    lastError_ = msg ? msg : "";
}

void Engine::SetError(const std::string& msg) {
    lastError_ = msg;
}

int Engine::ProbeHardwareEncoder(int hwProbe) {
    // 与易语言类一致：0=硬编探测（N/A/Intel 自动识别，CPU 兜底），1=强制 CPU
    if (hwProbe != 0) {
        encoderInfo_ = "CPU";
        return 0;
    }

    encoderInfo_ = "CPU";
    bool foundNvidia = false;
    bool foundAmd = false;
    bool foundIntel = false;
    bool foundGeneric = false;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (EnumHardwareEncoders(&activates, &count)) {
        for (UINT32 i = 0; i < count; ++i) {
            wchar_t* friendlyName = nullptr;
            UINT32 nameLen = 0;
            if (SUCCEEDED(activates[i]->GetAllocatedString(
                    MFT_FRIENDLY_NAME_Attribute, &friendlyName, &nameLen))) {
                switch (ClassifyHwEncoderVendor(friendlyName)) {
                case 0:
                    foundNvidia = true;
                    break;
                case 1:
                    foundAmd = true;
                    break;
                case 2:
                    foundIntel = true;
                    break;
                case 3:
                    foundGeneric = true;
                    break;
                default:
                    foundGeneric = true;
                    break;
                }
                CoTaskMemFree(friendlyName);
            } else {
                foundGeneric = true;
            }
            activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }

    if (foundNvidia)
        encoderInfo_ = "HW(NVIDIA)";
    else if (foundAmd)
        encoderInfo_ = "HW(AMD)";
    else if (foundIntel)
        encoderInfo_ = "HW(Intel)";
    else if (foundGeneric)
        encoderInfo_ = "HW(h264)";
    else {
        const int dxgiVendor = ProbeEncoderVendorByDxgi();
        if (const char* label = VendorEncoderLabel(dxgiVendor))
            encoderInfo_ = label;
        else
            encoderInfo_ = "CPU";
    }

    return 0;
}

bool Engine::ValidateCrop(int fullW, int fullH, const CropRect& crop) {
    if (crop.w <= 0 && crop.h <= 0)
        return crop.x >= 0 && crop.y >= 0 && crop.x < fullW && crop.y < fullH;
    const int w = crop.w > 0 ? crop.w : (fullW - crop.x);
    const int h = crop.h > 0 ? crop.h : (fullH - crop.y);
    if (w <= 0 || h <= 0)
        return false;
    return crop.x >= 0 && crop.y >= 0 && crop.x + w <= fullW && crop.y + h <= fullH;
}

void Engine::ApplyCropSize(int fullW, int fullH) {
    ApplyCropFromSource(fullW, fullH);
}

void Engine::ApplyCropFromSource(int fullW, int fullH) {
    const int x = (std::max)(0, crop_.x);
    const int y = (std::max)(0, crop_.y);
    int w = crop_.w > 0 ? crop_.w : (fullW - x);
    int h = crop_.h > 0 ? crop_.h : (fullH - y);
    w = (std::min)(w, fullW - x);
    h = (std::min)(h, fullH - y);
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    captureW_ = w;
    captureH_ = h;
    if (!ComputeFrameBytes(captureW_, captureH_, frameBytes_))
        frameBytes_ = 0;
}

void Engine::ReleaseCaptureSession() {
    if (capSession_) {
        try {
            capSession_.Close();
        } catch (...) {
        }
    }
    capSession_ = nullptr;
    framePool_ = nullptr;
    capItem_ = nullptr;
    winrtDevice_ = nullptr;
    poolSize_ = {};
}

void Engine::ReleaseDuplication() {
    if (duplication_) {
        duplication_->ReleaseFrame();
        duplication_.Reset();
    }
    dxgiOutput_.Reset();
    dxgiAdapter_.Reset();
}

int Engine::CreateD3dDevice() {
    device_.Reset();
    context_.Reset();
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL out = D3D_FEATURE_LEVEL_11_0;
    const HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, _countof(levels),
        D3D11_SDK_VERSION, device_.GetAddressOf(), &out, context_.GetAddressOf());
    if (FAILED(hr)) {
        SetError("D3D11CreateDevice failed");
        return kErrDevice;
    }
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context_.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);
    return 0;
}

int Engine::SetupWgcCapture() {
    ReleaseCaptureSession();
    useDuplication_ = false;

    if (!IsWgcSupported()) {
        SetError("WGC not supported");
        return kErrCapture;
    }

    try {
        if (!TryCreateWinRtDevice(winrtDevice_))
            return kErrDevice;
        auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        if (sourceHwnd_ == nullptr) {
            HMONITOR mon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
            if (!mon) {
                SetError("no monitor");
                return kErrNoOutput;
            }
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
            const HRESULT hr = interop->CreateForMonitor(
                mon, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(item));
            if (FAILED(hr)) {
                SetError("CreateForMonitor failed");
                return kErrNoOutput;
            }
            capItem_ = item;
        } else {
            if (!IsWindow(sourceHwnd_)) {
                SetError("invalid window");
                return kErrNoOutput;
            }
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
            const HRESULT hr = interop->CreateForWindow(
                sourceHwnd_, winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(item));
            if (FAILED(hr)) {
                SetError("CreateForWindow failed");
                return kErrNoOutput;
            }
            capItem_ = item;
        }

        const auto size = capItem_.Size();
        if (size.Width <= 0 || size.Height <= 0) {
            SetError("invalid capture size");
            return kErrCrop;
        }
        if (!ValidateCrop(size.Width, size.Height, crop_)) {
            SetError("invalid crop");
            return kErrCrop;
        }
        ApplyCropSize(size.Width, size.Height);

        poolSize_ = size;
        framePool_ = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice_,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, poolSize_);
        capSession_ = framePool_.CreateCaptureSession(capItem_);
        TryDisableWgcCaptureBorder(capSession_);
        try {
            capSession_.IsCursorCaptureEnabled(false);
        } catch (...) {
        }
        capSession_.StartCapture();
        captureReady_ = true;
        renderOnly_ = false;
        return 0;
    } catch (const winrt::hresult_error& ex) {
        char buf[128]{};
        sprintf_s(buf, "WGC 0x%08X", static_cast<unsigned>(ex.code().value));
        SetError(buf);
        return kErrCapture;
    } catch (...) {
        SetError("WGC setup failed");
        return kErrCapture;
    }
}

int Engine::SetupDuplicationCapture() {
    ReleaseDuplication();
    useDuplication_ = true;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) {
        SetError("IDXGIDevice");
        return kErrDevice;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
        SetError("GetAdapter");
        return kErrDevice;
    }
    if (FAILED(adapter.As(&dxgiAdapter_))) {
        SetError("IDXGIAdapter1");
        return kErrDevice;
    }

    HMONITOR targetMon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    if (sourceHwnd_)
        targetMon = MonitorFromWindow(sourceHwnd_, MONITOR_DEFAULTTONEAREST);

    ComPtr<IDXGIOutput> output;
    for (UINT i = 0;; ++i) {
        if (FAILED(dxgiAdapter_->EnumOutputs(i, output.GetAddressOf())))
            break;
        DXGI_OUTPUT_DESC desc{};
        output->GetDesc(&desc);
        if (desc.Monitor == targetMon) {
            if (FAILED(output.As(&dxgiOutput_)))
                continue;
            break;
        }
        output.Reset();
    }
    if (!dxgiOutput_) {
        SetError("output not found");
        return kErrNoOutput;
    }

    const HRESULT dupHr = dxgiOutput_->DuplicateOutput(device_.Get(), duplication_.GetAddressOf());
    if (dupHr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || dupHr == E_ACCESSDENIED) {
        SetError("DuplicateOutput");
        return kErrDuplicate;
    }
    if (FAILED(dupHr)) {
        SetError("DuplicateOutput failed");
        return dupHr == DXGI_ERROR_NOT_FOUND ? kErrNoOutput : kErrDevice;
    }

    DXGI_OUTDUPL_DESC dupDesc{};
    duplication_->GetDesc(&dupDesc);
    const int fullW = static_cast<int>(dupDesc.ModeDesc.Width);
    const int fullH = static_cast<int>(dupDesc.ModeDesc.Height);
    if (!ValidateCrop(fullW, fullH, crop_)) {
        SetError("invalid crop");
        ReleaseDuplication();
        return kErrCrop;
    }
    ApplyCropSize(fullW, fullH);
    captureReady_ = true;
    renderOnly_ = false;
    return 0;
}

int Engine::RebuildCaptureSession() {
    ReleaseCaptureSession();
    ReleaseDuplication();
    captureTex_.Reset();
    stagingTex_.Reset();
    cpuFrame_.clear();
    captureReady_ = false;

    if (!device_ || !context_) {
        if (CreateD3dDevice() != 0)
            return kErrDevice;
    }

    // Desktop (hwnd 0): DXGI duplication first (no yellow border), WGC fallback.
    if (sourceHwnd_ == nullptr) {
        int r = SetupDuplicationCapture();
        if (r == 0)
            return 0;
        if (IsWgcSupported())
            return SetupWgcCapture();
        return r;
    }

    int r = kErrCapture;
    if (IsWgcSupported())
        r = SetupWgcCapture();
    return r;
}

int Engine::RebuildCapture() {
    device_.Reset();
    context_.Reset();
    return RebuildCaptureSession();
}

bool Engine::AcquireDuplicationFrame() {
    if (!duplication_)
        return false;

    ComPtr<IDXGIResource> resource;
    DXGI_OUTDUPL_FRAME_INFO info{};
    const HRESULT hr = duplication_->AcquireNextFrame(100, &info, resource.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        SetError("capture timeout");
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        SetError("duplication access lost");
        return false;
    }
    if (FAILED(hr)) {
        SetError("AcquireNextFrame failed");
        return false;
    }

    ComPtr<ID3D11Texture2D> src;
    if (FAILED(resource.As(&src))) {
        duplication_->ReleaseFrame();
        return false;
    }

    DXGI_OUTDUPL_DESC dupDesc{};
    duplication_->GetDesc(&dupDesc);
    ApplyCropFromSource(static_cast<int>(dupDesc.ModeDesc.Width),
        static_cast<int>(dupDesc.ModeDesc.Height));
    if (captureW_ <= 0 || captureH_ <= 0) {
        duplication_->ReleaseFrame();
        SetError("invalid crop");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);
    desc.Width = static_cast<UINT>(captureW_);
    desc.Height = static_cast<UINT>(captureH_);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;

    ComPtr<ID3D11Texture2D> cropped;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, cropped.GetAddressOf()))) {
        duplication_->ReleaseFrame();
        SetError("crop texture");
        return false;
    }

    const UINT left = static_cast<UINT>((std::max)(0, crop_.x));
    const UINT top = static_cast<UINT>((std::max)(0, crop_.y));
    const D3D11_BOX box = {
        left, top, 0,
        left + static_cast<UINT>(captureW_), top + static_cast<UINT>(captureH_), 1,
    };
    context_->CopySubresourceRegion(cropped.Get(), 0, 0, 0, 0, src.Get(), 0, &box);
    duplication_->ReleaseFrame();
    context_->Flush();
    captureTex_ = cropped;
    return true;
}

bool Engine::AcquireWgcFrame() {
    if (!framePool_)
        return false;

    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{ nullptr };
    for (int attempt = 0; attempt < 10; ++attempt) {
        frame = framePool_.TryGetNextFrame();
        if (frame)
            break;
        Sleep(1);
    }
    if (!frame) {
        SetError("no frame");
        return false;
    }

    const auto contentSize = frame.ContentSize();
    if (contentSize.Width <= 0 || contentSize.Height <= 0) {
        SetError("empty frame");
        return false;
    }

    if (contentSize.Width != poolSize_.Width || contentSize.Height != poolSize_.Height) {
        framePool_.Recreate(
            winrtDevice_,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, contentSize);
        poolSize_ = contentSize;
    }

    ApplyCropFromSource(contentSize.Width, contentSize.Height);
    if (captureW_ <= 0 || captureH_ <= 0) {
        SetError("invalid crop");
        return false;
    }

    const auto surface = frame.Surface();
    ComPtr<ID3D11Texture2D> src;
    {
        using Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;
        ComPtr<IDirect3DDxgiInterfaceAccess> access;
        const winrt::com_ptr<::IUnknown> unk = surface.as<::IUnknown>();
        if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&access))) ||
            FAILED(access->GetInterface(IID_PPV_ARGS(&src)))) {
            SetError("frame texture");
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    src->GetDesc(&srcDesc);

    const UINT left = static_cast<UINT>((std::max)(0, crop_.x));
    const UINT top = static_cast<UINT>((std::max)(0, crop_.y));
    const UINT right = (std::min)(left + static_cast<UINT>(captureW_), srcDesc.Width);
    const UINT bottom = (std::min)(top + static_cast<UINT>(captureH_), srcDesc.Height);
    if (right <= left || bottom <= top) {
        SetError("crop out of bounds");
        return false;
    }

    const UINT outW = right - left;
    const UINT outH = bottom - top;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = outW;
    desc.Height = outH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = srcDesc.Format;
    desc.SampleDesc = srcDesc.SampleDesc;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;

    ComPtr<ID3D11Texture2D> cropped;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, cropped.GetAddressOf()))) {
        SetError("crop texture");
        return false;
    }

    const D3D11_BOX box = { left, top, 0, right, bottom, 1 };
    context_->CopySubresourceRegion(cropped.Get(), 0, 0, 0, 0, src.Get(), 0, &box);
    context_->Flush();

    captureW_ = static_cast<int>(outW);
    captureH_ = static_cast<int>(outH);
    if (!ComputeFrameBytes(captureW_, captureH_, frameBytes_))
        frameBytes_ = 0;
    captureTex_ = cropped;
    return true;
}

bool Engine::TryAcquireNewFrame() {
    if (!captureReady_ && !renderOnly_)
        return false;

    stagingTex_.Reset();

    if (useDuplication_ && duplication_)
        return AcquireDuplicationFrame();

    if (framePool_)
        return AcquireWgcFrame();

    SetError("capture not ready");
    return false;
}

int Engine::CopyTextureToCpu() {
    if (!captureTex_)
        return -1;

    D3D11_TEXTURE2D_DESC desc{};
    captureTex_->GetDesc(&desc);
    captureW_ = static_cast<int>(desc.Width);
    captureH_ = static_cast<int>(desc.Height);
    if (!ComputeFrameBytes(captureW_, captureH_, frameBytes_)) {
        SetError("frame too large");
        return -1;
    }

    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;

    stagingTex_.Reset();
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, stagingTex_.GetAddressOf()))) {
        SetError("staging texture");
        return kErrTexture;
    }
    context_->CopyResource(stagingTex_.Get(), captureTex_.Get());
    context_->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        SetError("Map failed");
        return kErrCapture;
    }

    const size_t need = static_cast<size_t>(frameBytes_);
    cpuFrame_.resize(need);
    const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
    auto* dst = cpuFrame_.data();
    const int rowBytes = captureW_ * 4;
    for (int y = 0; y < captureH_; ++y) {
        memcpy(dst + static_cast<size_t>(y) * rowBytes,
            src + static_cast<size_t>(y) * mapped.RowPitch,
            static_cast<size_t>(rowBytes));
    }
    context_->Unmap(stagingTex_.Get(), 0);
    stagingTex_.Reset();
    return static_cast<int>(need);
}

int Engine::EnsureRenderDevice() {
    if (device_ && context_)
        return 0;
    return CreateD3dDevice();
}

void Engine::DestroyAllRenderers() {
    renderers_.clear();
}

WindowRenderContext& Engine::GetOrCreateRenderer(HWND hwnd) {
    auto it = renderers_.find(hwnd);
    if (it != renderers_.end())
        return it->second;

    WindowRenderContext rc{};
    rc.hwnd = hwnd;
    auto inserted = renderers_.emplace(hwnd, std::move(rc));
    return inserted.first->second;
}

bool Engine::EnsureGpuRenderer(WindowRenderContext& rc, int texW, int texH) {
    if (!device_ || !context_)
        return false;

    const RECT cr = ClientRect(rc.hwnd);
    const int cw = cr.right - cr.left;
    const int ch = cr.bottom - cr.top;
    if (cw <= 0 || ch <= 0)
        return false;

    if (rc.gpuOk && rc.clientW == cw && rc.clientH == ch && rc.frameTex) {
        D3D11_TEXTURE2D_DESC cur{};
        rc.frameTex->GetDesc(&cur);
        if (static_cast<int>(cur.Width) == texW && static_cast<int>(cur.Height) == texH)
            return true;
    }

    rc.gpuOk = false;
    rc.swapChain.Reset();
    rc.rtv.Reset();
    rc.frameTex.Reset();
    rc.frameSrv.Reset();
    rc.clientW = cw;
    rc.clientH = ch;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice)))
        return false;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())))
        return false;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
        return false;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = static_cast<UINT>(cw);
    scd.Height = static_cast<UINT>(ch);
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Scaling = DXGI_SCALING_STRETCH;

    ComPtr<IDXGISwapChain1> swapChain;
    if (FAILED(factory->CreateSwapChainForHwnd(
            device_.Get(), rc.hwnd, &scd, nullptr, nullptr, swapChain.GetAddressOf())))
        return false;

    factory->MakeWindowAssociation(rc.hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> backBuf;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf))))
        return false;
    if (FAILED(device_->CreateRenderTargetView(backBuf.Get(), nullptr, rc.rtv.GetAddressOf())))
        return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(texW);
    td.Height = static_cast<UINT>(texH);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, rc.frameTex.GetAddressOf())))
        return false;
    if (FAILED(device_->CreateShaderResourceView(rc.frameTex.Get(), nullptr, rc.frameSrv.GetAddressOf())))
        return false;

    if (!rc.vs) {
        ComPtr<ID3DBlob> vsBlob, psBlob;
        if (FAILED(CompileShader(kVsSrc, "main", "vs_4_0", vsBlob)))
            return false;
        if (FAILED(CompileShader(kPsSrc, "main", "ps_4_0", psBlob)))
            return false;
        if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                rc.vs.GetAddressOf())))
            return false;
        if (FAILED(device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                rc.ps.GetAddressOf())))
            return false;

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        device_->CreateSamplerState(&sd, rc.sampler.GetAddressOf());

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        device_->CreateRasterizerState(&rd, rc.raster.GetAddressOf());
    }

    rc.swapChain = swapChain;
    rc.gpuOk = true;
    return true;
}

bool Engine::RenderGpu(WindowRenderContext& rc, ID3D11ShaderResourceView* srv, int texW, int texH) {
    if (!rc.gpuOk || !rc.rtv || !srv)
        return false;

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(rc.clientW);
    vp.Height = static_cast<float>(rc.clientH);
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);
    const float clear[4] = { 0, 0, 0, 1 };
    context_->ClearRenderTargetView(rc.rtv.Get(), clear);
    context_->OMSetRenderTargets(1, rc.rtv.GetAddressOf(), nullptr);
    if (rc.raster)
        context_->RSSetState(rc.raster.Get());
    context_->VSSetShader(rc.vs.Get(), nullptr, 0);
    context_->PSSetShader(rc.ps.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* samp = rc.sampler.Get();
    context_->PSSetSamplers(0, 1, &samp);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->Draw(3, 0);
    context_->Flush();
    return SUCCEEDED(rc.swapChain->Present(1, 0));
}

bool Engine::RenderCaptureTexToWindow(WindowRenderContext& rc) {
    if (!captureTex_)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    captureTex_->GetDesc(&desc);
    const int texW = static_cast<int>(desc.Width);
    const int texH = static_cast<int>(desc.Height);
    if (texW <= 0 || texH <= 0)
        return false;

    if (!EnsureGpuRenderer(rc, texW, texH))
        return false;

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device_->CreateShaderResourceView(captureTex_.Get(), nullptr, srv.GetAddressOf())))
        return false;

    return RenderGpu(rc, srv.Get(), texW, texH);
}

bool Engine::RenderGdi(HWND hwnd, const void* pixels, int texW, int texH) {
    if (!pixels || texW <= 0 || texH <= 0)
        return false;
    const RECT cr = ClientRect(hwnd);
    if (IsRectEmpty(cr))
        return false;
    HDC hdc = GetDC(hwnd);
    if (!hdc)
        return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = texW;
    bmi.bmiHeader.biHeight = -texH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(hdc, 0, 0, cr.right - cr.left, cr.bottom - cr.top,
        0, 0, texW, texH, pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, hdc);
    return true;
}

void Engine::ResetCaptureUnlocked() {
    ReleaseCaptureSession();
    ReleaseDuplication();
    captureTex_.Reset();
    stagingTex_.Reset();
    cpuFrame_.clear();
    captureReady_ = false;
    renderOnly_ = false;
    useDuplication_ = false;
}

int Engine::Init(HWND sourceHwnd, int hwProbe, const CropRect& crop) {
    EngineLock lock(mtx_);
    EnsureThreadCom();
    EnsureMfStarted();
    ResetCaptureUnlocked();
    DestroyAllRenderers();
    context_.Reset();
    device_.Reset();
    captureW_ = captureH_ = frameBytes_ = 0;

    const HWND normalized = NormalizeSourceHwnd(sourceHwnd);
    if (sourceHwnd != nullptr && normalized == nullptr) {
        SetError("invalid hwnd");
        return kErrNoOutput;
    }
    sourceHwnd_ = normalized;
    crop_ = crop;
    SanitizeCropRect(crop_);

    if (!mfStarted_) {
        SetError("MFStartup failed");
        return kErrDevice;
    }

    ProbeHardwareEncoder(hwProbe);

    const int r = RebuildCapture();
    if (r != 0)
        return r;

    SetError("OK");
    return 0;
}

int Engine::SetSource(HWND sourceHwnd, const CropRect& crop) {
    EngineLock lock(mtx_);
    EnsureThreadCom();
    if (!captureReady_ && !renderOnly_) {
        SetError("not initialized");
        return kErrNotInit;
    }
    if (renderOnly_) {
        SetError("render-only mode");
        return kErrNotInit;
    }

    const HWND normalized = NormalizeSourceHwnd(sourceHwnd);
    if (sourceHwnd != nullptr && normalized == nullptr) {
        SetError("invalid hwnd");
        return kErrNoOutput;
    }
    sourceHwnd_ = normalized;
    crop_ = crop;
    SanitizeCropRect(crop_);
    const int r = RebuildCaptureSession();
    if (r == 0)
        SetError("OK");
    return r;
}

int Engine::SetRenderSize(int frameW, int frameH) {
    EngineLock lock(mtx_);
    EnsureThreadCom();
    if (frameW <= 0 || frameH <= 0) {
        SetError("invalid size");
        return kErrSize;
    }
    int bytes = 0;
    if (!ComputeFrameBytes(frameW, frameH, bytes)) {
        SetError("invalid size");
        return kErrSize;
    }

    if (!device_) {
        if (EnsureRenderDevice() != 0)
            return kErrDevice;
    }

    DestroyAllRenderers();
    ResetCaptureUnlocked();
    sourceHwnd_ = nullptr;
    crop_ = {};
    renderOnly_ = true;
    captureW_ = frameW;
    captureH_ = frameH;
    frameBytes_ = bytes;
    SetError("OK");
    return 0;
}

void Engine::ShutdownForDllDetach() {
    if (!TryEnterCriticalSection(&mtx_))
        return;
    struct Leave {
        CRITICAL_SECTION& cs;
        ~Leave() { LeaveCriticalSection(&cs); }
    } leave{ mtx_ };
    ResetCaptureUnlocked();
    DestroyAllRenderers();
    cpuFrame_.clear();
    context_.Reset();
    device_.Reset();
    captureW_ = captureH_ = frameBytes_ = 0;
    sourceHwnd_ = nullptr;
    crop_ = {};
    encoderInfo_ = "CPU";
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
    lastError_ = "OK";
}

void Engine::Shutdown() {
    EngineLock lock(mtx_);
    ResetCaptureUnlocked();
    DestroyAllRenderers();
    cpuFrame_.clear();
    context_.Reset();
    device_.Reset();
    captureW_ = captureH_ = frameBytes_ = 0;
    sourceHwnd_ = nullptr;
    crop_ = {};
    encoderInfo_ = "CPU";
    if (mfStarted_) {
        MFShutdown();
        mfStarted_ = false;
    }
    SetError("OK");
}

int Engine::GetCaptureWidth() const {
    EngineLock lock(mtx_);
    return captureW_;
}

int Engine::GetCaptureHeight() const {
    EngineLock lock(mtx_);
    return captureH_;
}

int Engine::GetCaptureImageByteCount() const {
    EngineLock lock(mtx_);
    if (!captureReady_ && !renderOnly_)
        return 0;
    if (captureW_ <= 0 || captureH_ <= 0 || frameBytes_ <= 0)
        return 0;
    return frameBytes_;
}

int Engine::GetDevicePtr() const {
    EngineLock lock(mtx_);
    return reinterpret_cast<int>(device_.Get());
}

int Engine::GetContextPtr() const {
    EngineLock lock(mtx_);
    return reinterpret_cast<int>(context_.Get());
}

int Engine::GetTexturePtr() const {
    EngineLock lock(mtx_);
    return reinterpret_cast<int>(captureTex_.Get());
}

int Engine::CaptureToTexture() {
    EngineLock lock(mtx_);
    EnsureThreadCom();
    if (!captureReady_) {
        SetError("not initialized");
        return 0;
    }
    if (!TryAcquireNewFrame()) {
        if (captureTex_) {
            SetError("OK");
            return reinterpret_cast<int>(captureTex_.Get());
        }
        if (lastError_.empty() || lastError_ == "OK")
            SetError("capture failed");
        return 0;
    }
    SetError("OK");
    return reinterpret_cast<int>(captureTex_.Get());
}

int Engine::CaptureToImageBytes(void* buffer, int bufferLen) {
    EngineLock lock(mtx_);
    EnsureThreadCom();
    if (!buffer || bufferLen <= 0) {
        SetError("invalid buffer");
        return -1;
    }
    if (!captureReady_) {
        SetError("not initialized");
        return -1;
    }

    const bool gotNew = TryAcquireNewFrame();
    if (!gotNew && !captureTex_) {
        SetError("capture failed");
        return -1;
    }

    if (bufferLen < frameBytes_) {
        SetError("buffer size mismatch");
        return -1;
    }

    const int n = CopyTextureToCpu();
    if (n <= 0)
        return -1;
    if (n > bufferLen || cpuFrame_.size() < static_cast<size_t>(n)) {
        SetError("buffer size mismatch");
        return -1;
    }
    memcpy(buffer, cpuFrame_.data(), static_cast<size_t>(n));
    SetError("OK");
    return n;
}

int Engine::RenderToWindow(HWND hwnd, const void* frameData, int frameLen) {
    EngineLock lock(mtx_);
    EnsureThreadCom();
    if (!hwnd || !IsWindow(hwnd)) {
        SetError("invalid hwnd");
        return kErrRender;
    }
    if (EnsureRenderDevice() != 0)
        return kErrDevice;

    auto& rc = GetOrCreateRenderer(hwnd);

    if ((!frameData || frameLen == 0) && captureTex_) {
        if (RenderCaptureTexToWindow(rc)) {
            SetError("OK");
            return 0;
        }
    }

    if (frameData && frameLen <= 0) {
        SetError("invalid frame length");
        return kErrRender;
    }

    const void* pixels = frameData;
    int texW = captureW_;
    int texH = captureH_;
    if (!pixels || frameLen == 0) {
        if (cpuFrame_.empty()) {
            SetError("no frame");
            return kErrRender;
        }
        pixels = cpuFrame_.data();
        frameLen = frameBytes_;
    }
    int expectedBytes = 0;
    if (!ComputeFrameBytes(texW, texH, expectedBytes) || frameLen != expectedBytes) {
        SetError("frame size mismatch");
        return kErrRender;
    }

    if (EnsureGpuRenderer(rc, texW, texH)) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(rc.frameTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            const auto* src = static_cast<const std::uint8_t*>(pixels);
            for (int y = 0; y < texH; ++y) {
                memcpy(static_cast<std::uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    src + static_cast<size_t>(y) * texW * 4,
                    static_cast<size_t>(texW) * 4);
            }
            context_->Unmap(rc.frameTex.Get(), 0);
        }
        if (RenderGpu(rc, rc.frameSrv.Get(), texW, texH)) {
            SetError("OK");
            return 0;
        }
    }

    if (RenderGdi(hwnd, pixels, texW, texH)) {
        SetError("OK");
        return 0;
    }
    SetError("render failed");
    return kErrRender;
}

int Engine::RenderBlackToWindow(HWND hwnd) {
    EngineLock lock(mtx_);
    EnsureThreadCom();
    if (!hwnd || !IsWindow(hwnd)) {
        SetError("invalid hwnd");
        return kErrRender;
    }
    if (EnsureRenderDevice() != 0)
        return kErrDevice;

    auto& rc = GetOrCreateRenderer(hwnd);
    const int texW = captureW_ > 0 ? captureW_ : 2;
    const int texH = captureH_ > 0 ? captureH_ : 2;
    int blackBytes = 0;
    if (!ComputeFrameBytes(texW, texH, blackBytes))
        blackBytes = 16;
    std::vector<std::uint8_t> black(static_cast<size_t>(blackBytes), 0);

    if (EnsureGpuRenderer(rc, texW, texH)) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(rc.frameTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            for (int y = 0; y < texH; ++y)
                memset(static_cast<std::uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, 0,
                    static_cast<size_t>(texW) * 4);
            context_->Unmap(rc.frameTex.Get(), 0);
        }
        if (RenderGpu(rc, rc.frameSrv.Get(), texW, texH)) {
            SetError("OK");
            return 0;
        }
    }

    if (RenderGdi(hwnd, black.data(), texW, texH)) {
        SetError("OK");
        return 0;
    }
    SetError("render failed");
    return kErrRender;
}

int Engine::ReleaseLastFrame() {
    EngineLock lock(mtx_);
    EnsureThreadCom();
    if (!captureReady_ && !renderOnly_) {
        SetError("not initialized");
        return kErrNotInit;
    }
    captureTex_.Reset();
    stagingTex_.Reset();
    cpuFrame_.clear();

    if (framePool_) {
        for (;;) {
            auto frame = framePool_.TryGetNextFrame();
            if (!frame)
                break;
        }
    }
    if (duplication_) {
        duplication_->ReleaseFrame();
    }
    SetError("OK");
    return 0;
}

const char* Engine::GetEncoderInfo() {
    EngineLock lock(mtx_);
    return ThreadLocalString(encoderInfo_);
}

const char* Engine::GetLastError() {
    EngineLock lock(mtx_);
    return ThreadLocalString(lastError_);
}

}  // namespace d3d
