cmake_minimum_required(VERSION 3.20)
project(eosprobe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(eosprobe eosprobe.cpp)
target_compile_definitions(eosprobe PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(eosprobe PRIVATE /utf-8 /W4 /EHsc)
target_link_libraries(eosprobe PRIVATE PortableDeviceGuids ole32 windowscodecs)
