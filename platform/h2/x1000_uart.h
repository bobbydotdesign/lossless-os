/*
 * X1000 UART, polled, 8N1 with RTS/CTS (x1000_uart.c).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef X1000_UART_H
#define X1000_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 115200 or 3000000 baud; false for any other */
bool x1000_uart_init(int n, unsigned baud);
bool x1000_uart_set_baud(int n, unsigned baud);
void x1000_uart_off(int n);

/* Send all of it; false if the other side held CTS for timeout_ms */
bool x1000_uart_write(int n, const uint8_t *data, size_t len, int timeout_ms);
/* Whatever has arrived, up to max bytes; never waits */
size_t x1000_uart_read(int n, uint8_t *buf, size_t max);
void x1000_uart_flush_rx(int n);
uint32_t x1000_uart_overruns(void);

#endif
