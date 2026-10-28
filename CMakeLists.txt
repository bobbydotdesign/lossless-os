# Lossless OS on Zephyr.
#
#   west build -b qemu_malta . -d build/qemu          (MIPS, like the H2's X1000)
#   west build -b hifiwalker_h2 . -d build/h2         (the H2, from its boot menu)
#
# The Mac simulator has its own build: platform/host/CMakeLists.txt.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(SOC_ROOT ${CMAKE_CURRENT_LIST_DIR})
set(DTS_ROOT ${CMAKE_CURRENT_LIST_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lossless_os C CXX)

# The git commit, for About (LOS_BUILD)
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE LOS_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(LOS_GIT)
  add_compile_definitions(LOS_BUILD="${LOS_GIT}")
endif()

target_sources(app PRIVATE
    app/app.c
    app/theme.c
    app/widgets.c
    app/library.c
    app/covers.c
    app/jpegenc.c
    app/flac.c
    app/alac.c
    app/mp4.c
    app/ogg.c
    app/opusfile.c
    app/oggvorbis.c
    app/mp3file.c
    app/source.c
    app/playback.c
    app/output.c
    app/resample.c
    app/eq.c
    app/playlists.c
    app/bluetooth.c
    app/settings.c
    platform/zephyr/main.c
    platform/zephyr/sys.c)
target_include_directories(app PRIVATE app platform/zephyr lib/opus/include)

# libopus, decoder only, fixed point (lib/opus/sources.cmake, BSD licence)
include(${CMAKE_CURRENT_LIST_DIR}/lib/opus/sources.cmake)
target_sources(app PRIVATE ${OPUS_SOURCES})
# Tremor (Vorbis, fixed point) and libogg's bit reader (lib/tremor/sources.cmake, BSD)
include(${CMAKE_CURRENT_LIST_DIR}/lib/tremor/sources.cmake)
target_sources(app PRIVATE ${TREMOR_SOURCES})
# picolibc doesn't say the byte order the way Tremor asks; the X1000 is little-endian
set_property(SOURCE ${TREMOR_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS
    "BYTE_ORDER=1234;LITTLE_ENDIAN=1234;BIG_ENDIAN=4321")
set_source_files_properties(app/oggvorbis.c PROPERTIES INCLUDE_DIRECTORIES "${TREMOR_INCLUDES}")
# Android's MP3 decoder (lib/pvmp3/sources.cmake, Apache-2.0), compiled as C++
include(${CMAKE_CURRENT_LIST_DIR}/lib/pvmp3/sources.cmake)
target_sources(app PRIVATE ${PVMP3_SOURCES})
set_source_files_properties(app/mp3file.c PROPERTIES INCLUDE_DIRECTORIES "${PVMP3_INCLUDES}")

# lbt, our Bluetooth stack, and the SBC encoder it uses (Android's, Apache-2.0)
set(LBT_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/lbt)
file(GLOB SBC_SOURCES ${LBT_DIR}/sbc/encoder/srce/*.c)
target_sources(app PRIVATE
    ${LBT_DIR}/hci.c ${LBT_DIR}/l2cap.c ${LBT_DIR}/gap.c ${LBT_DIR}/sdp.c ${LBT_DIR}/avdtp.c
    ${LBT_DIR}/a2dp.c ${LBT_DIR}/avrcp.c ${LBT_DIR}/coverart.c ${LBT_DIR}/source.c ${SBC_SOURCES})
target_include_directories(app PRIVATE ${LBT_DIR}/sbc/encoder/include)
set_source_files_properties(${SBC_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
target_sources_ifdef(CONFIG_FILE_SYSTEM app PRIVATE platform/zephyr/lv_drive_a.c)

if(BOARD STREQUAL "hifiwalker_h2")
  target_sources(app PRIVATE
      platform/h2/h2_board.c
      platform/h2/draw.c
      platform/h2/screen.c
      platform/h2/ui.c
      platform/h2/x1000_i2c.c
      platform/h2/h2_buttons.c
      platform/h2/h2_power.c
      platform/h2/h2_usb.c
      platform/h2/usb_msc.c
      platform/h2/usb_dac.c
      platform/h2/h2_audio.c
      platform/h2/x1000_uart.c
      platform/h2/h2_bt.c
      platform/h2/h2_clock.c
      platform/h2/h2_backlight.c
      platform/h2/h2_lcd.c)
  target_include_directories(app PRIVATE platform/h2 drivers/sdhc)
endif()
