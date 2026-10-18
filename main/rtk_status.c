/*
 * This file is part of the ESP32-XBee distribution (https://github.com/nebkat/esp32-xbee).
 * Copyright (c) 2026 Reuben Rissler.
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

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "rtk_status.h"
#include "geodesy.h"
#include "tasks.h"
#include "uart.h"

static const char *TAG = "RTK_STATUS";

#define NMEA_LINE_MAX_LENGTH 128
#define NMEA_LINE_QUEUE_LENGTH 4

#define REPORT_INTERVAL_MS 5000
#define REPORT_MAX_AGE 10.0

typedef struct rtk_state {
    bool base_valid;
    rtcm_station_t base;
    int64_t base_time;

    bool gga_valid;
    nmea_gga_t gga;
    int64_t gga_time;

    bool gst_valid;
    nmea_gst_t gst;
    int64_t gst_time;
} rtk_state_t;

static bool initialized = false;

static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static rtk_state_t state;

// Only accessed from NTRIP client task
static rtcm_parser_t rtcm_parser;

// Only accessed from UART read event handler, parsing is deferred to rtk_status_task as event loop stack is small
static char nmea_line[NMEA_LINE_MAX_LENGTH];
static size_t nmea_line_length = 0;
static QueueHandle_t nmea_line_queue;

void rtk_status_rtcm_feed(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (!rtcm_parser_push(&rtcm_parser, data[i])) continue;

        rtcm_station_t station;
        if (!rtcm_decode_station(&rtcm_parser, &station)) continue;

        int64_t time = esp_timer_get_time();

        taskENTER_CRITICAL(&state_lock);
        state.base = station;
        state.base_valid = true;
        state.base_time = time;
        taskEXIT_CRITICAL(&state_lock);
    }
}

void rtk_status_base_reset() {
    rtcm_parser_reset(&rtcm_parser);

    taskENTER_CRITICAL(&state_lock);
    state.base_valid = false;
    taskEXIT_CRITICAL(&state_lock);
}

static void rtk_status_nmea_line_complete() {
    nmea_line[nmea_line_length] = '\0';

    if (nmea_line_length < 7) return;
    if (strncmp(nmea_line + 3, "GGA,", 4) != 0 && strncmp(nmea_line + 3, "GST,", 4) != 0) return;

    xQueueSend(nmea_line_queue, nmea_line, 0);
}

static void rtk_status_nmea_feed(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        char c = data[i];

        if (c == '$') {
            nmea_line_length = 0;
        } else if (nmea_line_length == 0) {
            continue;
        } else if (c == '\r' || c == '\n') {
            rtk_status_nmea_line_complete();
            nmea_line_length = 0;
            continue;
        } else if (nmea_line_length >= NMEA_LINE_MAX_LENGTH - 1) {
            nmea_line_length = 0;
            continue;
        }

        nmea_line[nmea_line_length++] = c;
    }
}

static void rtk_status_uart_handler(void* handler_args, esp_event_base_t base, int32_t length, void* buffer) {
    rtk_status_nmea_feed(buffer, length);
}

static void rtk_status_nmea_parse(const char *line) {
    nmea_gga_t gga;
    nmea_gst_t gst;

    int64_t time = esp_timer_get_time();

    if (nmea_parse_gga(line, &gga)) {
        taskENTER_CRITICAL(&state_lock);
        state.gga = gga;
        state.gga_valid = true;
        state.gga_time = time;
        taskEXIT_CRITICAL(&state_lock);
    } else if (nmea_parse_gst(line, &gst)) {
        taskENTER_CRITICAL(&state_lock);
        state.gst = gst;
        state.gst_valid = true;
        state.gst_time = time;
        taskEXIT_CRITICAL(&state_lock);
    }
}

bool rtk_status_get(rtk_status_t *status) {
    if (!initialized) return false;

    rtk_state_t snapshot;
    taskENTER_CRITICAL(&state_lock);
    snapshot = state;
    taskEXIT_CRITICAL(&state_lock);

    int64_t now = esp_timer_get_time();

    *status = (rtk_status_t) {
            .base_valid = snapshot.base_valid,
            .base = snapshot.base,
            .base_age = (now - snapshot.base_time) / 1e6,
            .gga_valid = snapshot.gga_valid,
            .gga = snapshot.gga,
            .gga_age = (now - snapshot.gga_time) / 1e6,
            .gst_valid = snapshot.gst_valid,
            .gst = snapshot.gst,
            .gst_age = (now - snapshot.gst_time) / 1e6
    };

    const nmea_gga_t *gga = &snapshot.gga;
    if (!snapshot.base_valid || !snapshot.gga_valid || gga->fix_quality == 0 || isnan(gga->latitude) ||
            isnan(gga->longitude) || isnan(gga->altitude) || isnan(gga->geoid_separation)) return true;

    // GGA altitude is above mean sea level, ECEF needs height above ellipsoid
    double rover[3], enu[3];
    geodesy_geodetic_to_ecef(gga->latitude, gga->longitude, gga->altitude + gga->geoid_separation, rover);
    geodesy_ecef_to_enu(snapshot.base.ecef, rover, enu);

    status->distance_valid = true;
    status->distance_horizontal = sqrt(enu[0] * enu[0] + enu[1] * enu[1]);
    status->distance_vertical = enu[2];
    status->distance_3d = sqrt(enu[0] * enu[0] + enu[1] * enu[1] + enu[2] * enu[2]);

    return true;
}

static void format_optional(char *buffer, size_t size, double value, int decimals) {
    if (isnan(value)) {
        buffer[0] = '\0';
    } else {
        snprintf(buffer, size, "%.*f", decimals, value);
    }
}

static bool rtcm_station_equal(const rtcm_station_t *a, const rtcm_station_t *b) {
    return a->message_type == b->message_type && a->station_id == b->station_id &&
            a->ecef[0] == b->ecef[0] && a->ecef[1] == b->ecef[1] && a->ecef[2] == b->ecef[2] &&
            a->antenna_height == b->antenna_height;
}

static void rtk_status_report(rtcm_station_t *logged_base, bool *logged_base_valid) {
    rtk_status_t status;
    rtk_status_get(&status);

    if (status.base_valid && (!*logged_base_valid || !rtcm_station_equal(logged_base, &status.base))) {
        double latitude, longitude, height;
        geodesy_ecef_to_geodetic(status.base.ecef, &latitude, &longitude, &height);
        ESP_LOGI(TAG, "Base station %d (RTCM %d): ECEF %.4f, %.4f, %.4f / %.8f, %.8f, %.4f m",
                status.base.station_id, status.base.message_type,
                status.base.ecef[0], status.base.ecef[1], status.base.ecef[2],
                latitude, longitude, height);

        *logged_base = status.base;
        *logged_base_valid = true;
    }

    if (!status.distance_valid || status.gga_age > REPORT_MAX_AGE) return;

    bool gst = status.gst_valid && status.gst_age <= REPORT_MAX_AGE;

    char hdop[24], correction_age[24], sigma_latitude[24], sigma_longitude[24], sigma_altitude[24];
    format_optional(hdop, sizeof(hdop), status.gga.hdop, 2);
    format_optional(correction_age, sizeof(correction_age), status.gga.correction_age, 1);
    format_optional(sigma_latitude, sizeof(sigma_latitude), gst ? status.gst.sigma_latitude : NAN, 3);
    format_optional(sigma_longitude, sizeof(sigma_longitude), gst ? status.gst.sigma_longitude : NAN, 3);
    format_optional(sigma_altitude, sizeof(sigma_altitude), gst ? status.gst.sigma_altitude : NAN, 3);

    // Station, distance 3D/horizontal/vertical, fix quality, satellites, HDOP, correction age, sigma lat/lon/alt
    uart_nmea("$PESP,NTRIP,CLI,BASE,%d,%.3f,%.3f,%.3f,%d,%d,%s,%s,%s,%s,%s",
            status.base.station_id, status.distance_3d, status.distance_horizontal, status.distance_vertical,
            status.gga.fix_quality, status.gga.satellites, hdop, correction_age,
            sigma_latitude, sigma_longitude, sigma_altitude);
}

static void rtk_status_task(void *ctx) {
    char line[NMEA_LINE_MAX_LENGTH];

    rtcm_station_t logged_base;
    bool logged_base_valid = false;

    const TickType_t report_interval = pdMS_TO_TICKS(REPORT_INTERVAL_MS);
    TickType_t last_report = xTaskGetTickCount();

    while (true) {
        TickType_t elapsed = xTaskGetTickCount() - last_report;
        if (xQueueReceive(nmea_line_queue, line, elapsed < report_interval ? report_interval - elapsed : 0) == pdTRUE) {
            rtk_status_nmea_parse(line);
        }

        if (xTaskGetTickCount() - last_report < report_interval) continue;
        last_report = xTaskGetTickCount();

        rtk_status_report(&logged_base, &logged_base_valid);
    }
}

void rtk_status_init() {
    nmea_line_queue = xQueueCreate(NMEA_LINE_QUEUE_LENGTH, NMEA_LINE_MAX_LENGTH);
    rtcm_parser_reset(&rtcm_parser);

    initialized = true;

    xTaskCreate(rtk_status_task, "rtk_status_task", 6144, NULL, TASK_PRIORITY_INTERFACE, NULL);

    uart_register_read_handler(rtk_status_uart_handler);
}
