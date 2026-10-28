# Test 3 on the H2: buttons, the wheel and switching off (docs/h2-hardware.md).
#
#   west build -b hifiwalker_h2 tests/h2-buttons -d build/h2-buttons
#
# Preview the screen on the Mac: tests/h2-buttons/host/preview.sh
#
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(SOC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(h2_buttons C)

target_sources(app PRIVATE src/main.c src/page.c
    ../../platform/h2/draw.c ../../platform/h2/screen.c ../../platform/h2/ui.c
    ../../platform/h2/x1000_i2c.c ../../platform/h2/h2_buttons.c ../../platform/h2/h2_power.c)
target_include_directories(app PRIVATE src ../../platform/h2 ../../app)
