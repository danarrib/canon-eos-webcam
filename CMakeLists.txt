cmake_minimum_required(VERSION 3.20)
project(eoscam CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL runs inside the Frame Server and must not depend on the VC++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX)
add_compile_options(/utf-8 /W4 /EHsc /permissive- /wd4324)

# Video source loaded by the Windows Frame Server. Lean on purpose: it only copies the ready NV12 frame
# from the channel and draws the warning screens (no WIC, Maxine or CUDA in here).
add_library(eoscam-source SHARED
    src/source/media_source.cpp
    src/source/renderer.cpp
    src/source/eoscam-source.def)
target_include_directories(eoscam-source PRIVATE src/common)
target_link_libraries(eoscam-source PRIVATE mfplat mfuuid ole32 runtimeobject gdi32 advapi32)

# NVIDIA Maxine Video Effects SDK (MIT): headers and "proxies" that load the DLL of the installed runtime
# (C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects) dynamically. No CUDA Toolkit needed.
add_library(maxine-proxy STATIC
    third_party/maxine/src/NVVideoEffectsProxy.cpp
    third_party/maxine/src/nvCVImageProxy.cpp)
target_include_directories(maxine-proxy PUBLIC third_party/maxine/include)
# third-party code: without the /W4 warnings
target_compile_options(maxine-proxy PRIVATE /W1)

set(EOSCAM_APP_SOURCES
    src/app/main.cpp
    src/app/canon_camera.cpp
    src/app/applog.cpp
    src/app/settings.cpp
    src/app/imaging.cpp
    src/app/maxine.cpp
    src/app/pipeline.cpp
    src/app/bridge.cpp
    src/app/panel.cpp)
set(EOSCAM_APP_LIBS PortableDeviceGuids mfplat mfuuid mfreadwrite mf mfsensorgroup ole32 windowscodecs shell32
    shlwapi comctl32 comdlg32 gdi32 user32 advapi32 uuid maxine-proxy)

# App: bridge to the camera + installation + tests
add_executable(eoscam ${EOSCAM_APP_SOURCES})
target_include_directories(eoscam PRIVATE src/common)
target_link_libraries(eoscam PRIVATE ${EOSCAM_APP_LIBS})

# Same bridge, without a console, with a tray icon and control panel (starts with Windows)
add_executable(eoscam-bg WIN32 ${EOSCAM_APP_SOURCES})
target_compile_definitions(eoscam-bg PRIVATE EOSCAM_BACKGROUND)
target_include_directories(eoscam-bg PRIVATE src/common)
target_link_libraries(eoscam-bg PRIVATE ${EOSCAM_APP_LIBS})

add_subdirectory(tools/eosprobe)
