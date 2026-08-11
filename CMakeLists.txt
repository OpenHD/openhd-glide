################################################################################
# OpenHD
#
# Licensed under the GNU General Public License (GPL) Version 3.
#
# This software is provided "as-is," without warranty of any kind, express or
# implied, including but not limited to the warranties of merchantability,
# fitness for a particular purpose, and non-infringement. For details, see the
# full license in the LICENSE file provided with this source code.
#
# Non-Military Use Only:
# This software and its associated components are explicitly intended for
# civilian and non-military purposes. Use in any military or defense
# applications is strictly prohibited unless explicitly and individually
# licensed otherwise by the OpenHD Team.
#
# Contributors:
# A full list of contributors can be found at the OpenHD GitHub repository:
# https://github.com/OpenHD
#
# © OpenHD, All Rights Reserved.
################################################################################

cmake_minimum_required(VERSION 3.20)

project(openhd-glide
    VERSION 0.1.0
    LANGUAGES CXX
)

if(UNIX AND NOT APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)
endif()

include(FetchContent)
include(GNUInstallDirs)
include(CheckCXXSymbolExists)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(UNIX AND NOT APPLE)
    set(FETCHCONTENT_BASE_DIR "/tmp/openhd-glide-fetchcontent" CACHE PATH "FetchContent cache outside drvfs" FORCE)
endif()

option(OPENHD_GLIDE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(OPENHD_GLIDE_DEVICE_KMS "Enable the device-oriented DRM/KMS runtime mode" ON)
option(OPENHD_GLIDE_REQUIRE_KMS_GBM "Fail configure if DRM/KMS GBM/EGL/GLES support is unavailable" OFF)
option(OPENHD_GLIDE_REQUIRE_GSTREAMER "Fail configure if GStreamer appsink/DMABUF support is unavailable" OFF)
option(OPENHD_GLIDE_REQUIRE_FREETYPE "Fail configure if FreeType support is unavailable" OFF)
option(OPENHD_GLIDE_REQUIRE_RKMPP "Fail configure if native Rockchip MPP support is unavailable" OFF)
option(OPENHD_GLIDE_REQUIRE_IMXVPU "Fail configure if native i.MX VPU API 2 support is unavailable" OFF)
option(OPENHD_GLIDE_CROSS_PACKAGE "Build a portable architecture package without host-side shlibdeps scanning" OFF)
option(OPENHD_GLIDE_ENABLE_SDL2 "Enable the SDL2 development preview backend when available" ON)
option(OPENHD_GLIDE_ENABLE_GSTREAMER "Enable GStreamer video paths when available" ON)
option(OPENHD_GLIDE_ENABLE_CEDAR "Enable native Allwinner Cedar decoding when available" ON)
option(OPENHD_GLIDE_ENABLE_RKMPP "Enable native Rockchip MPP decoding when available" ON)
option(OPENHD_GLIDE_ENABLE_IMXVPU "Enable native NXP i.MX VPU API 2 decoding when available" ON)
option(OPENHD_GLIDE_INSTALL_SYSTEMD "Install the OpenHD-Glide systemd autostart unit on Linux" ON)

if(WIN32)
    set(OPENHD_GLIDE_DEVICE_KMS OFF CACHE BOOL "DRM/KMS is unavailable on Windows" FORCE)
    set(OPENHD_GLIDE_ENABLE_CEDAR OFF CACHE BOOL "Cedar is unavailable on Windows" FORCE)
    set(OPENHD_GLIDE_ENABLE_RKMPP OFF CACHE BOOL "Rockchip MPP is unavailable on Windows" FORCE)
    set(OPENHD_GLIDE_ENABLE_IMXVPU OFF CACHE BOOL "i.MX VPU is unavailable on Windows" FORCE)
    set(OPENHD_GLIDE_INSTALL_SYSTEMD OFF CACHE BOOL "systemd is unavailable on Windows" FORCE)
endif()
set(OPENHD_GLIDE_PACKAGE_SUFFIX "" CACHE STRING "Optional suffix for the Debian package name")
set(OPENHD_GLIDE_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Debian package version")
set(OPENHD_GLIDE_PACKAGE_ARCHITECTURE "" CACHE STRING "Debian package architecture")
set(OPENHD_GLIDE_EXTRA_DEBIAN_DEPENDS "" CACHE STRING "Additional comma-separated Debian package dependencies")
set(OPENHD_GLIDE_SYSTEMD_UNIT_DIR "lib/systemd/system" CACHE PATH "Systemd unit install directory")
set(OPENHD_GLIDE_SERVICE_LIBEXEC_DIR "lib/openhd-glide" CACHE PATH "OpenHD-Glide service helper install directory")

add_library(glide_common
    src/common/ipc.cpp
    src/common/logging.cpp
    src/common/mavlink_state.cpp
    src/common/mavlink_udp_bridge.cpp
    src/common/network_discovery.cpp
    src/common/preview_control.cpp
    src/glide_ui/offline_map_package.cpp
    src/platform/core_assignment.cpp
    src/platform/cpu_topology.cpp
    src/platform/drm_probe.cpp
    src/platform/process_probe.cpp
)

target_include_directories(glide_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    target_link_libraries(glide_common PUBLIC ws2_32)
endif()

if(MSVC)
    target_compile_options(glide_common PRIVATE /W4)
    if(OPENHD_GLIDE_WARNINGS_AS_ERRORS)
        target_compile_options(glide_common PRIVATE /WX)
    endif()
else()
    target_compile_options(glide_common PRIVATE -Wall -Wextra -Wpedantic)
    if(OPENHD_GLIDE_WARNINGS_AS_ERRORS)
        target_compile_options(glide_common PRIVATE -Werror)
    endif()
endif()

find_package(PkgConfig QUIET)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBDRM QUIET libdrm)
    pkg_check_modules(GBM QUIET gbm)
    pkg_check_modules(EGL QUIET egl)
    pkg_check_modules(GLESV2 QUIET glesv2)
    if(OPENHD_GLIDE_ENABLE_SDL2 AND NOT WIN32)
        pkg_check_modules(SDL2 QUIET sdl2)
    endif()
    pkg_check_modules(FREETYPE QUIET freetype2)
    if(OPENHD_GLIDE_ENABLE_GSTREAMER)
        pkg_check_modules(GSTREAMER QUIET gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0)
    elseif(OPENHD_GLIDE_REQUIRE_GSTREAMER)
        message(FATAL_ERROR "OPENHD_GLIDE_REQUIRE_GSTREAMER=ON conflicts with OPENHD_GLIDE_ENABLE_GSTREAMER=OFF")
    endif()
    if(OPENHD_GLIDE_ENABLE_IMXVPU)
        pkg_check_modules(IMXVPUAPI2 QUIET libimxvpuapi2)
        pkg_check_modules(IMXDMABUFFER QUIET libimxdmabuffer)
    elseif(OPENHD_GLIDE_REQUIRE_IMXVPU)
        message(FATAL_ERROR "OPENHD_GLIDE_REQUIRE_IMXVPU=ON conflicts with OPENHD_GLIDE_ENABLE_IMXVPU=OFF")
    endif()
endif()

# Native Windows builds normally obtain SDL2, ANGLE and FreeType through
# vcpkg. Keep pkg-config above for the official GStreamer SDK.
if(WIN32)
    if(OPENHD_GLIDE_ENABLE_SDL2)
        find_package(SDL2 CONFIG REQUIRED)
        if(TARGET SDL2::SDL2)
            set(SDL2_FOUND TRUE)
            set(SDL2_LIBRARIES SDL2::SDL2)
            set(SDL2_INCLUDE_DIRS "")
            set(SDL2_LIBRARY_DIRS "")
        endif()
    endif()
    if(NOT GLESV2_FOUND)
        find_package(unofficial-angle CONFIG QUIET)
        if(TARGET unofficial::angle::libGLESv2)
            set(GLESV2_FOUND TRUE)
            # SDL creates the Windows GLES context through EGL.  Link both
            # ANGLE runtime targets so vcpkg deploys libEGL.dll beside every
            # preview executable as well as libGLESv2.dll.
            set(GLESV2_LIBRARIES
                unofficial::angle::libGLESv2
                unofficial::angle::libEGL)
            if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
                set(GLESV2_INCLUDE_DIRS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/angle")
            endif()
        endif()
    endif()
    if(NOT FREETYPE_FOUND)
        find_package(Freetype QUIET)
        if(TARGET Freetype::Freetype)
            set(FREETYPE_FOUND TRUE)
            set(FREETYPE_LIBRARIES Freetype::Freetype)
        endif()
    endif()
    if(NOT SDL2_FOUND OR NOT GLESV2_FOUND)
        message(FATAL_ERROR "Windows desktop builds require SDL2 and ANGLE (install the vcpkg manifest dependencies)")
    endif()
endif()

if(GBM_FOUND)
    set(CMAKE_REQUIRED_INCLUDES ${GBM_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${GBM_LIBRARIES})
    check_cxx_symbol_exists(gbm_surface_create_with_modifiers2 "gbm.h" OPENHD_GLIDE_HAS_GBM_SURFACE_CREATE_WITH_MODIFIERS2)
    unset(CMAKE_REQUIRED_LIBRARIES)
    unset(CMAKE_REQUIRED_INCLUDES)
endif()

option(OPENHD_GLIDE_PREFER_SYSTEM_MESA "Prefer distro GBM/EGL/GLES libraries over /usr/local Mesa on device builds" ON)
if(OPENHD_GLIDE_PREFER_SYSTEM_MESA AND UNIX AND NOT APPLE)
    if(CMAKE_LIBRARY_ARCHITECTURE)
        set(OPENHD_GLIDE_SYSTEM_LIBRARY_DIR "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
    else()
        set(OPENHD_GLIDE_SYSTEM_LIBRARY_DIR "/usr/lib")
    endif()
    find_library(OPENHD_GLIDE_SYSTEM_GBM NAMES gbm PATHS "${OPENHD_GLIDE_SYSTEM_LIBRARY_DIR}" NO_DEFAULT_PATH)
    find_library(OPENHD_GLIDE_SYSTEM_EGL NAMES EGL PATHS "${OPENHD_GLIDE_SYSTEM_LIBRARY_DIR}" NO_DEFAULT_PATH)
    find_library(OPENHD_GLIDE_SYSTEM_GLESV2 NAMES GLESv2 PATHS "${OPENHD_GLIDE_SYSTEM_LIBRARY_DIR}" NO_DEFAULT_PATH)
    if(OPENHD_GLIDE_SYSTEM_GBM AND OPENHD_GLIDE_SYSTEM_EGL AND OPENHD_GLIDE_SYSTEM_GLESV2)
        set(GBM_LIBRARIES "${OPENHD_GLIDE_SYSTEM_GBM}")
        set(EGL_LIBRARIES "${OPENHD_GLIDE_SYSTEM_EGL}")
        set(GLESV2_LIBRARIES "${OPENHD_GLIDE_SYSTEM_GLESV2}")
        set(GBM_LIBRARY_DIRS "")
        set(EGL_LIBRARY_DIRS "")
        set(GLESV2_LIBRARY_DIRS "")
        set(CMAKE_BUILD_RPATH "${OPENHD_GLIDE_SYSTEM_LIBRARY_DIR}")
        set(CMAKE_INSTALL_RPATH "${OPENHD_GLIDE_SYSTEM_LIBRARY_DIR}")
    endif()
endif()

if(OPENHD_GLIDE_ENABLE_CEDAR)
    find_library(CEDAR_VDECODER_LIBRARY NAMES vdecoder)
    find_library(CEDAR_MEMADAPTER_LIBRARY NAMES MemAdapter)
    find_library(CEDAR_VE_LIBRARY NAMES VE)
    find_library(CEDAR_VIDEOENGINE_LIBRARY NAMES videoengine)
endif()
if(OPENHD_GLIDE_ENABLE_RKMPP)
    find_library(ROCKCHIP_MPP_LIBRARY NAMES rockchip_mpp)
elseif(OPENHD_GLIDE_REQUIRE_RKMPP)
    message(FATAL_ERROR "OPENHD_GLIDE_REQUIRE_RKMPP=ON conflicts with OPENHD_GLIDE_ENABLE_RKMPP=OFF")
endif()

set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/src/glide_ui/lv_conf.h CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.2.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lvgl)
if(MSVC AND NOT BUILD_SHARED_LIBS)
    # LVGL 9.2 marks global data as dllimport even when its target is static,
    # which leaves fonts unresolved in consumers. Keep the static interface
    # free of that DLL-only definition.
    get_target_property(LVGL_INTERFACE_DEFINITIONS lvgl INTERFACE_COMPILE_DEFINITIONS)
    if(LVGL_INTERFACE_DEFINITIONS)
        list(FILTER LVGL_INTERFACE_DEFINITIONS EXCLUDE REGEX "LV_ATTRIBUTE_EXTERN_DATA")
        set_property(TARGET lvgl PROPERTY INTERFACE_COMPILE_DEFINITIONS "${LVGL_INTERFACE_DEFINITIONS}")
    endif()
endif()
if(SDL2_FOUND)
    target_compile_definitions(lvgl PRIVATE OPENHD_GLIDE_HAS_LVGL_SDL=1 SDL_MAIN_HANDLED=1)
    target_include_directories(lvgl SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(lvgl PRIVATE ${SDL2_LIBRARIES})
    target_link_directories(lvgl PRIVATE ${SDL2_LIBRARY_DIRS})
else()
    target_compile_definitions(lvgl PRIVATE OPENHD_GLIDE_HAS_LVGL_SDL=0)
endif()

if(LIBDRM_FOUND)
    target_compile_definitions(glide_common PRIVATE OPENHD_GLIDE_HAS_LIBDRM=1)
    target_include_directories(glide_common PRIVATE ${LIBDRM_INCLUDE_DIRS})
    target_link_libraries(glide_common PRIVATE ${LIBDRM_LIBRARIES})
    target_link_directories(glide_common PRIVATE ${LIBDRM_LIBRARY_DIRS})
else()
    target_compile_definitions(glide_common PRIVATE OPENHD_GLIDE_HAS_LIBDRM=0)
endif()

function(add_glide_executable target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE glide_common)
endfunction()

add_library(glide_dev
    src/dev/desktop_video_pipeline.cpp
    src/dev/dmabuf_gles_video_renderer.cpp
    src/dev/kms_atomic_compositor.cpp
    src/dev/kms_dmabuf_video_plane.cpp
    src/dev/kms_gles_window.cpp
    src/dev/sdl_gles_window.cpp
)

target_link_libraries(glide_dev PUBLIC glide_common)

if(SDL2_FOUND)
    target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_SDL2=1 SDL_MAIN_HANDLED=1)
    target_include_directories(glide_dev PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(glide_dev PRIVATE ${SDL2_LIBRARIES})
    target_link_directories(glide_dev PRIVATE ${SDL2_LIBRARY_DIRS})
else()
    target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_SDL2=0)
endif()

if(GSTREAMER_FOUND)
    target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_GSTREAMER=1)
    target_include_directories(glide_dev PRIVATE ${GSTREAMER_INCLUDE_DIRS})
    target_link_libraries(glide_dev PRIVATE ${GSTREAMER_LIBRARIES})
    # glide_dev is static, so consumers must inherit the search path for the
    # GStreamer libraries referenced by desktop_video_pipeline.cpp.
    target_link_directories(glide_dev PUBLIC ${GSTREAMER_LIBRARY_DIRS})
else()
    target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_GSTREAMER=0)
endif()

if(GLESV2_FOUND)
    target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_GLESV2=1)
    target_include_directories(glide_dev PRIVATE ${GLESV2_INCLUDE_DIRS})
    target_link_libraries(glide_dev PRIVATE ${GLESV2_LIBRARIES})
    target_link_directories(glide_dev PRIVATE ${GLESV2_LIBRARY_DIRS})
else()
    target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_GLESV2=0)
endif()

if(LIBDRM_FOUND AND GBM_FOUND AND EGL_FOUND AND GLESV2_FOUND)
    target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_KMS_GBM=1)
    target_include_directories(glide_dev PRIVATE
        ${LIBDRM_INCLUDE_DIRS}
        ${GBM_INCLUDE_DIRS}
        ${EGL_INCLUDE_DIRS}
        ${GLESV2_INCLUDE_DIRS})
    target_link_libraries(glide_dev PRIVATE
        ${LIBDRM_LIBRARIES}
        ${GBM_LIBRARIES}
        ${EGL_LIBRARIES}
        ${GLESV2_LIBRARIES})
    target_link_directories(glide_dev PRIVATE
        ${LIBDRM_LIBRARY_DIRS}
        ${GBM_LIBRARY_DIRS}
        ${EGL_LIBRARY_DIRS}
        ${GLESV2_LIBRARY_DIRS})
    if(OPENHD_GLIDE_HAS_GBM_SURFACE_CREATE_WITH_MODIFIERS2)
        target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_GBM_SURFACE_CREATE_WITH_MODIFIERS2=1)
    else()
        target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_GBM_SURFACE_CREATE_WITH_MODIFIERS2=0)
    endif()
else()
    if(OPENHD_GLIDE_REQUIRE_KMS_GBM)
        message(FATAL_ERROR "OPENHD_GLIDE_REQUIRE_KMS_GBM=ON but libdrm, gbm, egl, or glesv2 development files were not found")
    endif()
    target_compile_definitions(glide_dev PUBLIC OPENHD_GLIDE_HAS_KMS_GBM=0)
endif()

if(MSVC)
    target_compile_options(glide_dev PRIVATE /W4)
    if(OPENHD_GLIDE_WARNINGS_AS_ERRORS)
        target_compile_options(glide_dev PRIVATE /WX)
    endif()
else()
    target_compile_options(glide_dev PRIVATE -Wall -Wextra -Wpedantic)
    if(OPENHD_GLIDE_WARNINGS_AS_ERRORS)
        target_compile_options(glide_dev PRIVATE -Werror)
    endif()
endif()

add_library(glide_flow_core
    src/glide_flow/altitude_widget.cpp
    src/glide_flow/bitmap_font.cpp
    src/glide_flow/fps_counter.cpp
    src/glide_flow/fps_overlay.cpp
    src/glide_flow/gles_text_renderer.cpp
    src/glide_flow/gpu_map_renderer.cpp
    src/glide_flow/link_overview.cpp
    src/glide_flow/naval_osd.cpp
    src/glide_flow/performance_horizon.cpp
    src/glide_flow/rocket_osd.cpp
    src/glide_flow/rover_osd.cpp
    src/glide_flow/simulated_attitude.cpp
    src/glide_flow/speed_widget.cpp
    src/glide_flow/vector_font.cpp
)

target_link_libraries(glide_flow_core PUBLIC glide_common glide_dev ZLIB::ZLIB)

if(MSVC)
    target_compile_options(glide_flow_core PRIVATE /W4)
    if(OPENHD_GLIDE_WARNINGS_AS_ERRORS)
        target_compile_options(glide_flow_core PRIVATE /WX)
    endif()
else()
    target_compile_options(glide_flow_core PRIVATE -Wall -Wextra -Wpedantic)
    if(OPENHD_GLIDE_WARNINGS_AS_ERRORS)
        target_compile_options(glide_flow_core PRIVATE -Werror)
    endif()
endif()

if(GLESV2_FOUND)
    target_compile_definitions(glide_flow_core PUBLIC OPENHD_GLIDE_HAS_GLESV2=1)
    target_include_directories(glide_flow_core PRIVATE ${GLESV2_INCLUDE_DIRS})
    target_link_libraries(glide_flow_core PRIVATE ${GLESV2_LIBRARIES})
    target_link_directories(glide_flow_core PRIVATE ${GLESV2_LIBRARY_DIRS})
else()
    target_compile_definitions(glide_flow_core PUBLIC OPENHD_GLIDE_HAS_GLESV2=0)
endif()

if(FREETYPE_FOUND)
    target_compile_definitions(glide_flow_core PRIVATE OPENHD_GLIDE_HAS_FREETYPE=1)
    target_include_directories(glide_flow_core PRIVATE ${FREETYPE_INCLUDE_DIRS})
    target_link_libraries(glide_flow_core PRIVATE ${FREETYPE_LIBRARIES})
    target_link_directories(glide_flow_core PRIVATE ${FREETYPE_LIBRARY_DIRS})
else()
    if(OPENHD_GLIDE_REQUIRE_FREETYPE)
        message(FATAL_ERROR "OPENHD_GLIDE_REQUIRE_FREETYPE=ON but FreeType development files were not found")
    endif()
    target_compile_definitions(glide_flow_core PRIVATE OPENHD_GLIDE_HAS_FREETYPE=0)
endif()

add_glide_executable(openhd-glide src/openhd_glide/main.cpp)
target_sources(openhd-glide PRIVATE
    src/video/cedar_rtp_decoder.cpp
    src/video/imx_vpu_rtp_decoder.cpp
    src/video/rockchip_mpp_rtp_decoder.cpp)
target_link_libraries(openhd-glide PRIVATE glide_dev glide_flow_core Threads::Threads)
if(LIBDRM_FOUND)
    target_include_directories(openhd-glide PRIVATE ${LIBDRM_INCLUDE_DIRS})
endif()
if(OPENHD_GLIDE_DEVICE_KMS)
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_DEVICE_KMS=1)
else()
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_DEVICE_KMS=0)
endif()
if(GSTREAMER_FOUND)
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_HAS_GSTREAMER=1)
    target_include_directories(openhd-glide PRIVATE ${GSTREAMER_INCLUDE_DIRS})
    target_link_libraries(openhd-glide PRIVATE ${GSTREAMER_LIBRARIES})
    target_link_directories(openhd-glide PRIVATE ${GSTREAMER_LIBRARY_DIRS})
else()
    if(OPENHD_GLIDE_REQUIRE_GSTREAMER)
        message(FATAL_ERROR "OPENHD_GLIDE_REQUIRE_GSTREAMER=ON but required GStreamer development modules were not found")
    endif()
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_HAS_GSTREAMER=0)
endif()
if(CEDAR_VDECODER_LIBRARY AND CEDAR_MEMADAPTER_LIBRARY AND CEDAR_VE_LIBRARY AND CEDAR_VIDEOENGINE_LIBRARY)
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_HAS_CEDAR=1)
    target_link_libraries(openhd-glide PRIVATE
        ${CEDAR_VDECODER_LIBRARY}
        ${CEDAR_MEMADAPTER_LIBRARY}
        ${CEDAR_VE_LIBRARY}
        ${CEDAR_VIDEOENGINE_LIBRARY})
else()
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_HAS_CEDAR=0)
endif()
if(ROCKCHIP_MPP_LIBRARY)
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_HAS_RKMPP=1)
    target_link_libraries(openhd-glide PRIVATE ${ROCKCHIP_MPP_LIBRARY})
else()
    if(OPENHD_GLIDE_REQUIRE_RKMPP)
        message(FATAL_ERROR "OPENHD_GLIDE_REQUIRE_RKMPP=ON but librockchip_mpp was not found")
    endif()
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_HAS_RKMPP=0)
endif()

add_glide_executable(glide-rkmpp-rtp-probe src/tools/rkmpp_rtp_probe.cpp)
target_sources(glide-rkmpp-rtp-probe PRIVATE src/video/rockchip_mpp_rtp_decoder.cpp)
target_link_libraries(glide-rkmpp-rtp-probe PRIVATE Threads::Threads)
if(LIBDRM_FOUND)
    target_include_directories(glide-rkmpp-rtp-probe PRIVATE ${LIBDRM_INCLUDE_DIRS})
endif()
if(ROCKCHIP_MPP_LIBRARY)
    target_compile_definitions(glide-rkmpp-rtp-probe PRIVATE OPENHD_GLIDE_HAS_RKMPP=1)
    target_link_libraries(glide-rkmpp-rtp-probe PRIVATE ${ROCKCHIP_MPP_LIBRARY})
else()
    target_compile_definitions(glide-rkmpp-rtp-probe PRIVATE OPENHD_GLIDE_HAS_RKMPP=0)
endif()

if(IMXVPUAPI2_FOUND AND IMXDMABUFFER_FOUND)
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_HAS_IMXVPU=1)
    target_include_directories(openhd-glide PRIVATE ${IMXVPUAPI2_INCLUDE_DIRS} ${IMXDMABUFFER_INCLUDE_DIRS})
    target_link_libraries(openhd-glide PRIVATE ${IMXVPUAPI2_LIBRARIES} ${IMXDMABUFFER_LIBRARIES})
    target_link_directories(openhd-glide PRIVATE ${IMXVPUAPI2_LIBRARY_DIRS} ${IMXDMABUFFER_LIBRARY_DIRS})
else()
    if(OPENHD_GLIDE_REQUIRE_IMXVPU)
        message(FATAL_ERROR "OPENHD_GLIDE_REQUIRE_IMXVPU=ON but libimxvpuapi2 and libimxdmabuffer were not found")
    endif()
    target_compile_definitions(openhd-glide PRIVATE OPENHD_GLIDE_HAS_IMXVPU=0)
endif()
add_glide_executable(glide-view src/glide_view/main.cpp)
if(GSTREAMER_FOUND)
    message(STATUS "GStreamer appsink support enabled for glide-view")
    target_compile_definitions(glide-view PRIVATE OPENHD_GLIDE_HAS_GSTREAMER=1)
    target_include_directories(glide-view PRIVATE ${GSTREAMER_INCLUDE_DIRS})
    target_link_libraries(glide-view PRIVATE ${GSTREAMER_LIBRARIES})
    target_link_directories(glide-view PRIVATE ${GSTREAMER_LIBRARY_DIRS})
else()
    if(OPENHD_GLIDE_REQUIRE_GSTREAMER)
        message(FATAL_ERROR "OPENHD_GLIDE_REQUIRE_GSTREAMER=ON but required GStreamer development modules were not found for glide-view")
    endif()
    message(STATUS "GStreamer appsink/DMABUF support disabled; install gstreamer-1.0, gstreamer-app-1.0, gstreamer-allocators-1.0, and gstreamer-video-1.0 development packages")
    target_compile_definitions(glide-view PRIVATE OPENHD_GLIDE_HAS_GSTREAMER=0)
endif()
add_glide_executable(glide-send src/tools/glide_send.cpp)
add_glide_executable(openhd-glide-ethernet src/tools/openhd_glide_ethernet.cpp)
add_executable(glide-ui src/glide_ui/main.cpp)
target_link_libraries(glide-ui PRIVATE glide_common lvgl)
if(OPENHD_GLIDE_DEVICE_KMS)
    target_compile_definitions(glide-ui PRIVATE OPENHD_GLIDE_DEVICE_KMS=1)
else()
    target_compile_definitions(glide-ui PRIVATE OPENHD_GLIDE_DEVICE_KMS=0)
endif()
if(SDL2_FOUND)
    target_compile_definitions(glide-ui PRIVATE OPENHD_GLIDE_HAS_LVGL_SDL=1 SDL_MAIN_HANDLED=1)
    target_include_directories(glide-ui PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(glide-ui PRIVATE lvgl ${SDL2_LIBRARIES})
    target_link_directories(glide-ui PRIVATE ${SDL2_LIBRARY_DIRS})
else()
    target_compile_definitions(glide-ui PRIVATE OPENHD_GLIDE_HAS_LVGL_SDL=0)
endif()
target_compile_definitions(glide-ui PRIVATE OPENHD_GLIDE_HAS_LVGL=1)

add_library(glide_minimap
    src/glide_ui/minimap_widget.cpp
)
target_link_libraries(glide_minimap PUBLIC glide_common lvgl ZLIB::ZLIB)
target_include_directories(glide_minimap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(glide-ui PRIVATE glide_minimap)

add_executable(glide-minimap-demo src/glide_ui/minimap_demo.cpp)
target_link_libraries(glide-minimap-demo PRIVATE glide_minimap)
if(SDL2_FOUND)
    target_compile_definitions(glide-minimap-demo PRIVATE OPENHD_GLIDE_HAS_LVGL_SDL=1 SDL_MAIN_HANDLED=1)
    target_include_directories(glide-minimap-demo PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(glide-minimap-demo PRIVATE lvgl ${SDL2_LIBRARIES})
    target_link_directories(glide-minimap-demo PRIVATE ${SDL2_LIBRARY_DIRS})
else()
    target_compile_definitions(glide-minimap-demo PRIVATE OPENHD_GLIDE_HAS_LVGL_SDL=0)
endif()

add_executable(glide-flow src/glide_flow/main.cpp)
target_link_libraries(glide-flow PRIVATE glide_flow_core glide_dev)
if(WIN32 AND TARGET unofficial::angle::libEGL)
    # SDL loads ANGLE's EGL frontend dynamically when SDL_VIDEO_FORCE_EGL is
    # enabled, so it does not appear in the executable import table and
    # vcpkg's automatic DLL deployment cannot discover it.
    add_custom_command(TARGET glide-flow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:unofficial::angle::libEGL>
            $<TARGET_FILE_DIR:glide-flow>)
endif()
if(OPENHD_GLIDE_DEVICE_KMS)
    target_compile_definitions(glide-flow PRIVATE OPENHD_GLIDE_DEVICE_KMS=1)
else()
    target_compile_definitions(glide-flow PRIVATE OPENHD_GLIDE_DEVICE_KMS=0)
endif()

install(
    TARGETS
        openhd-glide
        glide-view
        glide-flow
        glide-ui
        glide-minimap-demo
        glide-send
        glide-rkmpp-rtp-probe
        openhd-glide-ethernet
    RUNTIME DESTINATION bin
)

install(
    PROGRAMS
        scripts/build-auto.sh
        scripts/generate_fake_minimap_tiles.py
        scripts/generate_offline_osm_tiles.py
        tools/artosyn_rtp_fixture.py
    DESTINATION share/openhd-glide/scripts
)

install(
    FILES
        packaging/assets/x20_header.h264
        packaging/assets/x21_header.h264
    DESTINATION share/openhd-glide
)

install(
    DIRECTORY assets/maps/packages/
    DESTINATION share/openhd-glide/assets/maps/packages
    FILES_MATCHING PATTERN "*.glidemap"
)

install(
    PROGRAMS
        examples/run-glide-view-decode-only.sh
        examples/run-kms-stack.sh
        examples/kms-example-common.sh
        examples/buildrun-orangepi-zero3w-bookworm.sh
        examples/run-kms-video-cedar-flow.sh
        examples/run-kms-video-cedar-video-only.sh
        examples/run-kms-video-gstreamer-flow.sh
        examples/run-kms-video-gstreamer-flow-ui.sh
        examples/run-kms-video-gstreamer-flow-30fps.sh
        examples/run-kms-video-gstreamer-video-only.sh
        examples/run-kms-video-rpi-video-only.sh
        examples/run-kms-video-imxvpu-flow-ui.sh
        examples/run-kms-video-rkmpp-video-only.sh
        examples/run-kms-video-rkmpp-flow-ui.sh
        examples/autorun-local-video-to-glide.sh
        examples/stream-blurbusters-1080p120-to-glide-view.sh
        examples/stream-h264-file-to-glide-view.sh
        examples/stream-orqa-video3-to-glide.sh
        examples/stream-rpicam-to-glide.sh
        examples/stream-videotestsrc-to-glide-auto.sh
        examples/stream-videotestsrc-to-glide-view.sh
        examples/run-wsl-flow-preview.sh
        examples/run-wsl-ui-preview.sh
        examples/run-wsl-minimap-preview.sh
        examples/gps-mavlink-emulator.py
        examples/run-gps-emulator.ps1
        examples/run-wsl-video-decode.sh
        examples/capture-artosyn-rtp.sh
        examples/run-rkmpp-isolated-probe.sh
    DESTINATION share/openhd-glide/examples
)

install(
    FILES
        testdata/artosyn/p401-midstream-no-idr.rtp
        testdata/artosyn/p401-midstream-no-idr.rtp.json
        testdata/artosyn/header-injection-reproduction.txt
        testdata/artosyn/README.md
    DESTINATION share/openhd-glide/testdata/artosyn
)

install(
    FILES
        docs/artosyn-rkmpp-debug.md
    DESTINATION share/doc/openhd-glide
)

if(UNIX AND NOT APPLE AND OPENHD_GLIDE_INSTALL_SYSTEMD)
    if(IS_ABSOLUTE "${OPENHD_GLIDE_SERVICE_LIBEXEC_DIR}")
        set(OPENHD_GLIDE_SERVICE_LIBEXEC_FULL_DIR "${OPENHD_GLIDE_SERVICE_LIBEXEC_DIR}")
    else()
        set(OPENHD_GLIDE_SERVICE_LIBEXEC_FULL_DIR "${CMAKE_INSTALL_PREFIX}/${OPENHD_GLIDE_SERVICE_LIBEXEC_DIR}")
    endif()
    set(OPENHD_GLIDE_SERVICE_RUN_FULL_PATH "${OPENHD_GLIDE_SERVICE_LIBEXEC_FULL_DIR}/openhd-glide-service-run")
    set(OPENHD_GLIDE_STREAM_EXAMPLE_FULL_PATH "${CMAKE_INSTALL_PREFIX}/share/openhd-glide/examples/autorun-local-video-to-glide.sh")
    configure_file(
        packaging/systemd/openhd-glide.service.in
        "${CMAKE_CURRENT_BINARY_DIR}/openhd-glide.service"
        @ONLY
    )
    configure_file(
        packaging/systemd/openhd-glide-stream.service.in
        "${CMAKE_CURRENT_BINARY_DIR}/openhd-glide-stream.service"
        @ONLY
    )

    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/openhd-glide.service"
            "${CMAKE_CURRENT_BINARY_DIR}/openhd-glide-stream.service"
        DESTINATION "${OPENHD_GLIDE_SYSTEMD_UNIT_DIR}"
    )

    install(
        PROGRAMS
            packaging/systemd/openhd-glide-service-run
        DESTINATION "${OPENHD_GLIDE_SERVICE_LIBEXEC_DIR}"
    )

    install(
        FILES
            packaging/systemd/openhd-glide.default
        DESTINATION /etc/default
        RENAME openhd-glide
    )
    install(
        FILES
            packaging/systemd/openhd-glide-stream.default
        DESTINATION /etc/default
        RENAME openhd-glide-stream
    )
endif()

set(CPACK_PACKAGE_NAME "openhd-glide${OPENHD_GLIDE_PACKAGE_SUFFIX}")
set(CPACK_PACKAGE_VENDOR "OpenHD")
set(CPACK_PACKAGE_CONTACT "OpenHD <contact@openhdfpv.org>")
set(CPACK_PACKAGE_VERSION "${OPENHD_GLIDE_PACKAGE_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "OpenHD Glide DRM/KMS rendering stack")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_GENERATOR "DEB")
if(OPENHD_GLIDE_PACKAGE_ARCHITECTURE)
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${OPENHD_GLIDE_PACKAGE_ARCHITECTURE}")
endif()
set(CPACK_DEBIAN_PACKAGE_SECTION "video")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_COMPRESSION_TYPE "xz")
if(OPENHD_GLIDE_CROSS_PACKAGE)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
else()
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
set(OPENHD_GLIDE_DEBIAN_DEPENDS "libc6, libstdc++6, libdrm2, libgbm1, libegl1, libgles2, libfreetype6, fonts-dejavu-core, zlib1g, libgstreamer1.0-0, libgstreamer-plugins-base1.0-0")
if(OPENHD_GLIDE_EXTRA_DEBIAN_DEPENDS)
    string(APPEND OPENHD_GLIDE_DEBIAN_DEPENDS ", ${OPENHD_GLIDE_EXTRA_DEBIAN_DEPENDS}")
endif()
set(CPACK_DEBIAN_PACKAGE_DEPENDS "${OPENHD_GLIDE_DEBIAN_DEPENDS}")
if(OPENHD_GLIDE_PACKAGE_SUFFIX STREQUAL "")
    set(OPENHD_GLIDE_LEGACY_ROCKCHIP_PACKAGES
        "openhd-glide-rock5a, openhd-glide-rock5b, openhd-glide-radxa-cm5, openhd-glide-radxa-zero3w")
    set(CPACK_DEBIAN_PACKAGE_CONFLICTS "${OPENHD_GLIDE_LEGACY_ROCKCHIP_PACKAGES}")
    set(CPACK_DEBIAN_PACKAGE_REPLACES "${OPENHD_GLIDE_LEGACY_ROCKCHIP_PACKAGES}")
    set(CPACK_DEBIAN_PACKAGE_PROVIDES "${OPENHD_GLIDE_LEGACY_ROCKCHIP_PACKAGES}")
endif()
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
if(UNIX AND NOT APPLE AND OPENHD_GLIDE_INSTALL_SYSTEMD)
    set(OPENHD_GLIDE_DEBIAN_CONTROL_DIR "${CMAKE_CURRENT_BINARY_DIR}/debian-control")
    file(MAKE_DIRECTORY "${OPENHD_GLIDE_DEBIAN_CONTROL_DIR}")

    set(OPENHD_GLIDE_DEBIAN_CONTROL_EXTRA)
    foreach(control_file postinst prerm postrm conffiles)
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/${control_file}"
            "${OPENHD_GLIDE_DEBIAN_CONTROL_DIR}/${control_file}"
            COPYONLY
        )
        list(APPEND OPENHD_GLIDE_DEBIAN_CONTROL_EXTRA "${OPENHD_GLIDE_DEBIAN_CONTROL_DIR}/${control_file}")
    endforeach()

    foreach(control_script postinst prerm postrm)
        file(
            CHMOD "${OPENHD_GLIDE_DEBIAN_CONTROL_DIR}/${control_script}"
            PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
        )
    endforeach()

    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA ${OPENHD_GLIDE_DEBIAN_CONTROL_EXTRA})
endif()
include(CPack)
