/*
 * This file is part of the ESP32-XBee distribution (https://github.com/nebkat/esp32-xbee).
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <esp_log.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <string.h>
#include <uart.h>
#include "log.h"

#define INITIAL_MAGIC "@@@@\n"

static const char *TAG = "LOG";

static RingbufHandle_t ringbuf_handle;

esp_err_t log_init() {
    ringbuf_handle = xRingbufferCreate(4096, RINGBUF_TYPE_BYTEBUF);
    if (ringbuf_handle == NULL) {
        ESP_LOGE(TAG, "Could not create log ring buffer");
        return ESP_FAIL;
    }

    // Magic string to let web log know that ESP32 has restart (to reset line counter)
    xRingbufferSend(ringbuf_handle, INITIAL_MAGIC, strlen(INITIAL_MAGIC), 0);

    return ESP_OK;
}

int log_vprintf(const char *format, va_list arg) {
    char buffer[512];
    int n = vsnprintf(buffer, sizeof(buffer), format, arg);
    if (n <= 0) return n;
    if (n >= (int)sizeof(buffer)) n = (int)sizeof(buffer) - 1;

    /* Strip ANSI escape codes before sending to the web log ring buffer.
     * All ESP-IDF color prefixes follow the pattern "\033[...m" — scan past
     * the first 'm' rather than assuming a fixed length (avoids underflow when
     * colours are disabled or the message is very short). */
    const char *start = buffer;
    int len = n;

    if (len > 2 && start[0] == '\033' && start[1] == '[') {
        const char *m = memchr(start + 2, 'm', (size_t)(len - 2));
        if (m != NULL) {
            int skip = (int)(m - start) + 1;
            start += skip;
            len   -= skip;
        }
    }

    /* Strip trailing LOG_RESET_COLOR ("\033[0m") and newline if present. */
    static const int reset_len = sizeof(LOG_RESET_COLOR) - 1;
    if (len > reset_len &&
        memcmp(start + len - reset_len, LOG_RESET_COLOR, (size_t)reset_len) == 0) {
        len -= reset_len;
    }
    if (len > 0 && start[len - 1] == '\n') len--;

    if (len > 0) xRingbufferSend(ringbuf_handle, start, (size_t)len, 0);
    xRingbufferSend(ringbuf_handle, "\n", 1, 0);

    uart_log(buffer, n);
    return n;
}

void *log_receive(size_t *length, TickType_t ticksToWait) {
    return xRingbufferReceive(ringbuf_handle, length, ticksToWait);
}

void log_return(void *item) {
    vRingbufferReturnItem(ringbuf_handle, item);
}
