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
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <uart.h>
#include "log.h"

#define INITIAL_MAGIC "@@@@"

// Power of two so offsets stay consistent with the buffer position when they wrap
#define LOG_BUFFER_SIZE 16384

// History of the newest log lines, kept until overwritten so every web client can read all of it.
// Offsets count bytes written since boot, the byte at offset p is at log_buffer[p % LOG_BUFFER_SIZE].
static char log_buffer[LOG_BUFFER_SIZE];
static uint32_t log_head;
static uint32_t log_boot_id;
static portMUX_TYPE log_lock = portMUX_INITIALIZER_UNLOCKED;

// Must be called with log_lock held
static void log_write(const char *data, size_t length) {
    while (length > 0) {
        size_t pos = log_head % LOG_BUFFER_SIZE;
        size_t n = MIN(length, LOG_BUFFER_SIZE - pos);
        memcpy(log_buffer + pos, data, n);
        log_head += n;
        data += n;
        length -= n;
    }
}

static void log_write_line(const char *line, size_t length) {
    taskENTER_CRITICAL(&log_lock);
    log_write(line, length);
    log_write("\n", 1);
    taskEXIT_CRITICAL(&log_lock);
}

esp_err_t log_init() {
    // Lets web log clients tell a restart apart from the log they already have
    log_boot_id = esp_random();

    // Magic string to let web log know that ESP32 has restart (to reset line counter)
    log_write_line(INITIAL_MAGIC, strlen(INITIAL_MAGIC));

    return ESP_OK;
}

int log_vprintf(const char * format, va_list arg) {
    char buffer[512];
    int n = vsnprintf(buffer, 512, format, arg);
    if (n <= 0) return n;

    if (n > 512) {
        n = 512;
    }

    // Remove log colors and trailing newline for web log buffer
    int strip = strlen(LOG_COLOR_E) + strlen(LOG_RESET_COLOR) + 1;
    if (n > strip) {
        log_write_line(buffer + strlen(LOG_COLOR_E), n - strip);
    }

    uart_log(buffer, n);

    return n;
}

size_t log_read(uint32_t since, char *out, size_t max, uint32_t *start, uint32_t *end, uint32_t *head, uint32_t *boot_id) {
    taskENTER_CRITICAL(&log_lock);

    uint32_t h = log_head;
    uint32_t oldest = h > LOG_BUFFER_SIZE ? h - LOG_BUFFER_SIZE : 0;

    // Offset from before a restart
    if (since > h) since = 0;

    uint32_t s = since;
    if (s < oldest) {
        // Older lines have been overwritten, skip the partial line left at the start
        s = oldest;
        while (s < h && log_buffer[s++ % LOG_BUFFER_SIZE] != '\n');
    }

    uint32_t e = MIN(h, s + max);
    if (e < h) {
        // Only send whole lines, the rest is sent on the next read
        uint32_t cut = e;
        while (cut > s && log_buffer[(cut - 1) % LOG_BUFFER_SIZE] != '\n') cut--;
        if (cut > s) e = cut;
    }

    size_t length = e - s;
    size_t pos = s % LOG_BUFFER_SIZE;
    size_t first = MIN(length, LOG_BUFFER_SIZE - pos);
    memcpy(out, log_buffer + pos, first);
    memcpy(out + first, log_buffer, length - first);

    taskEXIT_CRITICAL(&log_lock);

    *start = s;
    *end = e;
    *head = h;
    *boot_id = log_boot_id;

    return length;
}
