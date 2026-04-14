@echo off
setlocal

set MSVC_VER=14.50.35717
set SDK_VER=10.0.19041.0
set VSBASE=C:\Program Files\Microsoft Visual Studio\18\Community
set SDKBASE=C:\Program Files (x86)\Windows Kits\10

set PATH=%VSBASE%\VC\Tools\MSVC\%MSVC_VER%\bin\Hostx64\x64;%SDKBASE%\bin\%SDK_VER%\x64;%PATH%

set INCLUDE=%VSBASE%\VC\Tools\MSVC\%MSVC_VER%\include;%SDKBASE%\include\%SDK_VER%\um;%SDKBASE%\include\%SDK_VER%\ucrt;%SDKBASE%\include\%SDK_VER%\shared;%SDKBASE%\include\%SDK_VER%\winrt

set LIB=%VSBASE%\VC\Tools\MSVC\%MSVC_VER%\lib\x64;%SDKBASE%\lib\%SDK_VER%\um\x64;%SDKBASE%\lib\%SDK_VER%\ucrt\x64

:: ---- Icon ---------------------------------------------------------------
echo [1/3] Generating icon...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0generate_icon.ps1"
if %ERRORLEVEL% NEQ 0 echo       Warning: icon generation failed.

:: ---- Resources ----------------------------------------------------------
echo [2/3] Compiling resources...
set RES_ARG=
if exist viewfinder.ico (
    rc.exe /nologo resources.rc 2>nul
    if exist resources.res ( set RES_ARG=resources.res ) else (
        echo       Warning: rc.exe failed, embedding icon skipped.
    )
) else (
    echo       Warning: viewfinder.ico not found, skipping resources.
)

:: ---- Executable ---------------------------------------------------------
echo [3/3] Compiling viewfinder.exe...
cl.exe ^
  /W3 /O1 /Os /EHsc /GS- /nologo ^
  /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 ^
  /std:c++17 ^
  main.cpp %RES_ARG% ^
  /Fe:viewfinder.exe ^
  /link /SUBSYSTEM:WINDOWS ^
  mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ^
  ole32.lib oleaut32.lib propsys.lib ^
  user32.lib gdi32.lib d2d1.lib d3d11.lib dxgi.lib advapi32.lib

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build FAILED  [error %ERRORLEVEL%]
    exit /b %ERRORLEVEL%
)
echo.
echo Build succeeded^^!  ^>  viewfinder.exe

:: ---- Installer (optional) -----------------------------------------------
set "ISCC_X86=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
set "ISCC_X64=C:\Program Files\Inno Setup 6\ISCC.exe"
set "ISCC_USR=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
set "ISCC="
if exist "%ISCC_X86%" set "ISCC=%ISCC_X86%"
if exist "%ISCC_X64%" set "ISCC=%ISCC_X64%"
if exist "%ISCC_USR%" set "ISCC=%ISCC_USR%"

if not defined ISCC (
    echo.
    echo [Installer skipped - install Inno Setup 6 to enable]
    echo   https://jrsoftware.org/isdl.php
    goto :end
)

echo.
echo Building installer...
if not exist dist mkdir dist
call "%ISCC%" /Q installer.iss
if %ERRORLEVEL% == 0 (
    echo Installer written:  dist\viewfinder-setup.exe
) else (
    echo Warning: installer build failed.
)

:end
endlocal
