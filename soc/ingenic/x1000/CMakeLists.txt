# SPDX-License-Identifier: Apache-2.0

zephyr_compile_options(-march=mips32r2 -mno-mips16 ${TOOLCHAIN_C_FLAGS})
zephyr_ld_options(-march=mips32r2 -mno-mips16 ${TOOLCHAIN_LD_FLAGS})
zephyr_sources(start.S soc.c)
zephyr_sources_ifdef(CONFIG_SYS_CLOCK_EXISTS ost_timer.c)

# Lossless OS drivers for this chip (os/drivers)
set(LOS_DRIVERS ${CMAKE_CURRENT_LIST_DIR}/../../../drivers)
zephyr_sources_ifdef(CONFIG_SDHC_X1000 ${LOS_DRIVERS}/sdhc/x1000_msc.c)
zephyr_sources_ifdef(CONFIG_DISPLAY_HANDOVER_FB ${LOS_DRIVERS}/display/handover_fb.c)
zephyr_include_directories(.)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/mips/linker.ld CACHE INTERNAL "")
