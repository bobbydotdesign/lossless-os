# Tremor (Xiph's fixed-point Vorbis decoder, git 820fb32) and libogg 1.3.5's
# bit reader, both BSD licensed (COPYING in each). The files from upstream,
# unchanged, except lib/ogg/include/ogg/config_types.h (filled in by hand,
# as libogg's configure would). The Ogg pages are read by app/ogg.c.
# Memory: their malloc / calloc / realloc / free are app/oggvorbis.c's
# arena, for the one file playing.
# SPDX-License-Identifier: BSD-3-Clause
set(TREMOR_DIR ${CMAKE_CURRENT_LIST_DIR})
set(TREMOR_SOURCES
    ${TREMOR_DIR}/block.c ${TREMOR_DIR}/codebook.c ${TREMOR_DIR}/floor0.c
    ${TREMOR_DIR}/floor1.c ${TREMOR_DIR}/info.c ${TREMOR_DIR}/mapping0.c
    ${TREMOR_DIR}/mdct.c ${TREMOR_DIR}/registry.c ${TREMOR_DIR}/res012.c
    ${TREMOR_DIR}/sharedbook.c ${TREMOR_DIR}/synthesis.c ${TREMOR_DIR}/window.c
    ${TREMOR_DIR}/../ogg/src/bitwise.c)
set(TREMOR_INCLUDES ${TREMOR_DIR} ${TREMOR_DIR}/../ogg/include)
set_source_files_properties(${TREMOR_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "malloc=tremor_malloc;calloc=tremor_calloc;realloc=tremor_realloc;free=tremor_free;HAVE_ALLOCA_H"
    INCLUDE_DIRECTORIES "${TREMOR_INCLUDES}"
    COMPILE_OPTIONS "-w")
