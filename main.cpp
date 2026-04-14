//
// viewfinder — capture card viewer
// Video: Windows Media Foundation (IMFSourceReader)
// Audio: WASAPI loopback passthrough (mic → speakers)
// UI:    Win32 + Direct2D (GPU-scaled, bilinear)
// Build: cmake -B build -G "Visual Studio 17 2022" && cmake --build build --config Release
//

#include <windows.h>
#include <windowsx.h>

// Media Foundation
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

// WASAPI audio
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

// WRL ComPtr
#include <wrl/client.h>

// Direct3D 11 + DXGI
#include <d3d11.h>
#include <d3d11_4.h>   // ID3D11Multithread
#include <dxgi1_2.h>

// Direct2D 1.1
#include <d2d1_1.h>
#include <d2d1_1helper.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>

using Microsoft::WRL::ComPtr;

// Fallback pragma links (MSVC; CMakeLists.txt covers other generators)
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const wchar_t kClassName[]  = L"ViewfinderWnd";
static const wchar_t kWindowTitle[] = L"Viewfinder";

#define WM_NEW_FRAME   (WM_USER + 1)

#define IDM_CAM_BASE     1000   // 1000–1999: camera items
#define IDM_MIC_BASE     2000   // 2000–2999: mic items
#define IDM_FULLSCREEN   3000
#define IDM_FIT_CAMERA   3001
#define IDM_MUTE         3002
#define IDM_QUIT         9999

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

struct DeviceInfo {
    std::wstring name;
    std::wstring id;   // symbolic link (video) or IMMDevice id (audio)
};

// Shared video frame — written by capture thread, read by WM_PAINT.
// MF decodes into system-memory BGRA; D2D uploads and scales on the GPU.
struct FrameBuffer {
    std::vector<BYTE> data;
    int   width  = 0;
    int   height = 0;
    LONG  stride = 0;   // MF convention: >0 = top-down, <0 = bottom-up

    std::mutex mtx;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static HWND g_hwnd = nullptr;

static FrameBuffer g_frame;

// Video
static ComPtr<IMFSourceReader> g_videoReader;
static std::thread             g_videoThread;
static std::atomic<bool>       g_videoRunning{ false };

// Audio
static std::thread       g_audioThread;
static std::atomic<bool> g_audioRunning{ false };
static std::atomic<bool> g_audioMuted{ false };

// Set to true when a frame is waiting to be painted; cleared in WM_PAINT.
// Prevents the video thread from flooding the message queue.
static std::atomic<bool> g_framePending{ false };

// D3D11 + DXGI swap chain
static ComPtr<ID3D11Device>         g_d3dDevice;
static ComPtr<ID3D11DeviceContext>  g_d3dCtx;       // immediate context (main-thread only)
static ComPtr<IDXGISwapChain1>      g_swapChain;

// D2D 1.1 — device context renders directly into the swap chain back buffer
static ComPtr<ID2D1Factory1>        g_d2dFactory;
static ComPtr<ID2D1DeviceContext>   g_d2dContext;
static ComPtr<ID2D1Bitmap1>         g_d2dTarget;   // swap chain back buffer as D2D render target

// CPU-upload BGRA path — used when the camera gives us RGB32 frames.
static ComPtr<ID2D1Bitmap>          g_swBitmap;
static D2D1_SIZE_U                  g_swBitmapSize = {};
static std::vector<BYTE>            g_flipBuffer;    // row-flip buffer for bottom-up frames

// GPU YUV→BGRA path — used when the camera gives us NV12 or YUY2 frames.
// The capture thread writes the raw YUV bytes into g_frame.data; the render
// thread uploads them to g_vpInTex, runs ID3D11VideoProcessor to convert into
// g_vpOutTex (BGRA), then D2D draws g_vpOutTex via g_vpBitmap.
static ComPtr<ID3D11VideoDevice>              g_vidDev;
static ComPtr<ID3D11VideoContext>             g_vidCtx;
static ComPtr<ID3D11VideoProcessorEnumerator> g_vpEnum;
static ComPtr<ID3D11VideoProcessor>           g_vp;
static ComPtr<ID3D11Texture2D>                g_vpInTex;
static ComPtr<ID3D11VideoProcessorInputView>  g_vpInView;
static ComPtr<ID3D11Texture2D>                g_vpOutTex;
static ComPtr<ID3D11VideoProcessorOutputView> g_vpOutView;
static ComPtr<ID2D1Bitmap1>                   g_vpBitmap;
static DXGI_FORMAT                            g_vpInFmt   = DXGI_FORMAT_UNKNOWN;
static UINT                                   g_vpInPitch = 0;
static UINT                                   g_vpW = 0;
static UINT                                   g_vpH = 0;

// Fullscreen
static bool             g_fullscreen    = false;
static WINDOWPLACEMENT  g_savedPlacement = { sizeof(WINDOWPLACEMENT) };

// Device lists (rebuilt on every right-click)
static std::vector<DeviceInfo> g_cameras;
static std::vector<DeviceInfo> g_mics;
static int g_curCam = -1;
static int g_curMic = -1;

// ---------------------------------------------------------------------------
// Device enumeration
// ---------------------------------------------------------------------------

static std::vector<DeviceInfo> EnumVideoDevices()
{
    std::vector<DeviceInfo> result;

    ComPtr<IMFAttributes> pAttr;
    if (FAILED(MFCreateAttributes(&pAttr, 1))) return result;
    pAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                   MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(pAttr.Get(), &ppActivate, &count))) return result;

    for (UINT32 i = 0; i < count; i++) {
        DeviceInfo info;

        WCHAR* name = nullptr; UINT32 nameLen = 0;
        if (SUCCEEDED(ppActivate[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &nameLen))) {
            info.name = name;
            CoTaskMemFree(name);
        } else {
            info.name = L"Camera " + std::to_wstring(i + 1);
        }

        WCHAR* link = nullptr; UINT32 linkLen = 0;
        if (SUCCEEDED(ppActivate[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &linkLen))) {
            info.id = link;
            CoTaskMemFree(link);
        }

        result.push_back(std::move(info));
        ppActivate[i]->Release();
    }
    CoTaskMemFree(ppActivate);
    return result;
}

static std::vector<DeviceInfo> EnumAudioCaptureDevices()
{
    std::vector<DeviceInfo> result;

    ComPtr<IMMDeviceEnumerator> pEnum;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&pEnum)))) return result;

    ComPtr<IMMDeviceCollection> pCol;
    if (FAILED(pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCol))) return result;

    UINT count = 0;
    pCol->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        ComPtr<IMMDevice> pDev;
        if (FAILED(pCol->Item(i, &pDev))) continue;

        DeviceInfo info;

        LPWSTR id = nullptr;
        if (SUCCEEDED(pDev->GetId(&id))) {
            info.id = id;
            CoTaskMemFree(id);
        }

        ComPtr<IPropertyStore> pProps;
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &var))
                && var.vt == VT_LPWSTR)
                info.name = var.pwszVal;
            PropVariantClear(&var);
        }

        if (info.name.empty()) info.name = L"Microphone " + std::to_wstring(i + 1);
        result.push_back(std::move(info));
    }
    return result;
}

// forward declaration — defined in "Window aspect ratio helpers" section below
static void ResizeWindowToAspect(int camW, int camH);

// forward declarations — defined in "D3D11 / D2D helpers" section below
static bool SetupVideoProcessor(UINT w, UINT h, DXGI_FORMAT inFmt);
static void ReleaseVideoProcessor();

// ---------------------------------------------------------------------------
// Video capture
// ---------------------------------------------------------------------------

static void VideoThreadProc(ComPtr<IMFSourceReader> reader)
{
    while (g_videoRunning) {
        DWORD streamIndex, flags;
        LONGLONG timestamp;
        ComPtr<IMFSample> pSample;

        HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                        0, &streamIndex, &flags, &timestamp, &pSample);

        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)))
            break;
        if (!pSample) continue;

        // Copy the decoded BGRA pixels into our shared buffer for the renderer.
        ComPtr<IMFMediaBuffer> pContig;
        if (FAILED(pSample->ConvertToContiguousBuffer(&pContig))) continue;

        BYTE* pData = nullptr; DWORD maxLen, curLen;
        if (FAILED(pContig->Lock(&pData, &maxLen, &curLen))) continue;
        {
            std::lock_guard<std::mutex> lk(g_frame.mtx);
            if (g_frame.data.size() != curLen) g_frame.data.resize(curLen);
            memcpy(g_frame.data.data(), pData, curLen);
        }
        pContig->Unlock();

        // Only post if no paint is already queued — avoids flooding the message queue
        // when the camera runs faster than the renderer.
        if (g_hwnd && !g_framePending.exchange(true))
            PostMessage(g_hwnd, WM_NEW_FRAME, 0, 0);
    }
}

static bool OpenCamera(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(g_cameras.size())) return false;

    // Tear down previous capture
    g_videoRunning = false;
    if (g_videoThread.joinable()) g_videoThread.join();
    g_videoReader.Reset();
    ReleaseVideoProcessor();
    { std::lock_guard<std::mutex> lk(g_frame.mtx);
      g_frame.data.clear(); g_frame.width = g_frame.height = 0; }

    // Create source by symbolic link
    ComPtr<IMFAttributes> pSrcAttr;
    MFCreateAttributes(&pSrcAttr, 2);
    pSrcAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    pSrcAttr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        g_cameras[idx].id.c_str());

    ComPtr<IMFMediaSource> pSource;
    if (FAILED(MFCreateDeviceSource(pSrcAttr.Get(), &pSource))) return false;

    // Create source reader with software colour conversion.
    // We tried MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING (GPU video
    // processor, zero-copy DXGI samples) but at least one tested capture-card
    // driver fails inside MF on the second ReadSample (hr=0x80070428,
    // MF_SOURCE_READERF_ERROR after a ~3-second stall) regardless of how we
    // release the sample — a driver-side limitation, not a pool-pinning issue
    // in our code. Software decode + D2D GPU-accelerated scaling is the
    // reliable path.
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFAttributes> pSwAttr;
    MFCreateAttributes(&pSwAttr, 1);
    pSwAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (FAILED(MFCreateSourceReaderFromMediaSource(pSource.Get(), pSwAttr.Get(), &reader)))
        return false;

    // Pick the output format. Prefer the camera's native YUV layout (NV12 or
    // YUY2) so MF does no CPU colour conversion — we convert on the GPU with
    // ID3D11VideoProcessor. Fall back to RGB32 if neither YUV format is
    // accepted (MF will then convert in software).
    GUID        wantSubtype = MFVideoFormat_RGB32;
    DXGI_FORMAT vpFmt       = DXGI_FORMAT_UNKNOWN;
    {
        ComPtr<IMFMediaType> nativeType;
        GUID ns = {};
        if (g_vidDev
            && SUCCEEDED(reader->GetNativeMediaType(
                           MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeType))
            && SUCCEEDED(nativeType->GetGUID(MF_MT_SUBTYPE, &ns)))
        {
            if      (ns == MFVideoFormat_NV12) { wantSubtype = MFVideoFormat_NV12; vpFmt = DXGI_FORMAT_NV12; }
            else if (ns == MFVideoFormat_YUY2) { wantSubtype = MFVideoFormat_YUY2; vpFmt = DXGI_FORMAT_YUY2; }
        }
    }

    auto setSubtype = [&](const GUID& subtype) -> HRESULT {
        ComPtr<IMFMediaType> pOut;
        MFCreateMediaType(&pOut);
        pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOut->SetGUID(MF_MT_SUBTYPE,    subtype);
        return reader->SetCurrentMediaType(
                   MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pOut.Get());
    };

    if (FAILED(setSubtype(wantSubtype))) {
        // YUV attempt failed — retry with RGB32 so we at least get pixels.
        vpFmt = DXGI_FORMAT_UNKNOWN;
        if (FAILED(setSubtype(MFVideoFormat_RGB32))) return false;
    }

    // Read actual output type for dimensions / stride
    ComPtr<IMFMediaType> pActual;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pActual);

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(pActual.Get(), MF_MT_FRAME_SIZE, &w, &h);

    UINT32 strideRaw = 0;
    LONG   stride    = 0;
    if (SUCCEEDED(pActual->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideRaw)))
        stride = static_cast<LONG>(strideRaw);
    else if (vpFmt == DXGI_FORMAT_UNKNOWN)
        stride = static_cast<LONG>(w * 4);   // assume top-down RGB32
    else if (vpFmt == DXGI_FORMAT_YUY2)
        stride = static_cast<LONG>(w * 2);
    else
        stride = static_cast<LONG>(w);       // NV12 Y-plane pitch

    // If we locked in a YUV format, build the GPU conversion pipeline. If that
    // fails (unlikely on any D3D11 GPU), fall back to RGB32.
    if (vpFmt != DXGI_FORMAT_UNKNOWN && !SetupVideoProcessor(w, h, vpFmt)) {
        vpFmt = DXGI_FORMAT_UNKNOWN;
        if (FAILED(setSubtype(MFVideoFormat_RGB32))) return false;
        reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pActual);
        MFGetAttributeSize(pActual.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (SUCCEEDED(pActual->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideRaw)))
            stride = static_cast<LONG>(strideRaw);
        else
            stride = static_cast<LONG>(w * 4);
    }

    {
        std::lock_guard<std::mutex> lk(g_frame.mtx);
        g_frame.width  = static_cast<int>(w);
        g_frame.height = static_cast<int>(h);
        g_frame.stride = stride;
    }

    g_videoReader = reader;
    g_videoRunning = true;
    g_videoThread  = std::thread(VideoThreadProc, reader);
    g_curCam = idx;

    ResizeWindowToAspect(static_cast<int>(w), static_cast<int>(h));
    return true;
}

// ---------------------------------------------------------------------------
// Audio passthrough (WASAPI shared mode: mic → speakers)
// ---------------------------------------------------------------------------

// {00000003-0000-0010-8000-00aa00389b71} — IEEE float PCM subtype
static const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
};

static bool IsFloat32(const WAVEFORMATEX* f)
{
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && f->wBitsPerSample == 32)
        return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return (x->SubFormat == kSubtypeFloat) && f->wBitsPerSample == 32;
    }
    return false;
}

// Convert a block of audio frames between formats.
// Handles: float32 ↔ PCM16, any channel count, any sample-rate ratio
// (nearest-neighbour resampling — good enough for monitoring).
static void ConvertAudioBlock(
    const BYTE* src, UINT32 srcFrames, UINT32 srcCh, bool srcFloat, UINT32 srcBA,
          BYTE* dst, UINT32 dstFrames, UINT32 dstCh, bool dstFloat, UINT32 dstBA)
{
    for (UINT32 df = 0; df < dstFrames; ++df) {
        UINT32 sf = static_cast<UINT32>(static_cast<double>(df) * srcFrames / dstFrames);
        if (sf >= srcFrames) sf = srcFrames - 1;

        const BYTE* sRow = src + sf * srcBA;
              BYTE* dRow = dst + df * dstBA;

        for (UINT32 dc = 0; dc < dstCh; ++dc) {
            UINT32 sc = (dc < srcCh) ? dc : (srcCh - 1);

            float v = 0.0f;
            if (srcFloat)
                v = reinterpret_cast<const float*>(sRow)[sc];
            else
                v = reinterpret_cast<const int16_t*>(sRow)[sc] / 32768.0f;

            if (dstFloat)
                reinterpret_cast<float*>(dRow)[dc] = v;
            else
                reinterpret_cast<int16_t*>(dRow)[dc] =
                    static_cast<int16_t>(v * 32767.0f);
        }
    }
}

static void AudioThreadProc(std::wstring captureId)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> pEnum;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&pEnum)))) {
        CoUninitialize(); return;
    }

    // Capture device
    ComPtr<IMMDevice> pCapDev;
    if (!captureId.empty())
        pEnum->GetDevice(captureId.c_str(), &pCapDev);
    else
        pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pCapDev);

    // Render device (default speakers)
    ComPtr<IMMDevice> pRendDev;
    pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pRendDev);

    if (!pCapDev || !pRendDev) { CoUninitialize(); return; }

    // ---- Capture client — use capture device's own mix format ----
    ComPtr<IAudioClient> pCapClient;
    if (FAILED(pCapDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(pCapClient.GetAddressOf())))) {
        CoUninitialize(); return;
    }

    WAVEFORMATEX* pCapFmt = nullptr;
    pCapClient->GetMixFormat(&pCapFmt);

    constexpr REFERENCE_TIME kBufDur = 400000; // 40 ms
    if (FAILED(pCapClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                       kBufDur, 0, pCapFmt, nullptr))) {
        CoTaskMemFree(pCapFmt); CoUninitialize(); return;
    }

    // Event-driven: the audio engine signals this when a buffer is ready,
    // so the thread sleeps instead of polling every 10 ms.
    HANDLE hCapEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    pCapClient->SetEventHandle(hCapEvent);

    ComPtr<IAudioCaptureClient> pCapture;
    pCapClient->GetService(IID_PPV_ARGS(&pCapture));

    // ---- Render client — use render device's own mix format ----
    ComPtr<IAudioClient> pRendClient;
    if (FAILED(pRendDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(pRendClient.GetAddressOf())))) {
        CoTaskMemFree(pCapFmt); CoUninitialize(); return;
    }

    WAVEFORMATEX* pRendFmt = nullptr;
    pRendClient->GetMixFormat(&pRendFmt);

    if (FAILED(pRendClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                        kBufDur, 0, pRendFmt, nullptr))) {
        CoTaskMemFree(pCapFmt); CoTaskMemFree(pRendFmt); CoUninitialize(); return;
    }

    ComPtr<IAudioRenderClient> pRender;
    pRendClient->GetService(IID_PPV_ARGS(&pRender));

    if (!pCapture || !pRender) {
        CoTaskMemFree(pCapFmt); CoTaskMemFree(pRendFmt); CoUninitialize(); return;
    }

    UINT32 rendBufFrames = 0;
    pRendClient->GetBufferSize(&rendBufFrames);

    // Pre-compute format properties for the hot loop
    const UINT32 capRate   = pCapFmt->nSamplesPerSec;
    const UINT32 rendRate  = pRendFmt->nSamplesPerSec;
    const UINT32 capCh     = pCapFmt->nChannels;
    const UINT32 rendCh    = pRendFmt->nChannels;
    const UINT32 capBA     = pCapFmt->nBlockAlign;
    const UINT32 rendBA    = pRendFmt->nBlockAlign;
    const bool   capFloat  = IsFloat32(pCapFmt);
    const bool   rendFloat = IsFloat32(pRendFmt);

    // Direct memcpy is only safe when every parameter matches
    const bool directCopy = (capRate == rendRate) && (capCh == rendCh) && (capBA == rendBA);

    pCapClient->Start();
    pRendClient->Start();

    while (g_audioRunning) {
        // Sleep until the capture device signals a buffer is ready (or 200 ms timeout).
        WaitForSingleObject(hCapEvent, 200);

        UINT32 packetFrames = 0;
        while (SUCCEEDED(pCapture->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE*  pSrc   = nullptr;
            UINT32 nCapFr = 0;
            DWORD  flags  = 0;

            if (FAILED(pCapture->GetBuffer(&pSrc, &nCapFr, &flags, nullptr, nullptr))) break;

            // Scale frame count if sample rates differ
            UINT32 nRendFr = (capRate == rendRate)
                ? nCapFr
                : static_cast<UINT32>(static_cast<double>(nCapFr) * rendRate / capRate);

            UINT32 pad = 0;
            pRendClient->GetCurrentPadding(&pad);

            if (nRendFr > 0 && (rendBufFrames - pad) >= nRendFr) {
                BYTE* pDst = nullptr;
                if (SUCCEEDED(pRender->GetBuffer(nRendFr, &pDst))) {
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || g_audioMuted) {
                        memset(pDst, 0, nRendFr * rendBA);
                    } else if (directCopy) {
                        memcpy(pDst, pSrc, nCapFr * capBA);
                    } else {
                        ConvertAudioBlock(pSrc, nCapFr, capCh, capFloat, capBA,
                                          pDst, nRendFr, rendCh, rendFloat, rendBA);
                    }
                    pRender->ReleaseBuffer(nRendFr, 0);
                }
            }

            pCapture->ReleaseBuffer(nCapFr);
        }
    }

    pCapClient->Stop();
    pRendClient->Stop();
    CloseHandle(hCapEvent);
    CoTaskMemFree(pCapFmt);
    CoTaskMemFree(pRendFmt);
    CoUninitialize();
}

static bool OpenMic(int idx)
{
    g_audioRunning = false;
    if (g_audioThread.joinable()) g_audioThread.join();

    std::wstring id;
    if (idx >= 0 && idx < static_cast<int>(g_mics.size()))
        id = g_mics[idx].id;

    g_curMic = idx;
    g_audioRunning = true;
    g_audioThread  = std::thread(AudioThreadProc, std::move(id));
    return true;
}

// ---------------------------------------------------------------------------
// Window aspect ratio helpers
// ---------------------------------------------------------------------------

// Returns the non-client size overhead (frame + title bar)
static SIZE GetWindowFrameSize(HWND hwnd)
{
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    SIZE s;
    s.cx = (wr.right - wr.left) - cr.right;
    s.cy = (wr.bottom - wr.top) - cr.bottom;
    return s;
}

// Resize the window so the client area matches the camera's aspect ratio,
// capped at 90% of the primary monitor.
static void ResizeWindowToAspect(int camW, int camH)
{
    if (!g_hwnd || camW <= 0 || camH <= 0) return;

    SIZE frame = GetWindowFrameSize(g_hwnd);

    int monW = GetSystemMetrics(SM_CXSCREEN);
    int monH = GetSystemMetrics(SM_CYSCREEN);
    int maxCW = static_cast<int>(monW * 0.9f) - frame.cx;
    int maxCH = static_cast<int>(monH * 0.9f) - frame.cy;

    float scale = std::min(static_cast<float>(maxCW) / camW,
                           static_cast<float>(maxCH) / camH);
    if (scale > 1.0f) scale = 1.0f;   // don't upscale beyond native resolution

    int newCW = static_cast<int>(camW * scale);
    int newCH = static_cast<int>(camH * scale);

    SetWindowPos(g_hwnd, nullptr,
                 0, 0,
                 newCW + frame.cx, newCH + frame.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// Window placement persistence (registry)
// ---------------------------------------------------------------------------

static void SaveWindowPlacement(HWND hwnd)
{
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(hwnd, &wp)) return;

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Viewfinder",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(hKey, L"WindowPlacement", 0, REG_BINARY,
                   reinterpret_cast<const BYTE*>(&wp), sizeof(wp));
    RegCloseKey(hKey);
}

static bool LoadWindowPlacement(WINDOWPLACEMENT& wp)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Viewfinder",
                      0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD size = sizeof(wp);
    DWORD type = REG_BINARY;
    bool ok = RegQueryValueExW(hKey, L"WindowPlacement", nullptr, &type,
                               reinterpret_cast<BYTE*>(&wp), &size) == ERROR_SUCCESS
              && type == REG_BINARY && size == sizeof(wp) && wp.length == sizeof(wp);
    RegCloseKey(hKey);
    return ok;
}

// Toggle between windowed and borderless fullscreen on whichever monitor
// the window currently occupies.
static void ToggleFullscreen(HWND hwnd)
{
    if (!g_fullscreen) {
        // Save current placement so we can restore it later
        GetWindowPlacement(hwnd, &g_savedPlacement);

        // Cover the monitor the window is on (not necessarily the primary)
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT& m = mi.rcMonitor;

        SetWindowLong(hwnd, GWL_STYLE,
                      GetWindowLong(hwnd, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(hwnd, HWND_TOP,
                     m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = true;
    } else {
        SetWindowLong(hwnd, GWL_STYLE,
                      GetWindowLong(hwnd, GWL_STYLE) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &g_savedPlacement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

// ---------------------------------------------------------------------------
// D3D11 / D2D helpers
// ---------------------------------------------------------------------------

// Release per-frame and back-buffer resources (called on resize and shutdown).
// Does NOT release the factory, D3D device, or swap chain — only surface-bound objects.
static void DiscardD2DResources()
{
    g_swBitmap.Reset();
    g_swBitmapSize = {};
    g_d2dTarget.Reset();
    if (g_d2dContext) g_d2dContext->SetTarget(nullptr);
}

// Release all GPU-conversion resources tied to a specific camera configuration.
// Called when switching cameras or shutting down.
static void ReleaseVideoProcessor()
{
    g_vpBitmap.Reset();
    g_vpOutView.Reset(); g_vpOutTex.Reset();
    g_vpInView.Reset();  g_vpInTex.Reset();
    g_vp.Reset();        g_vpEnum.Reset();
    g_vpInFmt   = DXGI_FORMAT_UNKNOWN;
    g_vpInPitch = 0;
    g_vpW = g_vpH = 0;
}

// Build a D3D11 video processor that converts `inFmt` (NV12/YUY2) at `w`×`h`
// into a same-sized BGRA texture, plus a D2D bitmap view of that BGRA texture
// so the renderer can DrawBitmap it. Returns false on failure (caller should
// fall back to the CPU RGB32 path).
static bool SetupVideoProcessor(UINT w, UINT h, DXGI_FORMAT inFmt)
{
    if (!g_vidDev || !g_d3dDevice || !g_d2dContext) return false;
    ReleaseVideoProcessor();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat          = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate.Numerator  = 60; cd.InputFrameRate.Denominator  = 1;
    cd.InputWidth                = w;  cd.InputHeight                 = h;
    cd.OutputFrameRate.Numerator = 60; cd.OutputFrameRate.Denominator = 1;
    cd.OutputWidth               = w;  cd.OutputHeight                = h;
    cd.Usage                     = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(g_vidDev->CreateVideoProcessorEnumerator(&cd, &g_vpEnum)))  goto fail;
    if (FAILED(g_vidDev->CreateVideoProcessor(g_vpEnum.Get(), 0, &g_vp))) goto fail;

    {
        // Input texture — YUV. UpdateSubresource writes into this every frame.
        D3D11_TEXTURE2D_DESC td = {};
        td.Width              = w;
        td.Height             = h;
        td.MipLevels          = 1;
        td.ArraySize          = 1;
        td.Format             = inFmt;
        td.SampleDesc.Count   = 1;
        td.Usage              = D3D11_USAGE_DEFAULT;
        td.BindFlags          = 0;       // video processor input needs no bind
        if (FAILED(g_d3dDevice->CreateTexture2D(&td, nullptr, &g_vpInTex))) goto fail;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.ViewDimension            = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice       = 0;
        ivd.Texture2D.ArraySlice     = 0;
        if (FAILED(g_vidDev->CreateVideoProcessorInputView(
                       g_vpInTex.Get(), g_vpEnum.Get(), &ivd, &g_vpInView))) goto fail;

        // Output texture — BGRA. D2D draws from this.
        td.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_d3dDevice->CreateTexture2D(&td, nullptr, &g_vpOutTex))) goto fail;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
        ovd.ViewDimension         = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice    = 0;
        if (FAILED(g_vidDev->CreateVideoProcessorOutputView(
                       g_vpOutTex.Get(), g_vpEnum.Get(), &ovd, &g_vpOutView))) goto fail;
    }

    // Wrap the BGRA output texture as a D2D bitmap for DrawBitmap.
    {
        ComPtr<IDXGISurface> surf;
        if (FAILED(g_vpOutTex.As(&surf))) goto fail;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(g_d2dContext->CreateBitmapFromDxgiSurface(
                       surf.Get(), &bp, &g_vpBitmap))) goto fail;
    }

    // Colour-space hints. 1080p-class capture is typically BT.709 limited range;
    // treat anything 720p+ as 709 and smaller as 601. RGB output is full-range.
    {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs = {};
        inCs.YCbCr_Matrix  = (h >= 720) ? 1 : 0;   // 0=BT601, 1=BT709
        inCs.Nominal_Range = 1;                    // 1=16-235 (limited)
        g_vidCtx->VideoProcessorSetStreamColorSpace(g_vp.Get(), 0, &inCs);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs = {};
        outCs.RGB_Range = 0;                       // 0=full-range RGB
        g_vidCtx->VideoProcessorSetOutputColorSpace(g_vp.Get(), &outCs);
    }

    g_vpInFmt   = inFmt;
    g_vpW       = w;
    g_vpH       = h;
    // UpdateSubresource pitch:
    //   YUY2 is packed 4:2:2, 2 bytes/pixel → pitch = w*2.
    //   NV12 is planar — Y plane then interleaved UV. Pitch applies to both.
    g_vpInPitch = (inFmt == DXGI_FORMAT_YUY2) ? (w * 2) : w;
    return true;

fail:
    ReleaseVideoProcessor();
    return false;
}

// (Re)bind the D2D device context to the current swap chain back buffer.
// Call once after swap chain creation and again after ResizeBuffers.
static bool SetupSwapChainTarget()
{
    if (!g_swapChain || !g_d2dContext) return false;
    DiscardD2DResources();

    ComPtr<IDXGISurface> backBuf;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuf.GetAddressOf())))) return false;

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(g_d2dContext->CreateBitmapFromDxgiSurface(backBuf.Get(), &props, &g_d2dTarget)))
        return false;

    g_d2dContext->SetTarget(g_d2dTarget.Get());
    return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Shared letterbox calculation — returns dest rect given source and target sizes.
static D2D1_RECT_F Letterbox(float srcW, float srcH, float rtW, float rtH)
{
    float sc = (rtW / srcW < rtH / srcH) ? rtW / srcW : rtH / srcH;
    float dw = srcW * sc, dh = srcH * sc;
    float dx = (rtW - dw) * 0.5f, dy = (rtH - dh) * 0.5f;
    return D2D1::RectF(dx, dy, dx + dw, dy + dh);
}

static void RenderFrame(HWND /*hwnd*/)
{
    if (!g_d2dContext || !g_d2dTarget) return;

    g_d2dContext->BeginDraw();
    g_d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f));

    bool drawn = false;

    // ── GPU YUV→BGRA path ─────────────────────────────────────────────────
    // Camera gave us NV12/YUY2. Upload to g_vpInTex, run the video processor
    // to convert into g_vpOutTex (BGRA), then D2D draws g_vpOutTex via g_vpBitmap.
    if (g_vpBitmap && g_vpInTex && g_vidCtx && g_d3dCtx) {
        std::lock_guard<std::mutex> lk(g_frame.mtx);
        if (!g_frame.data.empty()) {
            g_d3dCtx->UpdateSubresource(
                g_vpInTex.Get(), 0, nullptr,
                g_frame.data.data(), g_vpInPitch, 0);

            RECT rect = { 0, 0, (LONG)g_vpW, (LONG)g_vpH };
            g_vidCtx->VideoProcessorSetStreamSourceRect(g_vp.Get(), 0, TRUE, &rect);
            g_vidCtx->VideoProcessorSetStreamDestRect  (g_vp.Get(), 0, TRUE, &rect);
            g_vidCtx->VideoProcessorSetOutputTargetRect(g_vp.Get(),    TRUE, &rect);

            D3D11_VIDEO_PROCESSOR_STREAM stream = {};
            stream.Enable         = TRUE;
            stream.OutputIndex    = 0;
            stream.InputFrameOrField = 0;
            stream.pInputSurface  = g_vpInView.Get();
            if (SUCCEEDED(g_vidCtx->VideoProcessorBlt(
                    g_vp.Get(), g_vpOutView.Get(), 0, 1, &stream)))
            {
                D2D1_SIZE_F bsz  = g_vpBitmap->GetSize();
                D2D1_SIZE_F rtsz = g_d2dContext->GetSize();
                g_d2dContext->DrawBitmap(g_vpBitmap.Get(),
                    Letterbox(bsz.width, bsz.height, rtsz.width, rtsz.height),
                    1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
                drawn = true;
            }
        }
    }

    // ── CPU BGRA path ─────────────────────────────────────────────────────
    // Camera gave us RGB32 (or GPU path wasn't set up). Upload each frame to
    // a D2D bitmap via CopyFromMemory and draw it letterboxed.
    if (!drawn) {
        std::lock_guard<std::mutex> lk(g_frame.mtx);
        const int fw = g_frame.width, fh = g_frame.height;

        if (!g_frame.data.empty() && fw > 0 && fh > 0) {
            const LONG   absStride = std::abs(g_frame.stride);
            const UINT32 pitch     = static_cast<UINT32>(absStride);

            if (!g_swBitmap ||
                g_swBitmapSize.width  != static_cast<UINT32>(fw) ||
                g_swBitmapSize.height != static_cast<UINT32>(fh))
            {
                g_swBitmap.Reset();
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8X8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                D2D1_SIZE_U sz = D2D1::SizeU(static_cast<UINT32>(fw), static_cast<UINT32>(fh));
                if (SUCCEEDED(g_d2dContext->CreateBitmap(sz, bp, &g_swBitmap)))
                    g_swBitmapSize = sz;
            }

            if (g_swBitmap) {
                const BYTE* src = g_frame.data.data();
                if (g_frame.stride < 0) {
                    const size_t rb = static_cast<size_t>(absStride);
                    g_flipBuffer.resize(rb * fh);
                    const BYTE* s = src + (fh - 1) * rb;
                    BYTE* d = g_flipBuffer.data();
                    for (int y = 0; y < fh; y++, s -= rb, d += rb)
                        memcpy(d, s, rb);
                    src = g_flipBuffer.data();
                }
                g_swBitmap->CopyFromMemory(nullptr, src, pitch);

                D2D1_SIZE_F sz = g_swBitmap->GetSize();
                D2D1_SIZE_F rtsz = g_d2dContext->GetSize();
                g_d2dContext->DrawBitmap(g_swBitmap.Get(),
                    Letterbox(sz.width, sz.height, rtsz.width, rtsz.height),
                    1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
            }
        }
    }

    HRESULT hr = g_d2dContext->EndDraw();
    if (FAILED(hr)) DiscardD2DResources();

    if (g_swapChain) g_swapChain->Present(0, 0);
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

static void ShowContextMenu(HWND hwnd, int screenX, int screenY)
{
    // Refresh device lists every time
    g_cameras = EnumVideoDevices();
    g_mics    = EnumAudioCaptureDevices();

    HMENU hRoot = CreatePopupMenu();
    HMENU hCam  = CreatePopupMenu();
    HMENU hMic  = CreatePopupMenu();

    for (int i = 0; i < static_cast<int>(g_cameras.size()); i++) {
        UINT flags = MF_STRING | (i == g_curCam ? MF_CHECKED : 0);
        AppendMenuW(hCam, flags, IDM_CAM_BASE + i, g_cameras[i].name.c_str());
    }
    if (g_cameras.empty())
        AppendMenuW(hCam, MF_STRING | MF_GRAYED, 0, L"No cameras found");

    for (int i = 0; i < static_cast<int>(g_mics.size()); i++) {
        UINT flags = MF_STRING | (i == g_curMic ? MF_CHECKED : 0);
        AppendMenuW(hMic, flags, IDM_MIC_BASE + i, g_mics[i].name.c_str());
    }
    if (g_mics.empty())
        AppendMenuW(hMic, MF_STRING | MF_GRAYED, 0, L"No microphones found");

    AppendMenuW(hRoot, MF_POPUP,     reinterpret_cast<UINT_PTR>(hCam), L"Camera");
    AppendMenuW(hRoot, MF_POPUP,     reinterpret_cast<UINT_PTR>(hMic), L"Microphone");
    AppendMenuW(hRoot, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hRoot, MF_STRING | (g_audioMuted ? MF_CHECKED : 0),
                IDM_MUTE,       L"Mute Microphone");
    AppendMenuW(hRoot, MF_STRING | (g_fullscreen ? MF_CHECKED : 0),
                IDM_FULLSCREEN, L"Full Screen\tF11");
    AppendMenuW(hRoot, MF_STRING,    IDM_FIT_CAMERA, L"Reset to Camera Size");
    AppendMenuW(hRoot, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hRoot, MF_STRING,    IDM_QUIT, L"Quit");

    TrackPopupMenu(hRoot, TPM_RIGHTBUTTON, screenX, screenY, 0, hwnd, nullptr);
    DestroyMenu(hRoot); // recursively destroys hCam and hMic too
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    case WM_NEW_FRAME:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        g_framePending = false;   // allow the next frame to queue a new paint
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);   // validates dirty region; D2D draws to the HWND directly
        RenderFrame(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // handled in WM_PAINT to avoid flicker

    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(hwnd, &pt);
        ShowContextMenu(hwnd, pt.x, pt.y);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= IDM_CAM_BASE && id < IDM_MIC_BASE) {
            OpenCamera(id - IDM_CAM_BASE);
        } else if (id >= IDM_MIC_BASE && id < IDM_FULLSCREEN) {
            OpenMic(id - IDM_MIC_BASE);
        } else if (id == IDM_FULLSCREEN) {
            ToggleFullscreen(hwnd);
        } else if (id == IDM_FIT_CAMERA) {
            if (g_fullscreen) ToggleFullscreen(hwnd);
            int cw, ch;
            { std::lock_guard<std::mutex> lk(g_frame.mtx); cw = g_frame.width; ch = g_frame.height; }
            ResizeWindowToAspect(cw, ch);
        } else if (id == IDM_MUTE) {
            g_audioMuted = !g_audioMuted;
        } else if (id == IDM_QUIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (g_fullscreen) ToggleFullscreen(hwnd);  // exit fullscreen first
            else              DestroyWindow(hwnd);      // quit only when windowed
        } else if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        ToggleFullscreen(hwnd);
        return 0;

    case WM_SIZING: {
        if (g_fullscreen) break;  // no constraints while fullscreen
        int camW, camH;
        { std::lock_guard<std::mutex> lk(g_frame.mtx); camW = g_frame.width; camH = g_frame.height; }
        if (camW <= 0 || camH <= 0) break;

        RECT* r = reinterpret_cast<RECT*>(lParam);
        SIZE frame = GetWindowFrameSize(hwnd);

        // Client dimensions from the proposed window rect
        int cw = (r->right  - r->left) - frame.cx;
        int ch = (r->bottom - r->top)  - frame.cy;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;

        // Adjust the opposite axis to maintain aspect ratio.
        // Which edges are "anchored" depends on which handle is being dragged.
        switch (wParam) {
        case WMSZ_LEFT:
        case WMSZ_RIGHT:
            // Width is driving; adjust height downward from top edge.
            ch = std::max(1, cw * camH / camW);
            r->bottom = r->top + ch + frame.cy;
            break;
        case WMSZ_TOP:
        case WMSZ_BOTTOM:
            // Height is driving; adjust width rightward from left edge.
            cw = std::max(1, ch * camW / camH);
            r->right = r->left + cw + frame.cx;
            break;
        case WMSZ_TOPLEFT:
        case WMSZ_TOPRIGHT:
            // Width drives; top edge moves.
            ch = std::max(1, cw * camH / camW);
            r->top = r->bottom - ch - frame.cy;
            break;
        case WMSZ_BOTTOMLEFT:
        case WMSZ_BOTTOMRIGHT:
            // Width drives; bottom edge moves.
            ch = std::max(1, cw * camH / camW);
            r->bottom = r->top + ch + frame.cy;
            break;
        }
        return TRUE;
    }

    case WM_SIZE: {
        UINT w = LOWORD(lParam), h = HIWORD(lParam);
        if (w > 0 && h > 0 && g_swapChain) {
            // Release back-buffer references before resizing the swap chain.
            DiscardD2DResources();
            g_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
            SetupSwapChainTarget();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_DESTROY:
        SaveWindowPlacement(hwnd);
        g_hwnd = nullptr;
        g_videoRunning = false;
        g_audioRunning = false;
        DiscardD2DResources();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    // COM (apartment-threaded for main; MF threads use their own)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    // Register window class
    WNDCLASSEX wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(101));
    wc.hIconSm       = static_cast<HICON>(
                           LoadImage(hInstance, MAKEINTRESOURCE(101),
                                     IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindowEx(
        0, kClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
        nullptr, nullptr, hInstance, nullptr);

    // Restore the last window placement, falling back to the OS default if
    // no saved state exists or the saved rect is entirely off-screen (e.g.
    // after disconnecting a monitor).
    {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        if (LoadWindowPlacement(wp) &&
            MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL) != nullptr)
        {
            if (wp.showCmd == SW_SHOWMINIMIZED) wp.showCmd = SW_SHOWNORMAL;
            SetWindowPlacement(hwnd, &wp);
            ShowWindow(hwnd, wp.showCmd);
        }
        else
        {
            ShowWindow(hwnd, nCmdShow);
        }
    }
    UpdateWindow(hwnd);

    // ── D3D11 device ──────────────────────────────────────────────────────────
    // BGRA_SUPPORT is required for D2D interop.
    // VIDEO_SUPPORT is required for ID3D11VideoDevice / VideoProcessor.
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                      nullptr, 0, D3D11_SDK_VERSION,
                      &g_d3dDevice, nullptr, &g_d3dCtx);

    if (g_d3dDevice) {
        // Video device/context for GPU-side colour conversion (NV12/YUY2 → BGRA).
        g_d3dDevice->QueryInterface(IID_PPV_ARGS(g_vidDev.GetAddressOf()));
        if (g_d3dCtx) g_d3dCtx->QueryInterface(IID_PPV_ARGS(g_vidCtx.GetAddressOf()));

        // ── DXGI swap chain ───────────────────────────────────────────────────
        ComPtr<IDXGIDevice1>  dxgiDev;
        ComPtr<IDXGIAdapter>  dxgiAdapter;
        ComPtr<IDXGIFactory2> dxgiFactory;
        g_d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDev.GetAddressOf()));
        dxgiDev->GetAdapter(&dxgiAdapter);
        dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.GetAddressOf()));

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc  = { 1, 0 };
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        dxgiFactory->CreateSwapChainForHwnd(g_d3dDevice.Get(), hwnd,
                                             &scd, nullptr, nullptr, &g_swapChain);

        // ── D2D 1.1 factory → device → device context ────────────────────────
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                          g_d2dFactory.GetAddressOf());

        ComPtr<ID2D1Device> d2dDev;
        g_d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDev);
        d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2dContext);

        // Use pixel units so our coordinates map 1:1 to physical swap-chain pixels.
        if (g_d2dContext) g_d2dContext->SetDpi(96.0f, 96.0f);

        SetupSwapChainTarget();
    }

    // Open first available camera and mic
    g_cameras = EnumVideoDevices();
    g_mics    = EnumAudioCaptureDevices();
    if (!g_cameras.empty()) OpenCamera(0);
    if (!g_mics.empty())    OpenMic(0);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup — stop threads before COM/D3D teardown
    g_videoRunning = false;
    g_audioRunning = false;
    if (g_videoThread.joinable()) g_videoThread.join();
    if (g_audioThread.joinable()) g_audioThread.join();
    g_videoReader.Reset();

    DiscardD2DResources();
    ReleaseVideoProcessor();
    g_vidCtx.Reset();
    g_vidDev.Reset();
    g_d2dContext.Reset();
    g_d2dFactory.Reset();
    g_swapChain.Reset();
    g_d3dCtx.Reset();
    g_d3dDevice.Reset();

    MFShutdown();
    CoUninitialize();

    return static_cast<int>(msg.wParam);
}
