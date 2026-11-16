cmake_minimum_required(VERSION 3.20)
project(eoscam CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# CRT estático: a DLL roda dentro do Frame Server e não deve depender do VC++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX)
add_compile_options(/utf-8 /W4 /EHsc /permissive- /wd4324)

# Fonte de vídeo carregada pelo Frame Server do Windows
add_library(eoscam-source SHARED
    src/source/media_source.cpp
    src/source/renderer.cpp
    src/source/eoscam-source.def)
target_include_directories(eoscam-source PRIVATE src/common)
target_link_libraries(eoscam-source PRIVATE mfplat mfuuid ole32 windowscodecs runtimeobject gdi32 advapi32)

# App: ponte com a câmera + instalação + teste
add_executable(eoscam
    src/app/main.cpp
    src/app/canon_camera.cpp)
target_include_directories(eoscam PRIVATE src/common)
target_link_libraries(eoscam PRIVATE PortableDeviceGuids mfplat mfuuid mfreadwrite mf mfsensorgroup ole32 windowscodecs)

# Mesma ponte, sem janela, para rodar em segundo plano / iniciar com o Windows
add_executable(eoscam-bg WIN32
    src/app/main.cpp
    src/app/canon_camera.cpp)
target_compile_definitions(eoscam-bg PRIVATE EOSCAM_BACKGROUND)
target_include_directories(eoscam-bg PRIVATE src/common)
target_link_libraries(eoscam-bg PRIVATE PortableDeviceGuids mfplat mfuuid mfreadwrite mf mfsensorgroup ole32 windowscodecs)

add_subdirectory(tools/eosprobe)
