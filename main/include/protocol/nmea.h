#ifndef ESP32_XBEE_NMEA_H
#define ESP32_XBEE_NMEA_H

#include <stdarg.h>
#include <stdbool.h>

typedef struct nmea_gga {
    double latitude; // Degrees, north positive, NAN if empty
    double longitude; // Degrees, east positive, NAN if empty
    double altitude; // Metres above mean sea level, NAN if empty
    double geoid_separation; // Metres, NAN if empty
    int fix_quality; // 0 none, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float, ...
    int satellites;
    double hdop; // NAN if empty
    double correction_age; // Seconds, NAN if empty
    int station_id; // -1 if empty
} nmea_gga_t;

typedef struct nmea_gst {
    double sigma_latitude; // Metres, 1-sigma, NAN if empty
    double sigma_longitude;
    double sigma_altitude;
} nmea_gst_t;

typedef struct nmea_gsa {
    double pdop; // NAN if empty
    double hdop;
    double vdop;
} nmea_gsa_t;

int nmea_asprintf(char **strp, const char *fmt, ...);
int nmea_vasprintf(char **strp, const char *fmt, va_list args);

bool nmea_parse_gga(const char *sentence, nmea_gga_t *gga);
bool nmea_parse_gst(const char *sentence, nmea_gst_t *gst);
bool nmea_parse_gsa(const char *sentence, nmea_gsa_t *gsa);

#endif //ESP32_XBEE_NMEA_H
