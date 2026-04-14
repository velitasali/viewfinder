# Viewfinder

A lightweight Windows capture card viewer. Displays camera output and passes microphone audio through to your speakers with minimal latency.

Built with native Windows APIs — no Electron, no runtime dependencies. Binary is ~135 KB.

## Features

- Live camera preview with correct aspect ratio
- Microphone audio passthrough (mic → speakers) with automatic format conversion
- Right-click context menu to switch cameras and microphones
- Full screen mode
- Mute toggle
- Aspect-ratio-locked window resizing
- Reset window to the camera's native resolution

## Controls

| Input | Action |
|---|---|
| Right-click | Open context menu |
| Double-click | Toggle full screen |
| F11 | Toggle full screen |
| Escape | Exit full screen / Quit |

## Building

### Prerequisites

- **Visual Studio 2022** (or later) with the **Desktop development with C++** workload
  — this includes the MSVC compiler and Windows SDK.
- **Inno Setup 6** *(optional)* — only needed to produce the installer `.exe`.
  Download from [jrsoftware.org/isdl.php](https://jrsoftware.org/isdl.php).

### Quick build

Double-click `build.bat`, or run it from any command prompt:

```bat
cd C:\path\to\viewfinder
build.bat
```

The script:
1. Generates `viewfinder.ico` from `generate_icon.ps1`
2. Compiles `resources.rc` (embeds the icon into the executable)
3. Compiles `main.cpp` → `viewfinder.exe`
4. If Inno Setup 6 is installed, builds `dist\viewfinder-setup.exe`

> **Note:** `build.bat` has the MSVC and Windows SDK paths hard-coded for the
> machine it was developed on. If the build fails with *cl.exe not found*, open
> the file and update `MSVC_VER` and `SDK_VER` to match your installation, or
> use the CMake path below instead.

### CMake build (alternative)

If you have CMake installed:

```bat
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

The resulting executable is at `build\Release\viewfinder.exe`.

## Tech stack

| Layer | API |
|---|---|
| Video capture | Windows Media Foundation (`IMFSourceReader`) |
| Colour conversion | Direct3D 11 Video Processor (`ID3D11VideoProcessor`) |
| Rendering | Direct2D 1.1 (`ID2D1DeviceContext`, `ID2D1Bitmap1`) |
| Audio passthrough | WASAPI (`IAudioCaptureClient` → `IAudioRenderClient`) |
| Window / UI | Win32 |
| Installer | Inno Setup 6 |

## Rendering pipeline

```
Capture card (NV12 / YUY2)
    │  Windows Media Foundation — software decode
    ▼
CPU buffer (YUV bytes)
    │  UpdateSubresource → D3D11 input texture
    ▼
ID3D11VideoProcessor  ←  colour space: BT.601 limited→full
    │  VideoProcessorBlt → BGRA output texture
    ▼
ID2D1Bitmap1 (DXGI surface)
    │  DrawBitmap
    ▼
IDXGISwapChain1  →  screen
```

Media Foundation reads frames from the capture card in the card's native YUV format (NV12 or YUY2) when available, falling back to RGB32. If a YUV format is chosen, a `ID3D11VideoProcessor` converts it to BGRA entirely on the GPU before handing it to Direct2D, keeping CPU usage low. The final blit is hardware-accelerated through the DXGI swap chain.

Hardware-accelerated decode through the MF DXVA pipeline (`MF_SOURCE_READER_D3D_MANAGER`) was evaluated but found to be unreliable for uncompressed capture-card sources: the driver returns `ERROR_EXCEPTION_IN_SERVICE` on the second `ReadSample` call, causing the preview to stall. Software decode with GPU colour conversion is the practical optimum for this class of device.
