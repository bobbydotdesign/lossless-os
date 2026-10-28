/*
 * The H2's sound (h2_audio.c): los_audio_* from los_sys.h, plus volume.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef H2_AUDIO_H
#define H2_AUDIO_H

/* DAC attenuation limits, in 0.5 dB steps: -2 dB loudest (the existing
 * firmware's on this player), -74 dB quietest */
#define H2_AUDIO_ATT_MAX 4
#define H2_AUDIO_ATT_MIN 148

#include <stdbool.h>
#include <stdint.h>

/* What the hardware says, for the check screen */
struct h2_audio_state {
	int dac_reg0, dac_vol, dac_reg10;   /* -1: no answer on I2C */
	uint32_t aic_cfg, aic_ccr, aic_i2scr, aic_sr, i2scdr, i2sdiv;
	uint32_t dma_ctrl, dma_cs, dma_sa, dma_da;
	uint32_t m, n, sclk;
	uint32_t pins_b;                    /* port B levels */
	bool powered, running;
};
void h2_audio_state_get(struct h2_audio_state *s);

/* Louder (+) or quieter (-) by db decibels */
void h2_audio_volume_step(int db);
int h2_audio_volume_db(void);

/* Bluetooth takes PC26 (the DAC's I2C data line) for its sleep clock:
 * true while it has it; false gives it back and sets the volume again */
void h2_audio_lend_bus(bool lent);

/* Times the DMA ran past what was written (counted, for bringing up the
 * USB DAC) */
extern uint32_t h2_audio_underruns;

#endif
