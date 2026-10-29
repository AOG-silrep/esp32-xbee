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

#include <inttypes.h>
#include <limits.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <wifi.h>
#include <cJSON.h>
#include <sys/param.h>
#include <config.h>
#include <log.h>
#include <core_dump.h>
#include <util.h>
#include <lwip/inet.h>
#include <esp_ota_ops.h>
#include <esp_wifi_ap_get_sta_list.h>
#include <stream_stats.h>
#include <rtk_status.h>
#include <f9p_config.h>
#include <www_files.h>
#include <lwip/sockets.h>
#include <esp_timer.h>
#include <math.h>
#include "interface/ntrip.h"
#include "web_server.h"

#define BUFFER_SIZE 4096

static const char *TAG = "WEB";

static char *buffer;

enum auth_method {
    AUTH_METHOD_OPEN = 0,
    AUTH_METHOD_HOTSPOT = 1,
    AUTH_METHOD_BASIC = 2
};

static char *basic_authentication;
static enum auth_method auth_method;

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

// Set HTTP response content type according to file extension
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".js")) {
        return httpd_resp_set_type(req, "application/javascript");
    } else if (IS_FILE_EXT(filename, ".css")) {
        return httpd_resp_set_type(req, "text/css");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        // Full path string won't fit into destination buffer
        return NULL;
    }

    // Construct full path (base + path)
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    // Return pointer to path, skipping the base
    return dest + base_pathlen;
}

static esp_err_t json_response(httpd_req_t *req, cJSON *root) {
    // Set mime type
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) return err;

    // Convert to string
    bool success = cJSON_PrintPreallocated(root, buffer, BUFFER_SIZE, false);
    cJSON_Delete(root);
    if (!success) {
        ESP_LOGE(TAG, "Not enough space in buffer to output JSON");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not enough space in buffer to output JSON");
        return ESP_FAIL;
    }

    // Send as response
    err = httpd_resp_send(req, buffer, strlen(buffer));
    if (err != ESP_OK) return err;

    return ESP_OK;
}

static esp_err_t basic_auth(httpd_req_t *req) {
    int authorization_length = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (authorization_length == 0) goto _auth_required;

    char *authorization_header = malloc(authorization_length);
    httpd_req_get_hdr_value_str(req, "Authorization", authorization_header, authorization_length);

    bool authenticated = strcasecmp(basic_authentication, authorization_header) == 0;
    free(authorization_header);

    if (authenticated) return ESP_OK;

    _auth_required:
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 XBee Config\"");
    httpd_resp_set_status(req, "401"); // Unauthorized
    char *unauthorized = "401 Unauthorized - Incorrect or no password provided";
    httpd_resp_send(req, unauthorized, strlen(unauthorized));
    return ESP_FAIL;
}

static esp_err_t hotspot_auth(httpd_req_t *req) {
    int sock = httpd_req_to_sockfd(req);

    struct sockaddr_in6 client_addr;
    socklen_t socklen = sizeof(client_addr);
    getpeername(sock, (struct sockaddr *)&client_addr, &socklen);

    // TODO: Correctly read IPv4?
    // ERROR_ACTION(TAG, client_addr.sin6_family != AF_INET, goto _auth_error, "IPv6 connections not supported, IP family %d", client_addr.sin6_family);

    wifi_sta_list_t *ap_sta_list = wifi_ap_sta_list();
    wifi_sta_mac_ip_list_t esp_netif_ap_sta_list;
    esp_wifi_ap_get_sta_list_with_ip(ap_sta_list, &esp_netif_ap_sta_list);

    // TODO: Correctly read IPv4?
    for (int i = 0; i < esp_netif_ap_sta_list.num; i++) {
        if (esp_netif_ap_sta_list.sta[i].ip.addr == client_addr.sin6_addr.un.u32_addr[3]) return ESP_OK;
    }

    //_auth_error:
    httpd_resp_set_status(req, "401"); // Unauthorized
    char *unauthorized = "401 Unauthorized - Configured to only accept connections from hotspot devices";
    httpd_resp_send(req, unauthorized, strlen(unauthorized));
    return ESP_FAIL;
}

static esp_err_t check_auth(httpd_req_t *req) {
    if (auth_method == AUTH_METHOD_HOTSPOT) return hotspot_auth(req);
    if (auth_method == AUTH_METHOD_BASIC) return basic_auth(req);
    return ESP_OK;
}

static esp_err_t log_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    // Clients ask for the log from the offset they have read up to, so each one gets the full history
    uint32_t since = 0;
    char query[64], value[11];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK) {
        since = strtoul(value, NULL, 10);
    }

    uint32_t start, end, head, boot_id;
    size_t length = log_read(since, buffer, BUFFER_SIZE, &start, &end, &head, &boot_id);

    // Header values must stay valid until the response is sent
    char start_str[11], end_str[11], head_str[11], boot_str[11];
    snprintf(start_str, sizeof(start_str), "%" PRIu32, start);
    snprintf(end_str, sizeof(end_str), "%" PRIu32, end);
    snprintf(head_str, sizeof(head_str), "%" PRIu32, head);
    snprintf(boot_str, sizeof(boot_str), "%" PRIu32, boot_id);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Log-Boot", boot_str);
    httpd_resp_set_hdr(req, "X-Log-Start", start_str);
    httpd_resp_set_hdr(req, "X-Log-End", end_str);
    httpd_resp_set_hdr(req, "X-Log-Head", head_str);

    httpd_resp_send(req, buffer, length);

    return ESP_OK;
}

static esp_err_t core_dump_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    size_t core_dump_size = core_dump_available();
    if (core_dump_size == 0) {
        httpd_resp_sendstr(req, "No core dump available");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");

    const esp_app_desc_t *app_desc = esp_app_get_description();

    char elf_sha256[7];
    esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));

    time_t t = time(NULL);
    char date[20] = "\0";
    if (t > 315360000l) strftime(date, sizeof(date), "_%F_%T", localtime(&t));

    char content_disposition[128];
    snprintf(content_disposition, sizeof(content_disposition),
            "attachment; filename=\"esp32_xbee_%s_core_dump_%s%s.bin\"", app_desc->version, elf_sha256, date);
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition);

    for (int offset = 0; offset < core_dump_size; offset += BUFFER_SIZE) {
        size_t read = core_dump_size - offset;
        if (read > BUFFER_SIZE) read = BUFFER_SIZE;

        core_dump_read(offset, buffer, read);
        httpd_resp_send_chunk(req, buffer, read);
    }

    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t heap_info_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);

    cJSON *root = cJSON_CreateObject();

    cJSON_AddNumberToObject(root, "total_free_bytes", info.total_free_bytes);
    cJSON_AddNumberToObject(root, "total_allocated_bytes", info.total_allocated_bytes);
    cJSON_AddNumberToObject(root, "largest_free_block", info.largest_free_block);
    cJSON_AddNumberToObject(root, "minimum_free_bytes", info.minimum_free_bytes);
    cJSON_AddNumberToObject(root, "allocated_blocks", info.allocated_blocks);
    cJSON_AddNumberToObject(root, "free_blocks", info.free_blocks);
    cJSON_AddNumberToObject(root, "total_blocks", info.total_blocks);

    return json_response(req, root);
}

static esp_err_t file_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char file_path[CONFIG_HTTPD_MAX_URI_LEN + sizeof("index.html")];

    // Extract filename from URL
    char *file_name = get_path_from_uri(file_path, "", req->uri, sizeof(file_path) - strlen("index.html"));
    ERROR_ACTION(TAG, file_name == NULL, {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }, "Filename too long")

    // If name has trailing '/', respond with index page
    if (file_name[strlen(file_name) - 1] == '/') {
        strcat(file_name, "index.html");

        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }

    set_content_type_from_file(req, file_name);

    const www_file_t *file = NULL;
    for (size_t i = 0; i < www_files_count && file == NULL; i++) {
        if (strcmp(www_files[i].path, file_name) == 0) file = &www_files[i];
    }

    // Check if file exists
    ERROR_ACTION(TAG, file == NULL, {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }, "Could not find file %s", file_name)

    httpd_resp_set_hdr(req, "ETag", file->etag);

    // Matching ETag sent by client, file is not modified
    char if_none_match[16];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", if_none_match, sizeof(if_none_match)) == ESP_OK
            && strcmp(if_none_match, file->etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Sending file %s (%u bytes gzipped)...", file_name, (unsigned int) file->size);

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *) file->data, file->size);
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "version", app_desc->version);

    int config_item_count;
    const config_item_t *config_items = config_items_get(&config_item_count);
    for (int i = 0; i < config_item_count; i++) {
        const config_item_t *item = &config_items[i];

        int64_t int64 = 0;
        uint64_t uint64 = 0;

        size_t length = 0;
        char *string = NULL;

        config_color_t color;
        esp_ip4_addr_t ip;

        switch (item->type) {
            case CONFIG_ITEM_TYPE_STRING:
            case CONFIG_ITEM_TYPE_BLOB:
                // Get length
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_str_blob(item, NULL, &length));
                string = calloc(1, length + 1);

                // Get value
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_str_blob(item, string, &length));
                string[length] = '\0';
                break;
            case CONFIG_ITEM_TYPE_COLOR:
                // Convert to hex
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &color));
                asprintf(&string, "#%02x%02x%02x", color.values.red, color.values.green, color.values.blue);
                break;
            case CONFIG_ITEM_TYPE_IP:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &ip));
                cJSON *ip_parts = cJSON_AddArrayToObject(root, item->key);
                for (int b = 0; b < 4; b++) {
                    cJSON_AddItemToArray(ip_parts, cJSON_CreateNumber(esp_ip4_addr_get_byte(&ip, b)));
                }

                break;
            case CONFIG_ITEM_TYPE_UINT8:
            case CONFIG_ITEM_TYPE_UINT16:
            case CONFIG_ITEM_TYPE_UINT32:
            case CONFIG_ITEM_TYPE_UINT64:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &uint64));
                asprintf(&string, "%llu", uint64);
                break;
            case CONFIG_ITEM_TYPE_BOOL:
            case CONFIG_ITEM_TYPE_INT8:
            case CONFIG_ITEM_TYPE_INT16:
            case CONFIG_ITEM_TYPE_INT32:
            case CONFIG_ITEM_TYPE_INT64:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &int64));
                asprintf(&string, "%lld", int64);
                break;
            default:
                string = calloc(1, 1);
                break;
        }

        if (string != NULL) {
            // Hide secret values that aren't empty
            char *value = item->secret && strlen(string) > 0 ? CONFIG_VALUE_UNCHANGED : string;
            cJSON_AddStringToObject(root, item->key, value);

            free(string);
        }
    }

    return json_response(req, root);
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    int ret = httpd_req_recv(req, buffer, BUFFER_SIZE - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }

        return ESP_FAIL;
    }

    buffer[ret] = '\0';

    cJSON *root = cJSON_Parse(buffer);

    int config_item_count;
    const config_item_t *config_items = config_items_get(&config_item_count);
    for (int i = 0; i < config_item_count; i++) {
        config_item_t item = config_items[i];

        if (cJSON_HasObjectItem(root, item.key)) {
            cJSON *entry = cJSON_GetObjectItem(root, item.key);

            size_t length = 0;
            if (cJSON_IsString(entry)) {
                length = strlen(entry->valuestring);

                // Ignore empty primitives
                if (length == 0 && item.type != CONFIG_ITEM_TYPE_BLOB && item.type != CONFIG_ITEM_TYPE_STRING) continue;

                // Ignore unchanged values
                if (strcmp(entry->valuestring, CONFIG_VALUE_UNCHANGED) == 0) continue;
            }

            // TODO: Cleanup
            esp_err_t err;
            if (item.type > CONFIG_ITEM_TYPE_MAX) {
                err = ESP_ERR_INVALID_ARG;
            } else if (item.type == CONFIG_ITEM_TYPE_STRING) {
                err = config_set_str(item.key, entry->valuestring);
            } else if (item.type == CONFIG_ITEM_TYPE_BLOB) {
                err = config_set_blob(item.key, entry->valuestring, length);
            } else if (item.type == CONFIG_ITEM_TYPE_COLOR) {
                bool is_black = strcmp(entry->valuestring, "#000000") == 0;
                config_color_t color;
                color.rgba = strtoul(entry->valuestring + 1, NULL, 16) << 8u;

                if (!is_black && color.rgba == 0) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    // Set alpha to default
                    if (!is_black) color.values.alpha = item.def.color.values.alpha;

                    err = config_set_color(item.key, color);
                }
            } else if (item.type == CONFIG_ITEM_TYPE_IP) {
                uint8_t a[4];

                if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) != 4) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    for (int b = 0; b < 4; b++) {
                        a[b] = (uint8_t) strtoul(cJSON_GetArrayItem(entry, b)->valuestring, NULL, 10);
                    }
;
                    uint32_t ip = esp_netif_htonl(esp_netif_ip4_makeu32(a[0], a[1], a[2], a[3]));
                    err = config_set_u32(item.key, ip);
                }
            } else {
                bool is_zero = strcmp(entry->valuestring, "0") == 0 || strcmp(entry->valuestring, "0.0") == 0;
                int64_t int64 = strtol(entry->valuestring, NULL, 10);
                uint64_t uint64 = strtoul(entry->valuestring, NULL, 10);

                if (!is_zero && (int64 == 0 || uint64 == 0)) {
                    err = ESP_ERR_INVALID_ARG;
                } else {
                    switch (item.type) {
                        case CONFIG_ITEM_TYPE_BOOL:
                        case CONFIG_ITEM_TYPE_INT8:
                        case CONFIG_ITEM_TYPE_INT16:
                        case CONFIG_ITEM_TYPE_INT32:
                        case CONFIG_ITEM_TYPE_INT64:
                            err = config_set(&item, &int64);
                            break;
                        case CONFIG_ITEM_TYPE_UINT8:
                        case CONFIG_ITEM_TYPE_UINT16:
                        case CONFIG_ITEM_TYPE_UINT32:
                        case CONFIG_ITEM_TYPE_UINT64:
                            err = config_set(&item, &uint64);
                            break;
                        default:
                            err = ESP_FAIL;
                            break;
                    }
                }
            }

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error setting %s = %s: %d - %s", item.key, entry->valuestring, err, esp_err_to_name(err));
            }
        }
    }

    cJSON_Delete(root);

    config_commit();
    config_restart();

    root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);

    return json_response(req, root);
}

static esp_err_t config_reset_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    // Keep the WiFi station credentials unless explicitly asked to clear them
    bool clear_wifi_sta = false;

    int ret = httpd_req_recv(req, buffer, BUFFER_SIZE - 1);
    if (ret > 0) {
        buffer[ret] = '\0';

        cJSON *request = cJSON_Parse(buffer);
        clear_wifi_sta = cJSON_IsTrue(cJSON_GetObjectItem(request, "wifi_sta"));
        cJSON_Delete(request);
    } else if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        httpd_resp_send_408(req);

        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Resetting configuration to defaults, WiFi station credentials %s",
            clear_wifi_sta ? "cleared" : "kept");

    esp_err_t err = config_reset_defaults(clear_wifi_sta);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config_reset_defaults failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));

        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    esp_err_t resp_err = json_response(req, root);

    config_restart();

    return resp_err;
}

// Computes each core's CPU load (%) since the previous call, based on the change in time
// each core's idle task spent running relative to the elapsed wall clock time.
static void cpu_load_get(float loads[portNUM_PROCESSORS]) {
    static bool initialized = false;
    static int64_t prev_time = 0;
    static configRUN_TIME_COUNTER_TYPE prev_idle[portNUM_PROCESSORS];

    int64_t time = esp_timer_get_time();
    configRUN_TIME_COUNTER_TYPE idle[portNUM_PROCESSORS];
    for (int core = 0; core < portNUM_PROCESSORS; core++) {
        idle[core] = ulTaskGetIdleRunTimeCounterForCore(core);
    }

    int64_t delta_time = time - prev_time;

    for (int core = 0; core < portNUM_PROCESSORS; core++) {
        if (initialized && delta_time > 0) {
            configRUN_TIME_COUNTER_TYPE delta_idle = idle[core] - prev_idle[core];
            loads[core] = 100.0f - MIN(100.0f, (float) delta_idle * 100.0f / (float) delta_time);
        } else {
            loads[core] = 0;
        }

        prev_idle[core] = idle[core];
    }

    prev_time = time;
    initialized = true;
}

static void json_add_number_if_valid(cJSON *object, const char *name, double number) {
    if (!isnan(number)) cJSON_AddNumberToObject(object, name, number);
}

static void rtk_status_json(cJSON *root) {
    rtk_status_t status;
    if (!rtk_status_get(&status)) return;

    cJSON *rtk = cJSON_AddObjectToObject(root, "rtk");

    if (status.base_valid) {
        cJSON *base = cJSON_AddObjectToObject(rtk, "base");
        cJSON_AddNumberToObject(base, "station_id", status.base.station_id);
        cJSON_AddNumberToObject(base, "message_type", status.base.message_type);
        cJSON *ecef = cJSON_AddArrayToObject(base, "ecef");
        for (int i = 0; i < 3; i++) {
            cJSON_AddItemToArray(ecef, cJSON_CreateNumber(status.base.ecef[i]));
        }
        if (status.base.message_type == 1006) cJSON_AddNumberToObject(base, "antenna_height", status.base.antenna_height);
        cJSON_AddNumberToObject(base, "age", status.base_age);
    }

    if (status.gga_valid) {
        cJSON *rover = cJSON_AddObjectToObject(rtk, "rover");
        json_add_number_if_valid(rover, "lat", status.gga.latitude);
        json_add_number_if_valid(rover, "lon", status.gga.longitude);
        cJSON_AddNumberToObject(rover, "fix", status.gga.fix_quality);
        cJSON_AddNumberToObject(rover, "sats", status.gga.satellites);
        json_add_number_if_valid(rover, "hdop", status.gga.hdop);
        json_add_number_if_valid(rover, "correction_age", status.gga.correction_age);
        if (status.gga.station_id >= 0) cJSON_AddNumberToObject(rover, "station_id", status.gga.station_id);
        cJSON_AddNumberToObject(rover, "age", status.gga_age);
    }

    if (status.gst_valid) {
        cJSON *gst = cJSON_AddObjectToObject(rtk, "gst");
        json_add_number_if_valid(gst, "lat", status.gst.sigma_latitude);
        json_add_number_if_valid(gst, "lon", status.gst.sigma_longitude);
        json_add_number_if_valid(gst, "alt", status.gst.sigma_altitude);
        cJSON_AddNumberToObject(gst, "age", status.gst_age);
    }

    if (status.gsa_valid) {
        cJSON *gsa = cJSON_AddObjectToObject(rtk, "gsa");
        json_add_number_if_valid(gsa, "vdop", status.gsa.vdop);
        cJSON_AddNumberToObject(gsa, "age", status.gsa_age);
    }

    if (status.distance_valid) {
        cJSON *distance = cJSON_AddObjectToObject(rtk, "distance");
        cJSON_AddNumberToObject(distance, "d3", status.distance_3d);
        cJSON_AddNumberToObject(distance, "horizontal", status.distance_horizontal);
        cJSON_AddNumberToObject(distance, "vertical", status.distance_vertical);
    }
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();

    // Uptime
    cJSON_AddNumberToObject(root, "uptime", (int) ((double) esp_timer_get_time() / 1000000));

    // Heap
    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(heap, "total", heap_caps_get_total_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(heap, "free", heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // CPU load
    float cpu_loads[portNUM_PROCESSORS];
    cpu_load_get(cpu_loads);
    cJSON *cpu_load = cJSON_AddArrayToObject(root, "cpu_load");
    for (int core = 0; core < portNUM_PROCESSORS; core++) {
        cJSON_AddItemToArray(cpu_load, cJSON_CreateNumber(cpu_loads[core]));
    }

    // Streams
    cJSON *streams = cJSON_AddObjectToObject(root, "streams");
    stream_stats_values_t values;
    for (stream_stats_handle_t stats = stream_stats_first(); stats != NULL; stats = stream_stats_next(stats)) {
        stream_stats_values(stats, &values);

        cJSON *stream = cJSON_AddObjectToObject(streams, values.name);
        cJSON *total = cJSON_AddObjectToObject(stream, "total");
        cJSON_AddNumberToObject(total, "in", values.total_in);
        cJSON_AddNumberToObject(total, "out", values.total_out);
        cJSON *rate = cJSON_AddObjectToObject(stream, "rate");
        cJSON_AddNumberToObject(rate, "in", values.rate_in);
        cJSON_AddNumberToObject(rate, "out", values.rate_out);
    }

    // Sockets
    cJSON *sockets = cJSON_AddArrayToObject(root, "sockets");
    for (int s = LWIP_SOCKET_OFFSET; s < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; s++) {
        int err;

        int socktype;
        socklen_t socktype_len = sizeof(socktype);
        err = getsockopt(s, SOL_SOCKET, SO_TYPE, &socktype, &socktype_len);
        if (err < 0) continue;

        cJSON *socket = cJSON_CreateObject();

        cJSON_AddStringToObject(socket, "type", SOCKTYPE_NAME(socktype));

        struct sockaddr_in6 addr;
        socklen_t socklen = sizeof(addr);

        err = getsockname(s, (struct sockaddr *)&addr, &socklen);
        if (err == 0) cJSON_AddStringToObject(socket, "local", sockaddrtostr((struct sockaddr *) &addr));

        err = getpeername(s, (struct sockaddr *)&addr, &socklen);
        if (err == 0) cJSON_AddStringToObject(socket, "peer", sockaddrtostr((struct sockaddr *) &addr));

        cJSON_AddItemToArray(sockets, socket);
    }

    // WiFi
    wifi_ap_status_t ap_status;
    wifi_sta_status_t sta_status;

    wifi_ap_status(&ap_status);
    wifi_sta_status(&sta_status);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");

    cJSON *ap = cJSON_AddObjectToObject(wifi, "ap");
    cJSON_AddBoolToObject(ap, "active", ap_status.active);
    cJSON_AddBoolToObject(ap, "auto_disabled", ap_status.auto_disabled);
    if (ap_status.active) {
        cJSON_AddStringToObject(ap, "ssid", (char *) ap_status.ssid);
        cJSON_AddStringToObject(ap, "authmode", wifi_auth_mode_name(ap_status.authmode));
        cJSON_AddNumberToObject(ap, "devices", ap_status.devices);

        char ip[40];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ap_status.ip4_addr));
        cJSON_AddStringToObject(ap, "ip4", ip);
        snprintf(ip, sizeof(ip), IPV6STR, IPV62STR(ap_status.ip6_addr));
        cJSON_AddStringToObject(ap, "ip6", ip);
    }

    cJSON *sta = cJSON_AddObjectToObject(wifi, "sta");
    cJSON_AddBoolToObject(sta, "active", sta_status.active);
    if (sta_status.active) {
        cJSON_AddBoolToObject(sta, "connected", sta_status.connected);
        if (sta_status.connected) {
            cJSON_AddStringToObject(sta, "ssid", (char *) sta_status.ssid);
            cJSON_AddStringToObject(sta, "authmode", wifi_auth_mode_name(sta_status.authmode));
            cJSON_AddNumberToObject(sta, "rssi", sta_status.rssi);

            char ip[40];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&sta_status.ip4_addr));
            cJSON_AddStringToObject(sta, "ip4", ip);
            snprintf(ip, sizeof(ip), IPV6STR, IPV62STR(sta_status.ip6_addr));
            cJSON_AddStringToObject(sta, "ip6", ip);
        }
    }

    // RTK base station / rover
    rtk_status_json(root);

    return json_response(req, root);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    uint16_t ap_count;
    wifi_ap_record_t *ap_records =  wifi_scan(&ap_count);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        wifi_ap_record_t *ap_record = &ap_records[i];
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddItemToArray(root, ap);
        cJSON_AddStringToObject(ap, "ssid", (char *) ap_record->ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_record->rssi);
        cJSON_AddStringToObject(ap, "authmode", wifi_auth_mode_name(ap_record->authmode));
    }

    free(ap_records);

    return json_response(req, root);
}

static esp_err_t ntrip_sourcetable_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    int ret = httpd_req_recv(req, buffer, BUFFER_SIZE - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }

        return ESP_FAIL;
    }

    buffer[ret] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    cJSON *host_item = cJSON_GetObjectItem(root, "host");
    cJSON *port_item = cJSON_GetObjectItem(root, "port");
    cJSON *user_item = cJSON_GetObjectItem(root, "user");
    cJSON *pass_item = cJSON_GetObjectItem(root, "pass");

    char *host = cJSON_IsString(host_item) ? host_item->valuestring : "";
    int port = cJSON_IsNumber(port_item) ? port_item->valueint :
            (cJSON_IsString(port_item) ? atoi(port_item->valuestring) : 0);
    char *username = cJSON_IsString(user_item) ? user_item->valuestring : "";

    if (strlen(host) == 0 || port <= 0 || port > 65535) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Host and port required");
        return ESP_FAIL;
    }

    // Saved password is masked in the form, so fall back to the stored one
    char *password = NULL;
    if (!cJSON_IsString(pass_item) || strcmp(pass_item->valuestring, CONFIG_VALUE_UNCHANGED) == 0) {
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_NTRIP_CLIENT_PASSWORD), (void **) &password);
    } else {
        password = strdup(pass_item->valuestring);
    }

    esp_err_t err = ESP_OK;
    const char *error = NULL;
    char error_message[128];
    char *authorization = NULL;
    char *status = NULL;
    char *body = NULL;
    int len = 0;

    int sock = connect_socket(host, port, SOCK_STREAM);
    if (sock == CONNECT_SOCKET_ERROR_RESOLVE) {
        error = "Could not resolve host";
        goto _done;
    } else if (sock < 0) {
        error = "Could not connect to host";
        goto _done;
    }

    // HTTP/1.0 so the caster sends a plain body and closes the connection when done
    if (strlen(username) > 0) authorization = http_auth_basic_header(username, password != NULL ? password : "");
    snprintf(buffer, BUFFER_SIZE, "GET / HTTP/1.0" NEWLINE \
            "Host: %s:%d" NEWLINE \
            "User-Agent: NTRIP %s/%s" NEWLINE \
            "%s%s%s" \
            NEWLINE
            , host, port, NTRIP_CLIENT_NAME, &esp_app_get_description()->version[1],
            authorization != NULL ? "Authorization: " : "", authorization != NULL ? authorization : "",
            authorization != NULL ? NEWLINE : "");

    if (send(sock, buffer, strlen(buffer), 0) < 0) {
        error = "Could not send request to caster";
        goto _done;
    }

    // Read until the end of the response headers
    while (body == NULL && len < BUFFER_SIZE - 1) {
        int read_len = recv(sock, buffer + len, BUFFER_SIZE - 1 - len, 0);
        if (read_len <= 0) break;

        len += read_len;
        buffer[len] = '\0';
        body = strstr(buffer, NEWLINE NEWLINE);
    }

    if (len == 0) {
        error = "No response from caster";
        goto _done;
    }

    status = extract_http_header(buffer, "");
    if (status == NULL) {
        error = "Malformed response from caster";
        goto _done;
    } else if (!ntrip_response_sourcetable_ok(status)) {
        snprintf(error_message, sizeof(error_message), "Caster did not return a sourcetable: %s", status);
        error = error_message;
        goto _done;
    } else if (body == NULL) {
        error = "Malformed response from caster";
        goto _done;
    }

    // Stream the sourcetable through, it can be far larger than the buffer
    httpd_resp_set_type(req, "text/plain");

    body += strlen(NEWLINE NEWLINE);
    bool ended = strstr(body, "ENDSOURCETABLE") != NULL;
    int body_len = len - (int) (body - buffer);
    if (body_len > 0) err = httpd_resp_send_chunk(req, body, body_len);

    while (err == ESP_OK && !ended && (len = recv(sock, buffer, BUFFER_SIZE, 0)) > 0) {
        ended = memmem(buffer, len, "ENDSOURCETABLE", strlen("ENDSOURCETABLE")) != NULL;
        err = httpd_resp_send_chunk(req, buffer, len);
    }

    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);

    _done:
    if (error != NULL) {
        ESP_LOGW(TAG, "Sourcetable from %s:%d failed: %s", host, port, error);
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_set_type(req, "text/plain");
        err = httpd_resp_sendstr(req, error);
    }

    destroy_socket(&sock);
    free(status);
    free(authorization);
    free(password);
    cJSON_Delete(root);

    return err;
}

// Whole number or string of digits, -1 if neither
static int json_int(const cJSON *item) {
    if (cJSON_IsNumber(item)) return item->valuedouble == (double) item->valueint ? item->valueint : -1;
    if (cJSON_IsString(item) && strlen(item->valuestring) > 0) {
        char *end;
        long value = strtol(item->valuestring, &end, 10);
        return *end == '\0' && value >= 0 && value <= INT_MAX ? (int) value : -1;
    }
    return -1;
}

// Receiver settings from the request body, sends the error response if they are missing or invalid
static esp_err_t f9p_settings_from_request(httpd_req_t *req, f9p_settings_t *settings) {
    int ret = httpd_req_recv(req, buffer, BUFFER_SIZE - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }

        return ESP_FAIL;
    }

    buffer[ret] = '\0';

    cJSON *request = cJSON_Parse(buffer);
    cJSON *mode_item = cJSON_GetObjectItem(request, "mode");
    bool dual = cJSON_IsString(mode_item) && strcmp(mode_item->valuestring, "dual") == 0;
    bool rs232 = cJSON_IsString(mode_item) && strcmp(mode_item->valuestring, "rs232") == 0;
    int gga = json_int(cJSON_GetObjectItem(request, "gga"));
    int vtg = json_int(cJSON_GetObjectItem(request, "vtg"));
    int rmc = json_int(cJSON_GetObjectItem(request, "rmc"));
    int gsa = json_int(cJSON_GetObjectItem(request, "gsa"));
    int baud = json_int(cJSON_GetObjectItem(request, "baud"));
    cJSON_Delete(request);

    *settings = (f9p_settings_t) {
            .mode = rs232 ? F9P_MODE_RS232 : F9P_MODE_DUAL,
            .gga_hz = gga,
            .vtg_hz = vtg,
            .rmc_hz = rmc,
            .gsa_hz = gsa,
            .uart2_baud = baud
    };

    if (!(dual || rs232) || !f9p_settings_valid(settings)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Mode must be dual or rs232, rates 0, 1, 2, 5 or 10 Hz "
                "up to GGA 10, VTG 5, RMC 2 and GSA 1 Hz, baud 19200 or 38400");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t f9p_config_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    f9p_settings_t settings;
    if (f9p_settings_from_request(req, &settings) != ESP_OK) return ESP_FAIL;

    f9p_result_t result;
    esp_err_t err = f9p_config_write(&settings, &result);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    cJSON_AddNumberToObject(root, "keys", result.keys);
    cJSON_AddNumberToObject(root, "total", result.total);
    cJSON *rejected = cJSON_AddArrayToObject(root, "rejected");
    for (int i = 0; i < result.rejected_count; i++) {
        char key[11];
        snprintf(key, sizeof(key), "0x%08" PRIX32, result.rejected[i]);
        cJSON_AddItemToArray(rejected, cJSON_CreateString(key));
    }
    if (result.error != NULL) cJSON_AddStringToObject(root, "error", result.error);

    return json_response(req, root);
}

static void f9p_state_json(cJSON *root, const char *name, const f9p_state_t *state) {
    if (!state->present) {
        cJSON_AddNullToObject(root, name);
        return;
    }

    cJSON *object = cJSON_AddObjectToObject(root, name);
    if (state->mode == F9P_MODE_DUAL) {
        cJSON_AddStringToObject(object, "mode", "dual");
    } else if (state->mode == F9P_MODE_RS232) {
        cJSON_AddStringToObject(object, "mode", "rs232");
    } else {
        cJSON_AddNullToObject(object, "mode");
    }

    cJSON *out = cJSON_AddObjectToObject(object, "out");
    cJSON_AddBoolToObject(out, "ubx", state->out_ubx);
    cJSON_AddBoolToObject(out, "nmea", state->out_nmea);
    cJSON_AddBoolToObject(out, "rtcm3", state->out_rtcm3);

    cJSON_AddNumberToObject(object, "baud", state->uart2_baud);
    cJSON_AddNumberToObject(object, "nav_hz", state->nav_hz);
    cJSON_AddNumberToObject(object, "gga", state->gga_hz);
    cJSON_AddNumberToObject(object, "vtg", state->vtg_hz);
    cJSON_AddNumberToObject(object, "rmc", state->rmc_hz);
    cJSON_AddNumberToObject(object, "gsa", state->gsa_hz);
}

static esp_err_t f9p_config_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    f9p_state_t ram, flash;
    const char *error;
    esp_err_t err = f9p_config_read(&ram, &flash, &error);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    if (error != NULL) cJSON_AddStringToObject(root, "error", error);
    if (err == ESP_OK) {
        f9p_state_json(root, "ram", &ram);
        f9p_state_json(root, "flash", &flash);
    }

    return json_response(req, root);
}

static void f9p_check_json(cJSON *root, const char *name, const f9p_check_t *check) {
    cJSON *object = cJSON_AddObjectToObject(root, name);
    cJSON_AddNumberToObject(object, "differ", check->differ);
    cJSON_AddNumberToObject(object, "missing", check->missing);

    // 64 bit values as strings, JSON numbers lose precision past 2^53
    cJSON *keys = cJSON_AddArrayToObject(object, "keys");
    for (int i = 0; i < check->count; i++) {
        const f9p_difference_t *difference = &check->keys[i];
        char text[24];

        cJSON *key = cJSON_CreateObject();
        snprintf(text, sizeof(text), "0x%08" PRIX32, difference->key);
        cJSON_AddStringToObject(key, "key", text);
        snprintf(text, sizeof(text), "%" PRIu64, difference->expected);
        cJSON_AddStringToObject(key, "expected", text);
        if (difference->missing) {
            cJSON_AddNullToObject(key, "actual");
        } else {
            snprintf(text, sizeof(text), "%" PRIu64, difference->actual);
            cJSON_AddStringToObject(key, "actual", text);
        }

        cJSON_AddItemToArray(keys, key);
    }
}

static esp_err_t f9p_check_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    f9p_settings_t settings;
    if (f9p_settings_from_request(req, &settings) != ESP_OK) return ESP_FAIL;

    // Too large for the web server task's stack
    static f9p_check_t ram, flash;
    const char *error;
    esp_err_t err = f9p_config_check(&settings, &ram, &flash, &error);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    if (error != NULL) cJSON_AddStringToObject(root, "error", error);
    if (err == ESP_OK) {
        cJSON_AddNumberToObject(root, "total", ram.total);
        f9p_check_json(root, "ram", &ram);
        f9p_check_json(root, "flash", &flash);
    }

    return json_response(req, root);
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buffer, MIN(remaining, BUFFER_SIZE));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) continue;

            ESP_LOGE(TAG, "Error receiving OTA firmware data");
            esp_ota_abort(ota_handle);
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_handle, buffer, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }

        remaining -= recv_len;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful, restarting into partition %s", update_partition->label);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    esp_err_t resp_err = json_response(req, root);

    config_restart();

    return resp_err;
}

static esp_err_t register_uri_handler(httpd_handle_t server, const char *path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *r)) {
    httpd_uri_t uri_config_get = {
            .uri        = path,
            .method     = method,
            .handler    = handler
    };
    return httpd_register_uri_handler(server, &uri_config_get);
}

static httpd_handle_t web_server_start(void)
{
    config_get_primitive(CONF_ITEM(KEY_CONFIG_ADMIN_AUTH), &auth_method);
    if (auth_method == AUTH_METHOD_BASIC) {
        char *username, *password;
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_ADMIN_USERNAME), (void **) &username);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_ADMIN_PASSWORD), (void **) &password);
        basic_authentication = http_auth_basic_header(username, password);
        free(username);
        free(password);
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 15;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        register_uri_handler(server, "/config", HTTP_GET, config_get_handler);
        register_uri_handler(server, "/config", HTTP_POST, config_post_handler);
        register_uri_handler(server, "/config/reset", HTTP_POST, config_reset_post_handler);
        register_uri_handler(server, "/status", HTTP_GET, status_get_handler);

        register_uri_handler(server, "/log", HTTP_GET, log_get_handler);
        register_uri_handler(server, "/core_dump", HTTP_GET, core_dump_get_handler);
        register_uri_handler(server, "/heap_info", HTTP_GET, heap_info_get_handler);

        register_uri_handler(server, "/wifi/scan", HTTP_GET, wifi_scan_get_handler);

        register_uri_handler(server, "/ntrip/sourcetable", HTTP_POST, ntrip_sourcetable_post_handler);

        register_uri_handler(server, "/f9p/config", HTTP_GET, f9p_config_get_handler);
        register_uri_handler(server, "/f9p/config", HTTP_POST, f9p_config_post_handler);
        register_uri_handler(server, "/f9p/check", HTTP_POST, f9p_check_post_handler);

        register_uri_handler(server, "/ota", HTTP_POST, ota_post_handler);

        register_uri_handler(server, "/*", HTTP_GET, file_get_handler);
    }

    if (server == NULL) {
        ESP_LOGE(TAG, "Could not start server");
        return NULL;
    }

    buffer = malloc(BUFFER_SIZE);

    return server;
}

void web_server_init() {
    web_server_start();
}