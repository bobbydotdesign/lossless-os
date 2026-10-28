# libopus 1.5.2, decoder only, fixed point (the players have no FPU).
# The files from upstream, unchanged; COPYING is its licence (BSD).
# SPDX-License-Identifier: BSD-3-Clause
set(OPUS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(OPUS_SOURCES
    ${OPUS_DIR}/celt/bands.c
    ${OPUS_DIR}/celt/celt.c
    ${OPUS_DIR}/celt/celt_decoder.c
    ${OPUS_DIR}/celt/cwrs.c
    ${OPUS_DIR}/celt/entcode.c
    ${OPUS_DIR}/celt/entdec.c
    ${OPUS_DIR}/celt/entenc.c
    ${OPUS_DIR}/celt/kiss_fft.c
    ${OPUS_DIR}/celt/laplace.c
    ${OPUS_DIR}/celt/mathops.c
    ${OPUS_DIR}/celt/mdct.c
    ${OPUS_DIR}/celt/modes.c
    ${OPUS_DIR}/celt/pitch.c
    ${OPUS_DIR}/celt/celt_lpc.c
    ${OPUS_DIR}/celt/quant_bands.c
    ${OPUS_DIR}/celt/rate.c
    ${OPUS_DIR}/celt/vq.c
    ${OPUS_DIR}/silk/CNG.c
    ${OPUS_DIR}/silk/code_signs.c
    ${OPUS_DIR}/silk/init_decoder.c
    ${OPUS_DIR}/silk/decode_core.c
    ${OPUS_DIR}/silk/decode_frame.c
    ${OPUS_DIR}/silk/decode_parameters.c
    ${OPUS_DIR}/silk/decode_indices.c
    ${OPUS_DIR}/silk/decode_pulses.c
    ${OPUS_DIR}/silk/decoder_set_fs.c
    ${OPUS_DIR}/silk/dec_API.c
    ${OPUS_DIR}/silk/gain_quant.c
    ${OPUS_DIR}/silk/interpolate.c
    ${OPUS_DIR}/silk/LP_variable_cutoff.c
    ${OPUS_DIR}/silk/NLSF_decode.c
    ${OPUS_DIR}/silk/PLC.c
    ${OPUS_DIR}/silk/shell_coder.c
    ${OPUS_DIR}/silk/tables_gain.c
    ${OPUS_DIR}/silk/tables_LTP.c
    ${OPUS_DIR}/silk/tables_NLSF_CB_NB_MB.c
    ${OPUS_DIR}/silk/tables_NLSF_CB_WB.c
    ${OPUS_DIR}/silk/tables_other.c
    ${OPUS_DIR}/silk/tables_pitch_lag.c
    ${OPUS_DIR}/silk/tables_pulses_per_block.c
    ${OPUS_DIR}/silk/NLSF_unpack.c
    ${OPUS_DIR}/silk/stereo_MS_to_LR.c
    ${OPUS_DIR}/silk/bwexpander_32.c
    ${OPUS_DIR}/silk/bwexpander.c
    ${OPUS_DIR}/silk/decode_pitch.c
    ${OPUS_DIR}/silk/inner_prod_aligned.c
    ${OPUS_DIR}/silk/lin2log.c
    ${OPUS_DIR}/silk/log2lin.c
    ${OPUS_DIR}/silk/LPC_analysis_filter.c
    ${OPUS_DIR}/silk/LPC_inv_pred_gain.c
    ${OPUS_DIR}/silk/table_LSF_cos.c
    ${OPUS_DIR}/silk/NLSF2A.c
    ${OPUS_DIR}/silk/NLSF_stabilize.c
    ${OPUS_DIR}/silk/resampler.c
    ${OPUS_DIR}/silk/resampler_down2_3.c
    ${OPUS_DIR}/silk/resampler_down2.c
    ${OPUS_DIR}/silk/resampler_private_AR2.c
    ${OPUS_DIR}/silk/resampler_private_down_FIR.c
    ${OPUS_DIR}/silk/resampler_private_IIR_FIR.c
    ${OPUS_DIR}/silk/resampler_private_up2_HQ.c
    ${OPUS_DIR}/silk/resampler_rom.c
    ${OPUS_DIR}/silk/sum_sqr_shift.c
    ${OPUS_DIR}/silk/pitch_est_tables.c
    ${OPUS_DIR}/silk/LPC_fit.c
    ${OPUS_DIR}/silk/biquad_alt.c
    ${OPUS_DIR}/silk/sort.c
    ${OPUS_DIR}/silk/stereo_decode_pred.c
    ${OPUS_DIR}/src/opus.c
    ${OPUS_DIR}/src/opus_decoder.c
    ${OPUS_DIR}/src/extensions.c
    ${OPUS_DIR}/src/repacketizer.c
)
set(OPUS_INCLUDES ${OPUS_DIR}/include ${OPUS_DIR}/celt ${OPUS_DIR}/silk ${OPUS_DIR}/silk/fixed)
# Only these files see libopus's settings and headers
set_source_files_properties(${OPUS_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "FIXED_POINT;OPUS_BUILD;VAR_ARRAYS;DISABLE_FLOAT_API"
    INCLUDE_DIRECTORIES "${OPUS_INCLUDES}"
    COMPILE_OPTIONS "-w")
