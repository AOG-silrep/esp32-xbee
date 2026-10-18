#ifndef ESP32_XBEE_RTK_STATUS_H
#define ESP32_XBEE_RTK_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol/nmea.h"
#include "protocol/rtcm.h"

typedef struct rtk_status {
    // Base station from RTCM 1005/1006
    bool base_valid;
    rtcm_station_t base;
    double base_age; // Seconds since last received

    // Rover from receiver NMEA
    bool gga_valid;
    nmea_gga_t gga;
    double gga_age;

    bool gst_valid;
    nmea_gst_t gst;
    double gst_age;

    // Rover relative to base, in the base's local east/north/up frame
    bool distance_valid;
    double distance_3d; // Metres
    double distance_horizontal;
    double distance_vertical; // Positive if rover is above base
} rtk_status_t;

void rtk_status_init();

void rtk_status_rtcm_feed(const uint8_t *data, size_t length);
void rtk_status_nmea_feed(const uint8_t *data, size_t length);
void rtk_status_base_reset();

bool rtk_status_get(rtk_status_t *status);

#endif //ESP32_XBEE_RTK_STATUS_H
