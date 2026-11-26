# SBC encoder and decoder

Android's SBC codec (Broadcom encoder, Open Interface decoder), Apache-2.0,
copied unchanged from Zephyr's `libsbc` module
(https://github.com/zephyrproject-rtos/libsbc, commit 8e1beda02acb).

lbt uses the encoder (`a2dp.c`). The decoder is only for the host tests.
