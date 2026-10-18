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

#include "protocol/rtcm.h"

#define RTCM_CRC24Q_POLY 0x1864CFB

// Bit positions are relative to the start of the frame, matching RTKLIB rtcm3.c
#define RTCM_HEADER_BITS 24
#define RTCM_MESSAGE_TYPE_BITS 12

#define RTCM_1005_BITS 140
#define RTCM_1006_BITS 156

static uint32_t rtcm_crc24q(const uint8_t *buffer, size_t length) {
    uint32_t crc = 0;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint32_t) buffer[i] << 16;
        for (int bit = 0; bit < 8; bit++) {
            crc <<= 1;
            if (crc & 0x1000000) crc ^= RTCM_CRC24Q_POLY;
        }
    }

    return crc & 0xFFFFFF;
}

static uint32_t rtcm_getbitu(const uint8_t *buffer, size_t pos, int length) {
    uint32_t bits = 0;
    for (size_t i = pos; i < pos + length; i++) {
        bits = (bits << 1) | ((buffer[i / 8] >> (7 - i % 8)) & 1u);
    }

    return bits;
}

static int32_t rtcm_getbits(const uint8_t *buffer, size_t pos, int length) {
    uint32_t bits = rtcm_getbitu(buffer, pos, length);
    if (length <= 0 || length >= 32 || !(bits & (1u << (length - 1)))) return (int32_t) bits;

    return (int32_t) (bits | (~0u << length));
}

static double rtcm_getbits_38(const uint8_t *buffer, size_t pos) {
    return (double) rtcm_getbits(buffer, pos, 32) * 64.0 + rtcm_getbitu(buffer, pos + 32, 6);
}

void rtcm_parser_reset(rtcm_parser_t *parser) {
    parser->count = 0;
    parser->length = 0;
}

bool rtcm_parser_push(rtcm_parser_t *parser, uint8_t byte) {
    if (parser->count == 0 && byte != RTCM_PREAMBLE) return false;

    parser->buffer[parser->count++] = byte;

    if (parser->count == 3) {
        // 6 reserved bits must be zero
        if (parser->buffer[1] & 0xFC) {
            rtcm_parser_reset(parser);
            return false;
        }

        parser->length = rtcm_getbitu(parser->buffer, 14, 10) + 3;
    }

    if (parser->count < 3 || parser->count < parser->length + 3) return false;

    parser->count = 0;

    return rtcm_crc24q(parser->buffer, parser->length) == rtcm_getbitu(parser->buffer, parser->length * 8, 24);
}

uint16_t rtcm_message_type(const rtcm_parser_t *parser) {
    if (parser->length * 8 < RTCM_HEADER_BITS + RTCM_MESSAGE_TYPE_BITS) return 0;

    return rtcm_getbitu(parser->buffer, RTCM_HEADER_BITS, RTCM_MESSAGE_TYPE_BITS);
}

bool rtcm_decode_station(const rtcm_parser_t *parser, rtcm_station_t *station) {
    const uint8_t *buffer = parser->buffer;
    size_t bits = parser->length * 8;
    size_t i = RTCM_HEADER_BITS + RTCM_MESSAGE_TYPE_BITS;

    uint16_t message_type = rtcm_message_type(parser);
    if (message_type == 1005) {
        if (i + RTCM_1005_BITS != bits) return false;
    } else if (message_type == 1006) {
        if (i + RTCM_1006_BITS > bits) return false;
    } else {
        return false;
    }

    station->message_type = message_type;
    station->station_id = rtcm_getbitu(buffer, i, 12); i += 12;
    i += 6 + 4; // ITRF realization year, GPS/GLONASS/Galileo/reference-station indicators
    station->ecef[0] = rtcm_getbits_38(buffer, i) * 0.0001; i += 38 + 2; // Oscillator indicator, reserved
    station->ecef[1] = rtcm_getbits_38(buffer, i) * 0.0001; i += 38 + 2; // Quarter cycle indicator
    station->ecef[2] = rtcm_getbits_38(buffer, i) * 0.0001; i += 38;
    station->antenna_height = message_type == 1006 ? rtcm_getbitu(buffer, i, 16) * 0.0001 : 0;

    return true;
}
