cmake_minimum_required(VERSION 3.20)
project(eoscam CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# CRT estático: a DLL roda dentro do Frame Server e não deve depender do VC++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE NOMINMAX)
add_compile_options(/utf-8 /W4 /EHsc /permissive- /wd4324)

# Fonte de vídeo carregada pelo Frame Server do Windows. Fica enxuta de propósito: só copia o quadro
# NV12 pronto do canal e desenha as telas de aviso (nada de WIC, Maxine ou CUDA aqui dentro).
add_library(eoscam-source SHARED
    src/source/media_source.cpp
    src/source/renderer.cpp
    src/source/eoscam-source.def)
target_include_directories(eoscam-source PRIVATE src/common)
target_link_libraries(eoscam-source PRIVATE mfplat mfuuid ole32 runtimeobject gdi32 advapi32)

# NVIDIA Maxine Video Effects SDK (MIT): headers e "proxies" que carregam a DLL do runtime instalado
# (C:\Program Files\NVIDIA Corporation\NVIDIA Video Effects) dinamicamente. Não precisa do CUDA Toolkit.
add_library(maxine-proxy STATIC
    third_party/maxine/src/NVVideoEffectsProxy.cpp
    third_party/maxine/src/nvCVImageProxy.cpp)
target_include_directories(maxine-proxy PUBLIC third_party/maxine/include)
# código de terceiros: sem os avisos do /W4
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

# App: ponte com a câmera + instalação + teste
add_executable(eoscam ${EOSCAM_APP_SOURCES})
target_include_directories(eoscam PRIVATE src/common)
target_link_libraries(eoscam PRIVATE ${EOSCAM_APP_LIBS})

# Mesma ponte, sem console, com ícone na bandeja e painel de controle (inicia com o Windows)
add_executable(eoscam-bg WIN32 ${EOSCAM_APP_SOURCES})
target_compile_definitions(eoscam-bg PRIVATE EOSCAM_BACKGROUND)
target_include_directories(eoscam-bg PRIVATE src/common)
target_link_libraries(eoscam-bg PRIVATE ${EOSCAM_APP_LIBS})

add_subdirectory(tools/eosprobe)
