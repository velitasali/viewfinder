cmake_minimum_required(VERSION 3.20)
project(viewfinder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(viewfinder WIN32 main.cpp)

target_compile_definitions(viewfinder PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00   # Windows 10+
)

target_compile_options(viewfinder PRIVATE
    $<$<CONFIG:Release>:/O1 /Os /GS->  # Optimize for size in release
)

target_link_libraries(viewfinder PRIVATE
    mf mfplat mfreadwrite mfuuid
    ole32 oleaut32 propsys
    user32 gdi32
)

# Remove console window and merge CRT for smaller binary
set_target_properties(viewfinder PROPERTIES
    LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup"
)
