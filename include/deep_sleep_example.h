/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once
#include <stdbool.h>
#include "esp_sleep.h"
#ifdef __cplusplus
extern "C" {
#endif

// #include "sdkconfig.h"
#define RING_BUFFER_SIZE 4
typedef struct {
	float data[RING_BUFFER_SIZE];
	int head;
	int tail;
	int count;
	bool is_full;
}ring_buffer_t;


void example_deep_sleep_register_ext0_wakeup(void);
bool RTC_IRAM_ATTR ring_buffer_append(ring_buffer_t *buffer, float item);
bool RTC_IRAM_ATTR ring_buffer_flush(ring_buffer_t *buffer, float *flushed_data);
ring_buffer_t ring_buffer_init(ring_buffer_t *buffer);
#ifdef __cplusplus
}
#endif
