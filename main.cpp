//
// viewfinder — capture card viewer
// Video: Windows Media Foundation (IMFSourceReader)
// Audio: WASAPI loopback passthrough (mic → speakers)
// UI:    Win32 + GDI (StretchDIBits)
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

// Shared video frame — written by capture thread, read by WM_PAINT
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

        ComPtr<IMFMediaBuffer> pBuf;
        if (FAILED(pSample->ConvertToContiguousBuffer(&pBuf))) continue;

        BYTE* pData = nullptr; DWORD maxLen, curLen;
        if (FAILED(pBuf->Lock(&pData, &maxLen, &curLen))) continue;

        {
            std::lock_guard<std::mutex> lk(g_frame.mtx);
            g_frame.data.assign(pData, pData + curLen);
        }
        pBuf->Unlock();

        if (g_hwnd) PostMessage(g_hwnd, WM_NEW_FRAME, 0, 0);
    }
}

static bool OpenCamera(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(g_cameras.size())) return false;

    // Tear down previous capture
    g_videoRunning = false;
    if (g_videoThread.joinable()) g_videoThread.join();
    g_videoReader.Reset();
    { std::lock_guard<std::mutex> lk(g_frame.mtx); g_frame.data.clear(); g_frame.width = g_frame.height = 0; }

    // Create source by symbolic link
    ComPtr<IMFAttributes> pSrcAttr;
    MFCreateAttributes(&pSrcAttr, 2);
    pSrcAttr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    pSrcAttr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        g_cameras[idx].id.c_str());

    ComPtr<IMFMediaSource> pSource;
    if (FAILED(MFCreateDeviceSource(pSrcAttr.Get(), &pSource))) return false;

    // Create source reader with automatic color conversion
    ComPtr<IMFAttributes> pReaderAttr;
    MFCreateAttributes(&pReaderAttr, 1);
    pReaderAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(pSource.Get(), pReaderAttr.Get(), &reader)))
        return false;

    // Request RGB32 output
    ComPtr<IMFMediaType> pOut;
    MFCreateMediaType(&pOut);
    pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOut->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_RGB32);
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pOut.Get())))
        return false;

    // Read actual output type for dimensions / stride
    ComPtr<IMFMediaType> pActual;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pActual);

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(pActual.Get(), MF_MT_FRAME_SIZE, &w, &h);

    UINT32 strideRaw = 0;
    LONG   stride    = 0;
    if (SUCCEEDED(pActual->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideRaw)))
        stride = static_cast<LONG>(strideRaw);
    else
        stride = static_cast<LONG>(w * 4);   // assume top-down RGB32

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
    if (FAILED(pCapClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                       kBufDur, 0, pCapFmt, nullptr))) {
        CoTaskMemFree(pCapFmt); CoUninitialize(); return;
    }

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
        Sleep(10);

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
// Rendering
// ---------------------------------------------------------------------------

static void RenderFrame(HDC hdc, int cw, int ch)
{
    std::lock_guard<std::mutex> lk(g_frame.mtx);

    RECT rc = { 0, 0, cw, ch };

    if (g_frame.data.empty() || g_frame.width == 0) {
        FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(80, 80, 80));
        HFONT hFont = CreateFont(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT old = static_cast<HFONT>(SelectObject(hdc, hFont));
        DrawText(hdc, L"Right-click to select a camera", -1, &rc,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        DeleteObject(hFont);
        return;
    }

    const int fw = g_frame.width;
    const int fh = g_frame.height;

    // Letterbox: scale to fit, centered
    const float sx    = static_cast<float>(cw) / fw;
    const float sy    = static_cast<float>(ch) / fh;
    const float scale = (sx < sy) ? sx : sy;
    const int dw = static_cast<int>(fw * scale);
    const int dh = static_cast<int>(fh * scale);
    const int dx = (cw - dw) / 2;
    const int dy = (ch - dh) / 2;

    // Black letterbox bars
    if (dx > 0) {
        RECT bar = { 0, 0, dx, ch };           FillRect(hdc, &bar, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        bar = { dx + dw, 0, cw, ch };          FillRect(hdc, &bar, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    if (dy > 0) {
        RECT bar = { 0, 0, cw, dy };           FillRect(hdc, &bar, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        bar = { 0, dy + dh, cw, ch };          FillRect(hdc, &bar, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }

    // MF stride convention: >0 = top-down, <0 = bottom-up
    // GDI biHeight:         <0 = top-down, >0 = bottom-up
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = fw;
    bmi.bmiHeader.biHeight      = (g_frame.stride >= 0) ? -fh : fh;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);
    StretchDIBits(hdc,
                  dx, dy, dw, dh,
                  0,  0,  fw, fh,
                  g_frame.data.data(), &bmi,
                  DIB_RGB_COLORS, SRCCOPY);
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
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        RenderFrame(hdc, cr.right, cr.bottom);
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

    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_DESTROY:
        g_hwnd = nullptr;
        g_videoRunning = false;
        g_audioRunning = false;
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

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

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

    // Cleanup — stop threads before COM teardown
    g_videoRunning = false;
    g_audioRunning = false;
    if (g_videoThread.joinable()) g_videoThread.join();
    if (g_audioThread.joinable()) g_audioThread.join();
    g_videoReader.Reset();

    MFShutdown();
    CoUninitialize();

    return static_cast<int>(msg.wParam);
}
