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

#include <stdbool.h>
#include <esp_log.h>
#include <esp_event_base.h>
#include <sys/socket.h>
#include <wifi.h>
#include <tasks.h>
#include <status_led.h>
#include <retry.h>
#include <stream_stats.h>
#include <rtk_status.h>
#include <freertos/event_groups.h>
#include <esp_ota_ops.h>
#include <lwip/netdb.h>
#include <ping/ping_sock.h>
#include "interface/ntrip.h"
#include "config.h"
#include "util.h"
#include "uart.h"

static const char *TAG = "NTRIP_CLIENT";

#define BUFFER_SIZE 512

#define GPGGA_HEADER "$GPGGA"
#define GNGGA_HEADER "$GNGGA"
#define GGA_END "\r\n"

static const int CASTER_READY_BIT = BIT0;

static int sock = -1;

static TaskHandle_t nmea_gga_send_task = NULL;

static EventGroupHandle_t client_event_group;

static status_led_handle_t status_led = NULL;
static stream_stats_handle_t stream_stats = NULL;

static char nmea_gga_latest[128] = "";

static int caster_ping_ms = -1;

#define CASTER_LOOKUP_ATTEMPTS 3
#define CASTER_PING_COUNT 5

#define GGA_MAX_AGE 10 // Seconds, same as the web UI's stale GGA

static bool receiver_has_position() {
    rtk_status_t status;
    return rtk_status_get(&status) && status.gga_valid && status.gga_age < GGA_MAX_AGE && status.gga.fix_quality != 0;
}

typedef struct caster_ping_result {
    TaskHandle_t task;
    int ms;
} caster_ping_result_t;

static void caster_ping_success(esp_ping_handle_t ping, void *args) {
    caster_ping_result_t *result = args;

    uint32_t ms;
    esp_ping_get_profile(ping, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
    result->ms = ms;

    // One reply is enough
    esp_ping_stop(ping);
}

static void caster_ping_end(esp_ping_handle_t ping, void *args) {
    xTaskNotifyGive(((caster_ping_result_t *) args)->task);
}

// Pings the caster and shows the result as the Internet health in the WiFi panel. Lookups and pings are retried,
// as the odd one goes missing: rtn.dot.ny.gov has a 1 s DNS TTL so every lookup goes out to the network, and
// firewalls often rate limit ICMP
static wifi_internet_t caster_ping(const char *host) {
    struct addrinfo hints = {.ai_family = AF_INET};
    struct addrinfo *addr = NULL;
    for (int attempt = 1; getaddrinfo(host, NULL, &hints, &addr) != 0 || addr == NULL; attempt++) {
        if (attempt == CASTER_LOOKUP_ATTEMPTS) {
            wifi_internet_set(WIFI_INTERNET_NO_DNS, -1, host);
            return WIFI_INTERNET_NO_DNS;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Default 1 s apart and 1 s timeout
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.count = CASTER_PING_COUNT;
    ip_addr_set_ip4_u32(&config.target_addr, ((struct sockaddr_in *) addr->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(addr);

    caster_ping_result_t result = {.task = xTaskGetCurrentTaskHandle(), .ms = -1};
    esp_ping_callbacks_t callbacks = {
            .cb_args = &result,
            .on_ping_success = caster_ping_success,
            .on_ping_end = caster_ping_end
    };

    esp_ping_handle_t ping;
    if (esp_ping_new_session(&config, &callbacks, &ping) == ESP_OK) {
        esp_ping_start(ping);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        esp_ping_delete_session(ping);
    }

    caster_ping_ms = result.ms;

    if (result.ms < 0) {
        wifi_internet_set(WIFI_INTERNET_NO_REPLY, -1, host);
        return WIFI_INTERNET_NO_REPLY;
    }

    wifi_internet_set(WIFI_INTERNET_OK, result.ms, host);
    return WIFI_INTERNET_OK;
}

static void nmea_gga_extract(int32_t length, void *buffer) {
    void *start = memmem(buffer, length, GPGGA_HEADER, strlen(GPGGA_HEADER));
    if (start == NULL) start = memmem(buffer, length, GNGGA_HEADER, strlen(GNGGA_HEADER));
    if (start == NULL) return;
    void *zero = memmem(start, length - (start - buffer), "\0", 1);
    void *end = memmem(start, length - (start - buffer), GGA_END, strlen(GGA_END));

    if (end == NULL || (zero != NULL && zero < end)) return;

    unsigned int size = (end - start) + strlen(GGA_END);
    if (size > (sizeof(nmea_gga_latest) - 1)) return;

    memcpy(nmea_gga_latest, start, size);
    nmea_gga_latest[size] = '\0';
}

static void ntrip_client_nmea_gga_send_task(void *ctx) {
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (true) {
        int sent = send(sock, nmea_gga_latest, strlen(nmea_gga_latest), 0);
        if (sent < 0) {
            destroy_socket(&sock);
        } else {
            stream_stats_increment(stream_stats, 0, sent);
        }

        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

static void ntrip_client_uart_handler(void* handler_args, esp_event_base_t base, int32_t length, void* buffer) {
    // Also before connecting, so the caster gets a position as soon as it is connected
    nmea_gga_extract(length, buffer);

    // Caster connected and ready for data
    if ((xEventGroupGetBits(client_event_group) & CASTER_READY_BIT) == 0) return;

    /*int sent = send(sock, buffer, length, 0);
    if (sent < 0) {
        destroy_socket(&sock);
    } else {
        stream_stats_increment(stream_stats, 0, sent);
    }*/
}

static void ntrip_client_task(void *ctx) {
    client_event_group = xEventGroupCreate();
    uart_register_read_handler(ntrip_client_uart_handler);

    config_color_t status_led_color = config_get_color(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_COLOR));
    if (status_led_color.rgba != 0) status_led = status_led_add(status_led_color.rgba, STATUS_LED_FADE, 500, 2000, 0);
    if (status_led != NULL) status_led->active = false;

    stream_stats = stream_stats_new("ntrip_client");

    // At most a minute between attempts, so corrections come back soon after the caster is reachable again
    retry_delay_handle_t delay_handle = retry_init(true, 5, 2000, 60000);

    while (true) {
        retry_delay(delay_handle);

        wait_for_ip();

        char *buffer = NULL;

        char *host, *mountpoint, *username, *password;
        uint16_t port = config_get_u16(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_PORT));
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_HOST), (void **) &host);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_USERNAME), (void **) &username);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_PASSWORD), (void **) &password);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_MOUNTPOINT), (void **) &mountpoint);

        // Don't use up retries while the caster's name doesn't resolve. No ping reply alone doesn't stop the attempt,
        // some networks drop pings now and then while the caster's port works fine
        if (caster_ping(host) == WIFI_INTERNET_NO_DNS) {
            ESP_LOGW(TAG, "Cannot resolve %s, waiting for Internet", host);
            uart_nmea("$PESP,NTRIP,CLI,WAITING_INTERNET,%s", host);

            do {
                vTaskDelay(pdMS_TO_TICKS(10000));
                wait_for_ip();
            } while (caster_ping(host) == WIFI_INTERNET_NO_DNS);

            retry_reset(delay_handle);
        }

        // An NTRIP mountpoint may send nothing until it gets a position, and casters may stop answering clients that
        // keep reconnecting, so hold off until the receiver has one
        if (!receiver_has_position()) {
            ESP_LOGI(TAG, "Waiting for a GGA position from the receiver");
            uart_nmea("$PESP,NTRIP,CLI,WAITING_GGA");

            for (int seconds = 1; !receiver_has_position(); seconds++) {
                vTaskDelay(pdMS_TO_TICKS(1000));

                // Keep the Internet health in the WiFi panel current
                if (seconds % 60 == 0) caster_ping(host);
            }

            retry_reset(delay_handle);
        }

        ESP_LOGI(TAG, "Connecting to %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,CLI,CONNECTING,%s:%d,%s", host, port, mountpoint);
        sock = connect_socket(host, port, SOCK_STREAM);
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_RESOLVE, goto _error, "Could not resolve host");
        ERROR_ACTION(TAG, sock == CONNECT_SOCKET_ERROR_CONNECT, goto _error, "Could not connect to host");

        buffer = malloc(BUFFER_SIZE);

        char *authorization = http_auth_basic_header(username, password);
        snprintf(buffer, BUFFER_SIZE, "GET /%s HTTP/1.1" NEWLINE \
                "User-Agent: NTRIP %s/%s" NEWLINE \
                "Authorization: %s" NEWLINE
                NEWLINE
                , mountpoint, NTRIP_CLIENT_NAME, &esp_app_get_description()->version[1], authorization);
        free(authorization);

        int err = write(sock, buffer, strlen(buffer));
        ERROR_ACTION(TAG, err < 0, goto _error, "Could not send request to caster: %d %s", errno, strerror(errno));

        int len = read(sock, buffer, BUFFER_SIZE - 1);
        ERROR_ACTION(TAG, len <= 0, goto _error, "Could not receive response from caster: %d %s", errno, strerror(errno));
        buffer[len] = '\0';

        char *status = extract_http_header(buffer, "");
        ERROR_ACTION(TAG, status == NULL || !ntrip_response_ok(status), free(status); goto _error,
                "Could not connect to mountpoint: %s",
                status == NULL ? "HTTP response malformed" :
                        (ntrip_response_sourcetable_ok(status) ? "Mountpoint not found" : status))
        free(status);

        ESP_LOGI(TAG, "Successfully connected to %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,CLI,CONNECTED,%s:%d,%s", host, port, mountpoint);

        retry_reset(delay_handle);

        // The stream proves the link, even from a caster that doesn't answer pings
        wifi_internet_set(WIFI_INTERNET_OK, caster_ping_ms, host);

        rtk_status_base_reset();

        if (status_led != NULL) status_led->active = true;

        // Start sending GGA to caster
        xTaskCreate(ntrip_client_nmea_gga_send_task, "ntrip_client_gga", 2048, NULL, TASK_PRIORITY_INTERFACE, &nmea_gga_send_task);

        // Connected
        xEventGroupSetBits(client_event_group, CASTER_READY_BIT);

        // Read from socket until disconnected (0 = closed by caster, -1 = error/timeout)
        while (sock != -1 && (len = read(sock, buffer, BUFFER_SIZE)) > 0) {
            uart_write(buffer, len);

            rtk_status_rtcm_feed((uint8_t *) buffer, len);

            stream_stats_increment(stream_stats, len, 0);
        }

        // Disconnected
        xEventGroupClearBits(client_event_group, CASTER_READY_BIT);

        // Stop sending GGA to caster
        vTaskDelete(nmea_gga_send_task);

        if (status_led != NULL) status_led->active = false;

        ESP_LOGW(TAG, "Disconnected from %s:%d/%s", host, port, mountpoint);
        uart_nmea("$PESP,NTRIP,CLI,DISCONNECTED,%s:%d,%s", host, port, mountpoint);

        _error:
        destroy_socket(&sock);

        free(buffer);
        free(host);
        free(mountpoint);
        free(username);
        free(password);
    }

    vTaskDelete(NULL);
}

void ntrip_client_init() {
    if (!config_get_bool1(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_ACTIVE))) return;

    xTaskCreate(ntrip_client_task, "ntrip_client_task", 4096, NULL, TASK_PRIORITY_INTERFACE, NULL);
}