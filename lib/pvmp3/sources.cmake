# Android's fixed-point MP3 decoder (PacketVideo), from AOSP
# frameworks/av media/module/codecs/mp3dec, the files unchanged but for
# build_config.h (ours). C written as C++: it's compiled as C++, and
# called from C (its API is extern "C"). Apache License 2.0 (NOTICE).
# SPDX-License-Identifier: Apache-2.0
set(PVMP3_DIR ${CMAKE_CURRENT_LIST_DIR})
set(PVMP3_SOURCES
    ${PVMP3_DIR}/src/pvmp3_normalize.cpp
    ${PVMP3_DIR}/src/pvmp3_alias_reduction.cpp
    ${PVMP3_DIR}/src/pvmp3_crc.cpp
    ${PVMP3_DIR}/src/pvmp3_decode_header.cpp
    ${PVMP3_DIR}/src/pvmp3_decode_huff_cw.cpp
    ${PVMP3_DIR}/src/pvmp3_getbits.cpp
    ${PVMP3_DIR}/src/pvmp3_dequantize_sample.cpp
    ${PVMP3_DIR}/src/pvmp3_framedecoder.cpp
    ${PVMP3_DIR}/src/pvmp3_get_main_data_size.cpp
    ${PVMP3_DIR}/src/pvmp3_get_side_info.cpp
    ${PVMP3_DIR}/src/pvmp3_get_scale_factors.cpp
    ${PVMP3_DIR}/src/pvmp3_mpeg2_get_scale_data.cpp
    ${PVMP3_DIR}/src/pvmp3_mpeg2_get_scale_factors.cpp
    ${PVMP3_DIR}/src/pvmp3_mpeg2_stereo_proc.cpp
    ${PVMP3_DIR}/src/pvmp3_huffman_decoding.cpp
    ${PVMP3_DIR}/src/pvmp3_huffman_parsing.cpp
    ${PVMP3_DIR}/src/pvmp3_tables.cpp
    ${PVMP3_DIR}/src/pvmp3_imdct_synth.cpp
    ${PVMP3_DIR}/src/pvmp3_mdct_6.cpp
    ${PVMP3_DIR}/src/pvmp3_dct_6.cpp
    ${PVMP3_DIR}/src/pvmp3_poly_phase_synthesis.cpp
    ${PVMP3_DIR}/src/pvmp3_equalizer.cpp
    ${PVMP3_DIR}/src/pvmp3_seek_synch.cpp
    ${PVMP3_DIR}/src/pvmp3_stereo_proc.cpp
    ${PVMP3_DIR}/src/pvmp3_reorder.cpp
    ${PVMP3_DIR}/src/pvmp3_polyphase_filter_window.cpp
    ${PVMP3_DIR}/src/pvmp3_mdct_18.cpp
    ${PVMP3_DIR}/src/pvmp3_dct_9.cpp
    ${PVMP3_DIR}/src/pvmp3_dct_16.cpp
)
set(PVMP3_INCLUDES ${PVMP3_DIR}/include ${PVMP3_DIR}/src)
set_source_files_properties(${PVMP3_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${PVMP3_INCLUDES}"
    COMPILE_OPTIONS "-include;${PVMP3_DIR}/build_config.h;-w;-fno-exceptions;-fno-rtti")
