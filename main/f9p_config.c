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

#include <inttypes.h>
#include <string.h>
#include <sys/param.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "f9p_config.h"
#include "f9p_config_data.h"
#include "uart.h"

static const char *TAG = "F9P_CONFIG";

#define UBX_SYNC_1 0xB5
#define UBX_SYNC_2 0x62
#define UBX_CLASS_ACK 0x05
#define UBX_ID_ACK_NAK 0x00
#define UBX_ID_ACK_ACK 0x01
#define UBX_CLASS_CFG 0x06
#define UBX_ID_CFG_VALSET 0x8A
#define UBX_ID_CFG_VALGET 0x8B

#define UBX_HEADER_LENGTH 6
#define UBX_CHECKSUM_LENGTH 2

// Up to 64 keys per CFG-VALSET or CFG-VALGET answer
#define CFG_MAX_KEYS 64
#define CFG_PAYLOAD_MAX (4 + CFG_MAX_KEYS * (4 + 8))

// RAM, BBR and flash, so the config survives a power cycle
#define VALSET_LAYERS 0x07
#define VALSET_TIMEOUT_MS 2000
#define VALSET_ATTEMPTS 3

#define VALGET_LAYER_RAM 0
#define VALGET_LAYER_FLASH 2
#define VALGET_TIMEOUT_MS 1000
#define VALGET_ATTEMPTS 2

// Both configs run the navigation at 10 Hz (CFG-RATE-MEAS 100 ms), message rates are set in epochs
#define NAV_RATE_HZ 10

// Configuration keys from the u-blox ZED-F9P HPG 1.13 interface description
#define CFG_RATE_MEAS 0x30210001
#define CFG_RATE_NAV 0x30210002
#define CFG_UART1INPROT_UBX 0x10730001
#define CFG_UART2_BAUDRATE 0x40530001
#define CFG_UART2OUTPROT_UBX 0x10760001
#define CFG_UART2OUTPROT_NMEA 0x10760002
#define CFG_UART2OUTPROT_RTCM3X 0x10760004
#define CFG_USBOUTPROT_UBX 0x10780001
#define CFG_USBOUTPROT_NMEA 0x10780002
#define CFG_USBOUTPROT_RTCM3X 0x10780004

#define CFG_SIGNAL_SBAS_L1CA_ENA 0x10310005
#define CFG_SIGNAL_BDS_B1_ENA 0x1031000D
#define CFG_SIGNAL_BDS_B2_ENA 0x1031000E
#define CFG_SIGNAL_QZSS_L1CA_ENA 0x10310012
#define CFG_SIGNAL_QZSS_L1S_ENA 0x10310014
#define CFG_SIGNAL_QZSS_L2C_ENA 0x10310015
#define CFG_SIGNAL_SBAS_ENA 0x10310020
#define CFG_SIGNAL_BDS_ENA 0x10310022
#define CFG_SIGNAL_QZSS_ENA 0x10310024

#define CFG_NMEA_PROTVER 0x20930001
#define CFG_NMEA_MAINTALKERID 0x20930031
#define NMEA_PROTVER_4_11 42
#define NMEA_MAINTALKERID_GP 1

// CFG-MSGOUT keys of one message on each port
typedef struct msgout_keys {
    uint32_t uart1;
    uint32_t uart2;
    uint32_t usb;
} msgout_keys_t;

static const msgout_keys_t MSGOUT_UBX_NAV_PVT = {0x20910007, 0x20910008, 0x20910009};
static const msgout_keys_t MSGOUT_UBX_NAV_RELPOSNED = {0x2091008E, 0x2091008F, 0x20910090};
static const msgout_keys_t MSGOUT_NMEA_RMC = {0x209100AC, 0x209100AD, 0x209100AE};
static const msgout_keys_t MSGOUT_NMEA_VTG = {0x209100B1, 0x209100B2, 0x209100B3};
static const msgout_keys_t MSGOUT_NMEA_GGA = {0x209100BB, 0x209100BC, 0x209100BD};
static const msgout_keys_t MSGOUT_NMEA_GSA = {0x209100C0, 0x209100C1, 0x209100C2};
static const msgout_keys_t MSGOUT_NMEA_GSV = {0x209100C5, 0x209100C6, 0x209100C7};
static const msgout_keys_t MSGOUT_NMEA_GST = {0x209100D4, 0x209100D5, 0x209100D6};

// Every message either config outputs on some port, in RS-232 mode USB outputs the same as UART2
static const msgout_keys_t *const MSGOUT_USB_MIRRORED[] = {
        &MSGOUT_UBX_NAV_PVT, &MSGOUT_UBX_NAV_RELPOSNED, &MSGOUT_NMEA_RMC, &MSGOUT_NMEA_VTG,
        &MSGOUT_NMEA_GGA, &MSGOUT_NMEA_GSA, &MSGOUT_NMEA_GSV, &MSGOUT_NMEA_GST
};

#define F9P_CFG_COUNT (sizeof(F9P_CFG_DUAL) / sizeof(F9P_CFG_DUAL[0]))

typedef enum {
    ANSWER_ACK, // ACK-ACK, or for CFG-VALGET the values
    ANSWER_NAK,
    ANSWER_TIMEOUT
} answer_result_t;

// A UBX frame from the receiver that answers one of ours
typedef struct ubx_answer {
    uint8_t msg_class;
    uint8_t msg_id;
    uint16_t length;
    uint8_t payload[CFG_PAYLOAD_MAX];
} ubx_answer_t;

static SemaphoreHandle_t lock;
static QueueHandle_t answer_queue;
static volatile bool active = false;

// Only accessed from UART read event handler
static struct {
    size_t position; // Bytes of the current frame received so far
    uint8_t ck_a;
    uint8_t ck_b;
    uint8_t received_ck_a;
    ubx_answer_t answer;
} parser;

// Only accessed while holding lock
static uint8_t frame[UBX_HEADER_LENGTH + CFG_PAYLOAD_MAX + UBX_CHECKSUM_LENGTH];
static ubx_answer_t answer;

static void ubx_checksum(const uint8_t *data, size_t length, uint8_t *ck_a, uint8_t *ck_b) {
    *ck_a = 0;
    *ck_b = 0;
    for (size_t i = 0; i < length; i++) {
        *ck_a += data[i];
        *ck_b += *ck_a;
    }
}

// Value size from the key's size bits
static size_t key_value_size(uint32_t key) {
    switch ((key >> 28) & 0x07) {
        case 1: // Bit, stored as a byte
        case 2:
            return 1;
        case 3:
            return 2;
        case 4:
            return 4;
        case 5:
            return 8;
        default:
            return 0;
    }
}

static const f9p_cfg_item_t *dual_item(uint32_t key) {
    for (size_t i = 0; i < F9P_CFG_COUNT; i++) {
        if (F9P_CFG_DUAL[i].key == key) return &F9P_CFG_DUAL[i];
    }

    return NULL;
}

// Value from the mode's config file
static uint64_t file_value(f9p_mode_t mode, const f9p_cfg_item_t *item) {
    if (mode == F9P_MODE_RS232) {
        for (size_t i = 0; i < sizeof(F9P_CFG_RS232) / sizeof(F9P_CFG_RS232[0]); i++) {
            if (F9P_CFG_RS232[i].key == item->key) return F9P_CFG_RS232[i].value;
        }
    }

    return item->value;
}

static uint8_t rate_epochs(int hz) {
    return hz == 0 ? 0 : NAV_RATE_HZ / hz;
}

static uint64_t setting_value(const f9p_settings_t *settings, const f9p_cfg_item_t *item);

// Value of another key, for keys that follow it
static uint64_t setting_value_of(const f9p_settings_t *settings, uint32_t key) {
    const f9p_cfg_item_t *item = dual_item(key);
    return item != NULL ? setting_value(settings, item) : 0;
}

// Value written to the receiver: the mode's config file with the changes made for this device
static uint64_t setting_value(const f9p_settings_t *settings, const f9p_cfg_item_t *item) {
    uint32_t key = item->key;

    switch (key) {
        // The ESP32 needs UBX input on UART1 to configure the receiver.
        // ACKs come back regardless of the UBX output setting.
        case CFG_UART1INPROT_UBX:
            return 1;

        // GPS+GLO+GAL only, the ZED-F9P-02B data sheet rates all four systems to 8 Hz RTK, three to 10 Hz
        case CFG_SIGNAL_QZSS_ENA:
        case CFG_SIGNAL_QZSS_L1CA_ENA:
        case CFG_SIGNAL_QZSS_L1S_ENA:
        case CFG_SIGNAL_QZSS_L2C_ENA:
        case CFG_SIGNAL_BDS_ENA:
        case CFG_SIGNAL_BDS_B1_ENA:
        case CFG_SIGNAL_BDS_B2_ENA:
            return 0;

        case CFG_SIGNAL_SBAS_ENA:
        case CFG_SIGNAL_SBAS_L1CA_ENA:
            return 1;

        case CFG_NMEA_PROTVER:
            return NMEA_PROTVER_4_11;
        case CFG_NMEA_MAINTALKERID:
            return NMEA_MAINTALKERID_GP;
    }

    if (key == MSGOUT_NMEA_VTG.uart1) return rate_epochs(1);

    if (settings->mode == F9P_MODE_RS232) {
        if (key == CFG_UART2_BAUDRATE) return settings->uart2_baud;

        if (key == MSGOUT_NMEA_GGA.uart2) return rate_epochs(settings->gga_hz);
        if (key == MSGOUT_NMEA_VTG.uart2) return rate_epochs(settings->vtg_hz);
        if (key == MSGOUT_NMEA_RMC.uart2) return rate_epochs(settings->rmc_hz);
        if (key == MSGOUT_NMEA_GSA.uart2) return rate_epochs(settings->gsa_hz);

        // VDOP and accuracy for the UART card at 1 Hz, the RS-232 file only has GGA and VTG on UART1
        if (key == MSGOUT_NMEA_GSA.uart1 || key == MSGOUT_NMEA_GST.uart1) return rate_epochs(1);

        if (key == CFG_USBOUTPROT_UBX) return setting_value_of(settings, CFG_UART2OUTPROT_UBX);
        if (key == CFG_USBOUTPROT_NMEA) return setting_value_of(settings, CFG_UART2OUTPROT_NMEA);
        if (key == CFG_USBOUTPROT_RTCM3X) return setting_value_of(settings, CFG_UART2OUTPROT_RTCM3X);

        for (size_t i = 0; i < sizeof(MSGOUT_USB_MIRRORED) / sizeof(MSGOUT_USB_MIRRORED[0]); i++) {
            const msgout_keys_t *message = MSGOUT_USB_MIRRORED[i];
            if (key == message->usb) return setting_value_of(settings, message->uart2);
        }
    }

    return file_value(settings->mode, item);
}

// Answers to our CFG-VALSET and CFG-VALGET, everything else the receiver sends (NMEA, NAV-PVT) is skipped
static void ubx_frame_complete(const ubx_answer_t *frame_answer) {
    bool cfg_ack = frame_answer->msg_class == UBX_CLASS_ACK &&
            (frame_answer->msg_id == UBX_ID_ACK_ACK || frame_answer->msg_id == UBX_ID_ACK_NAK) &&
            frame_answer->length == 2 && frame_answer->payload[0] == UBX_CLASS_CFG;
    bool valget = frame_answer->msg_class == UBX_CLASS_CFG && frame_answer->msg_id == UBX_ID_CFG_VALGET;

    if (cfg_ack || valget) xQueueSend(answer_queue, frame_answer, 0);
}

static void f9p_uart_handler(void *handler_args, esp_event_base_t base, int32_t length, void *buffer) {
    if (!active) return;

    const uint8_t *data = buffer;
    for (int32_t i = 0; i < length; i++) {
        uint8_t c = data[i];
        size_t position = parser.position++;

        if (position == 0) {
            if (c != UBX_SYNC_1) parser.position = 0;
            continue;
        } else if (position == 1) {
            if (c != UBX_SYNC_2) parser.position = c == UBX_SYNC_1 ? 1 : 0;
            parser.ck_a = 0;
            parser.ck_b = 0;
            continue;
        }

        size_t payload_end = UBX_HEADER_LENGTH + parser.answer.length;
        if (position < payload_end || position < UBX_HEADER_LENGTH) {
            parser.ck_a += c;
            parser.ck_b += parser.ck_a;
        }

        if (position == 2) {
            parser.answer.msg_class = c;
        } else if (position == 3) {
            parser.answer.msg_id = c;
        } else if (position == 4) {
            parser.answer.length = c;
        } else if (position == 5) {
            parser.answer.length |= c << 8;

            // Longer than anything we wait for, skip it and look for the next frame
            if (parser.answer.length > CFG_PAYLOAD_MAX) parser.position = 0;
        } else if (position < payload_end) {
            parser.answer.payload[position - UBX_HEADER_LENGTH] = c;
        } else if (position == payload_end) {
            parser.received_ck_a = c;
        } else {
            parser.position = 0;
            if (parser.received_ck_a == parser.ck_a && c == parser.ck_b) ubx_frame_complete(&parser.answer);
        }
    }
}

// Completes the UBX frame around a payload already in frame, returns the frame length
static size_t ubx_frame(uint8_t msg_id, size_t length) {
    frame[0] = UBX_SYNC_1;
    frame[1] = UBX_SYNC_2;
    frame[2] = UBX_CLASS_CFG;
    frame[3] = msg_id;
    frame[4] = length & 0xFF;
    frame[5] = length >> 8;
    ubx_checksum(frame + 2, length + 4, &frame[UBX_HEADER_LENGTH + length], &frame[UBX_HEADER_LENGTH + length + 1]);

    return UBX_HEADER_LENGTH + length + UBX_CHECKSUM_LENGTH;
}

// Waits for the ACK/NAK of the CFG message msg_id, or for a CFG-VALGET of layer the values of keys.
// Late answers to earlier requests are skipped.
static answer_result_t wait_answer(uint8_t msg_id, uint8_t layer, const uint32_t *keys, size_t count,
        TickType_t timeout) {
    TickType_t start = xTaskGetTickCount();

    while (true) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout || xQueueReceive(answer_queue, &answer, timeout - elapsed) != pdTRUE) {
            return ANSWER_TIMEOUT;
        }

        if (answer.msg_class == UBX_CLASS_ACK) {
            if (answer.payload[1] != msg_id) continue;
            if (answer.msg_id == UBX_ID_ACK_NAK) return ANSWER_NAK;
            if (msg_id == UBX_ID_CFG_VALSET) return ANSWER_ACK;

            // CFG-VALGET is ACKed after its values
            continue;
        }

        if (msg_id != UBX_ID_CFG_VALGET || answer.length < 4 || answer.payload[1] != layer) continue;

        // Values of another request start with a key this one didn't ask for
        if (answer.length >= 8) {
            uint32_t key = answer.payload[4] | answer.payload[5] << 8 | answer.payload[6] << 16 |
                    (uint32_t) answer.payload[7] << 24;

            bool asked = false;
            for (size_t i = 0; i < count && !asked; i++) asked = keys[i] == key;
            if (!asked) continue;
        }

        return ANSWER_ACK;
    }
}

static answer_result_t send_and_wait(size_t frame_length, uint8_t msg_id, uint8_t layer, const uint32_t *keys,
        size_t count, int timeout_ms, int attempts) {
    for (int attempt = 1; attempt <= attempts; attempt++) {
        xQueueReset(answer_queue);
        uart_write_exclusive((char *) frame, frame_length);

        answer_result_t result = wait_answer(msg_id, layer, keys, count, pdMS_TO_TICKS(timeout_ms));
        if (result != ANSWER_TIMEOUT) return result;

        ESP_LOGW(TAG, "No answer to CFG 0x%02X, attempt %d of %d", msg_id, attempt, attempts);
    }

    return ANSWER_TIMEOUT;
}

// Sends CFG-VALSET with the keys F9P_CFG_DUAL[start, start + count) and waits for the receiver's answer
static answer_result_t f9p_valset(const f9p_settings_t *settings, size_t start, size_t count) {
    uint8_t *payload = frame + UBX_HEADER_LENGTH;
    size_t length = 0;

    payload[length++] = 0; // Version, no transaction
    payload[length++] = VALSET_LAYERS;
    payload[length++] = 0;
    payload[length++] = 0;

    for (size_t i = start; i < start + count; i++) {
        uint32_t key = F9P_CFG_DUAL[i].key;
        uint64_t value = setting_value(settings, &F9P_CFG_DUAL[i]);

        for (int b = 0; b < 4; b++) payload[length++] = key >> (8 * b);
        for (size_t b = 0; b < key_value_size(key); b++) payload[length++] = value >> (8 * b);
    }

    return send_and_wait(ubx_frame(UBX_ID_CFG_VALSET, length), UBX_ID_CFG_VALSET, 0, NULL, 0, VALSET_TIMEOUT_MS,
            VALSET_ATTEMPTS);
}

// Reads up to CFG_MAX_KEYS keys from a layer, found[i] tells whether the layer has keys[i].
// A NAK means the layer has none of them, e.g. nothing saved to flash.
static answer_result_t f9p_valget(uint8_t layer, const uint32_t *keys, size_t count, uint64_t *values, bool *found) {
    uint8_t *payload = frame + UBX_HEADER_LENGTH;
    size_t length = 0;

    payload[length++] = 0; // Version
    payload[length++] = layer;
    payload[length++] = 0; // Position
    payload[length++] = 0;

    for (size_t i = 0; i < count; i++) {
        for (int b = 0; b < 4; b++) payload[length++] = keys[i] >> (8 * b);
        found[i] = false;
        values[i] = 0;
    }

    answer_result_t result = send_and_wait(ubx_frame(UBX_ID_CFG_VALGET, length), UBX_ID_CFG_VALGET, layer, keys,
            count, VALGET_TIMEOUT_MS, VALGET_ATTEMPTS);
    if (result != ANSWER_ACK) return result;

    for (size_t offset = 4; offset + 4 <= answer.length; ) {
        uint32_t key = answer.payload[offset] | answer.payload[offset + 1] << 8 | answer.payload[offset + 2] << 16 |
                (uint32_t) answer.payload[offset + 3] << 24;
        size_t size = key_value_size(key);
        if (size == 0 || offset + 4 + size > answer.length) break;

        uint64_t value = 0;
        for (size_t b = 0; b < size; b++) value |= (uint64_t) answer.payload[offset + 4 + b] << (8 * b);

        for (size_t i = 0; i < count; i++) {
            if (keys[i] == key) {
                values[i] = value;
                found[i] = true;
            }
        }

        offset += 4 + size;
    }

    return ANSWER_ACK;
}

static esp_err_t f9p_write_chunk(const f9p_settings_t *settings, size_t start, size_t count, f9p_result_t *result) {
    switch (f9p_valset(settings, start, count)) {
        case ANSWER_ACK:
            result->keys += count;
            return ESP_OK;
        case ANSWER_TIMEOUT:
            result->error = result->keys == 0 && result->rejected_count == 0 ?
                    "No answer from the receiver. Enable UBX input on F9P UART1 (CFG-UART1INPROT-UBX) once with u-center." :
                    "Receiver stopped answering";
            return ESP_ERR_TIMEOUT;
        case ANSWER_NAK:
            break;
    }

    // The receiver refuses the whole message if it refuses one key, send them one at a time to find out which
    ESP_LOGW(TAG, "Keys %zu-%zu refused, writing them one at a time", start, start + count - 1);
    for (size_t i = start; i < start + count; i++) {
        switch (f9p_valset(settings, i, 1)) {
            case ANSWER_ACK:
                result->keys++;
                break;
            case ANSWER_NAK:
                ESP_LOGW(TAG, "Key 0x%08" PRIX32 " refused", F9P_CFG_DUAL[i].key);
                if (result->rejected_count == F9P_REJECTED_MAX) {
                    result->error = "Receiver refused too many keys";
                    return ESP_FAIL;
                }

                result->rejected[result->rejected_count++] = F9P_CFG_DUAL[i].key;
                break;
            case ANSWER_TIMEOUT:
                result->error = "Receiver stopped answering";
                return ESP_ERR_TIMEOUT;
        }
    }

    return ESP_OK;
}

static bool rate_valid(int hz, int max_hz) {
    return hz == 0 || (hz > 0 && hz <= max_hz && NAV_RATE_HZ % hz == 0);
}

bool f9p_settings_valid(const f9p_settings_t *settings) {
    return (settings->mode == F9P_MODE_DUAL || settings->mode == F9P_MODE_RS232) &&
            rate_valid(settings->gga_hz, F9P_GGA_MAX_HZ) &&
            rate_valid(settings->vtg_hz, F9P_VTG_MAX_HZ) &&
            rate_valid(settings->rmc_hz, F9P_RMC_MAX_HZ) &&
            rate_valid(settings->gsa_hz, F9P_GSA_MAX_HZ) &&
            (settings->uart2_baud == 19200 || settings->uart2_baud == 38400);
}

// Takes the receiver for this task, keeping NTRIP and socket data from landing in the middle of a frame
static bool receiver_take() {
    if (xSemaphoreTake(lock, 0) != pdTRUE) return false;

    uart_set_exclusive(true);
    active = true;

    return true;
}

static void receiver_give() {
    active = false;
    uart_set_exclusive(false);

    xSemaphoreGive(lock);
}

static const char *const RECEIVER_BUSY = "Receiver busy with another read or write, try again";
static const char *const RECEIVER_NOT_ANSWERING =
        "Receiver not answering. F9P UART1 needs UBX input enabled (CFG-UART1INPROT-UBX) and its TX wired to the ESP32.";

esp_err_t f9p_config_write(const f9p_settings_t *settings, f9p_result_t *result) {
    *result = (f9p_result_t) {
            .total = F9P_CFG_COUNT
    };

    if (!f9p_settings_valid(settings)) {
        result->error = "Invalid receiver settings";
        return ESP_ERR_INVALID_ARG;
    }

    if (!receiver_take()) {
        result->error = RECEIVER_BUSY;
        return ESP_ERR_INVALID_STATE;
    }

    if (settings->mode == F9P_MODE_RS232) {
        ESP_LOGI(TAG, "Writing RS-232 config to receiver, %d baud, GGA %d Hz, VTG %d Hz, RMC %d Hz, GSA %d Hz",
                settings->uart2_baud, settings->gga_hz, settings->vtg_hz, settings->rmc_hz, settings->gsa_hz);
    } else {
        ESP_LOGI(TAG, "Writing dual GPS config to receiver");
    }

    esp_err_t err = ESP_OK;
    for (size_t start = 0; start < F9P_CFG_COUNT && err == ESP_OK; start += CFG_MAX_KEYS) {
        err = f9p_write_chunk(settings, start, MIN(CFG_MAX_KEYS, F9P_CFG_COUNT - start), result);
    }

    receiver_give();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Receiver config written, %d of %d keys, %d refused", result->keys, result->total,
                result->rejected_count);
    } else {
        ESP_LOGE(TAG, "Receiver config failed after %d of %d keys: %s", result->keys, result->total, result->error);
    }

    return err;
}

// Keys behind f9p_state_t, in the order state_from_values() expects
#define STATE_KEY_COUNT 10

static void state_keys(uint32_t keys[STATE_KEY_COUNT]) {
    const uint32_t state_keys[STATE_KEY_COUNT] = {
            CFG_RATE_MEAS, CFG_RATE_NAV,
            CFG_UART2OUTPROT_UBX, CFG_UART2OUTPROT_NMEA, CFG_UART2OUTPROT_RTCM3X, CFG_UART2_BAUDRATE,
            MSGOUT_NMEA_GGA.uart2, MSGOUT_NMEA_VTG.uart2, MSGOUT_NMEA_RMC.uart2, MSGOUT_NMEA_GSA.uart2
    };
    memcpy(keys, state_keys, sizeof(state_keys));
}

static double epochs_hz(double nav_hz, uint64_t epochs) {
    return epochs == 0 ? 0 : nav_hz / (double) epochs;
}

static void state_from_values(f9p_state_t *state, const uint64_t *values, const bool *found) {
    *state = (f9p_state_t) {
            .present = true,
            .mode = -1
    };

    for (int i = 0; i < STATE_KEY_COUNT; i++) {
        if (!found[i]) state->present = false;
    }
    if (!state->present) return;

    // Measurement period in ms, navigation solution every nav measurements
    uint64_t measurement_ms = values[0], nav_ratio = values[1];
    state->nav_hz = measurement_ms == 0 || nav_ratio == 0 ? 0 : 1000.0 / (double) (measurement_ms * nav_ratio);

    state->out_ubx = values[2];
    state->out_nmea = values[3];
    state->out_rtcm3 = values[4];
    state->uart2_baud = values[5];

    if (state->out_rtcm3 && !state->out_nmea) {
        state->mode = F9P_MODE_DUAL;
    } else if (state->out_nmea && !state->out_rtcm3) {
        state->mode = F9P_MODE_RS232;
    }

    state->gga_hz = epochs_hz(state->nav_hz, values[6]);
    state->vtg_hz = epochs_hz(state->nav_hz, values[7]);
    state->rmc_hz = epochs_hz(state->nav_hz, values[8]);
    state->gsa_hz = epochs_hz(state->nav_hz, values[9]);
}

esp_err_t f9p_config_read(f9p_state_t *ram, f9p_state_t *flash, const char **error) {
    *error = NULL;

    if (!receiver_take()) {
        *error = RECEIVER_BUSY;
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t keys[STATE_KEY_COUNT];
    uint64_t values[STATE_KEY_COUNT];
    bool found[STATE_KEY_COUNT];
    state_keys(keys);

    esp_err_t err = ESP_OK;
    answer_result_t result = f9p_valget(VALGET_LAYER_RAM, keys, STATE_KEY_COUNT, values, found);
    if (result == ANSWER_ACK) {
        state_from_values(ram, values, found);

        result = f9p_valget(VALGET_LAYER_FLASH, keys, STATE_KEY_COUNT, values, found);
        if (result == ANSWER_TIMEOUT) {
            *error = "Receiver stopped answering";
            err = ESP_ERR_TIMEOUT;
        } else {
            // A NAK leaves found all false: nothing saved
            state_from_values(flash, values, found);
        }
    } else {
        *error = result == ANSWER_NAK ? "Receiver refused to report its settings" : RECEIVER_NOT_ANSWERING;
        err = result == ANSWER_NAK ? ESP_FAIL : ESP_ERR_TIMEOUT;
    }

    receiver_give();

    return err;
}

static void check_add(f9p_check_t *check, uint32_t key, uint64_t expected, uint64_t actual, bool missing) {
    if (missing) {
        check->missing++;
    } else {
        check->differ++;
    }

    if (check->count < F9P_DIFFERENCES_MAX) {
        check->keys[check->count++] = (f9p_difference_t) {
                .key = key,
                .expected = expected,
                .actual = actual,
                .missing = missing
        };
    }
}

static esp_err_t check_layer(const f9p_settings_t *settings, uint8_t layer, f9p_check_t *check) {
    static uint32_t keys[CFG_MAX_KEYS];
    static uint64_t values[CFG_MAX_KEYS];
    static bool found[CFG_MAX_KEYS];

    *check = (f9p_check_t) {
            .total = F9P_CFG_COUNT
    };

    for (size_t start = 0; start < F9P_CFG_COUNT; start += CFG_MAX_KEYS) {
        size_t count = MIN(CFG_MAX_KEYS, F9P_CFG_COUNT - start);
        for (size_t i = 0; i < count; i++) keys[i] = F9P_CFG_DUAL[start + i].key;

        if (f9p_valget(layer, keys, count, values, found) == ANSWER_TIMEOUT) return ESP_ERR_TIMEOUT;

        for (size_t i = 0; i < count; i++) {
            uint64_t expected = setting_value(settings, &F9P_CFG_DUAL[start + i]);
            if (!found[i] || values[i] != expected) check_add(check, keys[i], expected, values[i], !found[i]);
        }
    }

    return ESP_OK;
}

esp_err_t f9p_config_check(const f9p_settings_t *settings, f9p_check_t *ram, f9p_check_t *flash, const char **error) {
    *error = NULL;

    if (!f9p_settings_valid(settings)) {
        *error = "Invalid receiver settings";
        return ESP_ERR_INVALID_ARG;
    }

    if (!receiver_take()) {
        *error = RECEIVER_BUSY;
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = check_layer(settings, VALGET_LAYER_RAM, ram);
    if (err == ESP_OK) {
        err = check_layer(settings, VALGET_LAYER_FLASH, flash);
        if (err != ESP_OK) *error = "Receiver stopped answering";
    } else {
        *error = RECEIVER_NOT_ANSWERING;
    }

    receiver_give();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Receiver checked, RAM %d differ %d missing, flash %d differ %d missing",
                ram->differ, ram->missing, flash->differ, flash->missing);
    }

    return err;
}

void f9p_config_init() {
    lock = xSemaphoreCreateMutex();
    answer_queue = xQueueCreate(2, sizeof(ubx_answer_t));

    uart_register_read_handler(f9p_uart_handler);
}
