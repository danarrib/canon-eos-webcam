cmake_minimum_required(VERSION 3.20)
project(canon-eos-webcam-linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
find_package(Threads REQUIRED)

# Service: libusb + TurboJPEG + PipeWire only (no GTK).
add_executable(canon-eos-webcam
  src/service/main.cpp
  src/service/log.cpp
  src/common/ini.cpp
  src/common/jpeg.cpp
  src/service/ptp_usb.cpp
  src/service/settings.cpp
  src/service/pipeline.cpp
  src/service/bridge.cpp
  src/service/pw_output.cpp
  src/service/control.cpp
  src/common/camera_values.cpp
  src/service/canon_camera.cpp
)
target_link_libraries(canon-eos-webcam PRIVATE PkgConfig::LIBUSB PkgConfig::TURBOJPEG PkgConfig::PIPEWIRE Threads::Threads)
target_include_directories(canon-eos-webcam PRIVATE src)

# Panel: GTK4 + TurboJPEG (preview decoding); talks to the service over the control socket only.
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
add_executable(canon-eos-webcam-panel
  src/panel/main.cpp
  src/panel/client.cpp
  src/common/ini.cpp
  src/common/jpeg.cpp
  src/common/camera_values.cpp
)
target_link_libraries(canon-eos-webcam-panel PRIVATE PkgConfig::GTK4 PkgConfig::TURBOJPEG)
target_include_directories(canon-eos-webcam-panel PRIVATE src)

include(GNUInstallDirs)
install(TARGETS canon-eos-webcam canon-eos-webcam-panel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
