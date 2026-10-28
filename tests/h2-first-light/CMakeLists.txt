# Test 1 on the H2: our code runs (docs/h2-hardware.md).
#
#   west build -b hifiwalker_h2 tests/h2-first-light -d build/h2-first-light
#
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(SOC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(DTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(h2_first_light C)

target_sources(app PRIVATE src/main.c)
