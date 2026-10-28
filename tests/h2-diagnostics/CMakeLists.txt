# Test 2 on the H2: the screen becomes our log (docs/h2-hardware.md).
#
#   west build -b hifiwalker_h2 tests/h2-diagnostics -d build/h2-diagnostics
#
# Preview the page on the Mac: tests/h2-diagnostics/host/preview.sh
#
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(SOC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(h2_diagnostics C)

target_sources(app PRIVATE src/main.c src/page.c
    ../../platform/h2/draw.c ../../platform/h2/screen.c ../../platform/h2/ui.c)
target_include_directories(app PRIVATE ../../platform/h2 ../../app)
