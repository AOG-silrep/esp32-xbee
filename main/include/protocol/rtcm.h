#ifndef ESP32_XBEE_RTCM_H
#define ESP32_XBEE_RTCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RTCM_PREAMBLE 0xD3

// Header (3) + maximum payload (1023) + CRC-24Q (3)
#define RTCM_FRAME_MAX_LENGTH (3 + 1023 + 3)

typedef struct rtcm_parser {
    uint8_t buffer[RTCM_FRAME_MAX_LENGTH];
    size_t count;
    size_t length; // Header + payload, excluding CRC
} rtcm_parser_t;

typedef struct rtcm_station {
    uint16_t message_type;
    uint16_t station_id;
    double ecef[3]; // Antenna reference point, metres
    double antenna_height; // Metres, 1006 only
} rtcm_station_t;

void rtcm_parser_reset(rtcm_parser_t *parser);
bool rtcm_parser_push(rtcm_parser_t *parser, uint8_t byte);

uint16_t rtcm_message_type(const rtcm_parser_t *parser);
bool rtcm_decode_station(const rtcm_parser_t *parser, rtcm_station_t *station);

#endif //ESP32_XBEE_RTCM_H
