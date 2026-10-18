#ifndef ESP32_XBEE_GEODESY_H
#define ESP32_XBEE_GEODESY_H

// WGS84 conversions, latitude/longitude in degrees, heights and ECEF in metres
void geodesy_geodetic_to_ecef(double latitude, double longitude, double height, double ecef[3]);
void geodesy_ecef_to_geodetic(const double ecef[3], double *latitude, double *longitude, double *height);

// Position relative to origin in the origin's local east/north/up frame
void geodesy_ecef_to_enu(const double origin[3], const double ecef[3], double enu[3]);

#endif //ESP32_XBEE_GEODESY_H
