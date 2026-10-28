# Lossless OS simulator for the Mac (and Linux): the app, LVGL and SDL2.
#
#   cmake -S platform/host -B build/host -G Ninja && ninja -C build/host
#
# LVGL comes from the Zephyr workspace (zephyr-ws/modules/lib/gui/lvgl),
# so the simulator and the player build the same LVGL.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(lossless_os_sim C CXX)

# The git commit, for About (LOS_BUILD)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE LOS_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(LOS_GIT)
  add_compile_definitions(LOS_BUILD="${LOS_GIT}")
endif()

set(LOS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(LVGL_DIR ${LOS_ROOT}/../zephyr-ws/modules/lib/gui/lvgl CACHE PATH "LVGL source")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${LVGL_DIR}/src ${LOS_ROOT}/app ${CMAKE_CURRENT_LIST_DIR} ${SDL2_INCLUDE_DIRS})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE LOS_HOST)
target_compile_options(lvgl PRIVATE -w -O2)

# lbt, our Bluetooth stack, with its simulated controller and AirPods
set(LBT_DIR ${LOS_ROOT}/lib/lbt)
file(GLOB SBC_SOURCES ${LBT_DIR}/sbc/encoder/srce/*.c ${LBT_DIR}/sbc/decoder/srce/*.c)
add_library(lbt STATIC
    ${LBT_DIR}/hci.c ${LBT_DIR}/l2cap.c ${LBT_DIR}/gap.c ${LBT_DIR}/sdp.c ${LBT_DIR}/avdtp.c
    ${LBT_DIR}/a2dp.c ${LBT_DIR}/avrcp.c ${LBT_DIR}/coverart.c ${LBT_DIR}/source.c ${LBT_DIR}/test/sim.c
    ${SBC_SOURCES})
target_include_directories(lbt PUBLIC ${LBT_DIR}/sbc/encoder/include ${LBT_DIR}/sbc/decoder/include
    PRIVATE ${LBT_DIR}/sbc/decoder/srce)
set_source_files_properties(${SBC_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

# libopus, decoder only, fixed point as on the players
include(${LOS_ROOT}/lib/opus/sources.cmake)
add_library(opus STATIC ${OPUS_SOURCES})
include(${LOS_ROOT}/lib/pvmp3/sources.cmake)
add_library(pvmp3 STATIC ${PVMP3_SOURCES})
target_compile_options(pvmp3 PRIVATE -O2)
include(${LOS_ROOT}/lib/tremor/sources.cmake)
add_library(tremor STATIC ${TREMOR_SOURCES})
target_compile_options(tremor PRIVATE -O2)
target_compile_options(opus PRIVATE -O2)

add_executable(los-sim
    ${LOS_ROOT}/app/app.c
    ${LOS_ROOT}/app/theme.c
    ${LOS_ROOT}/app/widgets.c
    ${LOS_ROOT}/app/library.c
    ${LOS_ROOT}/app/covers.c
    ${LOS_ROOT}/app/jpegenc.c
    ${LOS_ROOT}/app/flac.c
    ${LOS_ROOT}/app/alac.c
    ${LOS_ROOT}/app/mp4.c
    ${LOS_ROOT}/app/ogg.c
    ${LOS_ROOT}/app/opusfile.c
    ${LOS_ROOT}/app/oggvorbis.c
    ${LOS_ROOT}/app/mp3file.c
    ${LOS_ROOT}/app/source.c
    ${LOS_ROOT}/app/playback.c
    ${LOS_ROOT}/app/output.c
    ${LOS_ROOT}/app/resample.c
    ${LOS_ROOT}/app/eq.c
    ${LOS_ROOT}/app/playlists.c
    ${LOS_ROOT}/app/bluetooth.c
    ${LOS_ROOT}/app/settings.c
    ${CMAKE_CURRENT_LIST_DIR}/bt.c
    ${CMAKE_CURRENT_LIST_DIR}/sys.c
    ${CMAKE_CURRENT_LIST_DIR}/main.c)
target_compile_options(los-sim PRIVATE -Wall -Wextra -g)
target_include_directories(los-sim PRIVATE ${LOS_ROOT}/lib/opus/include)
set_source_files_properties(${LOS_ROOT}/app/oggvorbis.c PROPERTIES INCLUDE_DIRECTORIES "${TREMOR_INCLUDES}")
set_source_files_properties(${LOS_ROOT}/app/mp3file.c PROPERTIES INCLUDE_DIRECTORIES "${PVMP3_INCLUDES}")
target_link_libraries(los-sim lvgl lbt opus tremor pvmp3 ${SDL2_LINK_LIBRARIES} m)
target_link_directories(los-sim PRIVATE ${SDL2_LIBRARY_DIRS})
