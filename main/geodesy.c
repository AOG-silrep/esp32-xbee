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

#include "geodesy.h"

// Formulas follow RTKLIB rtkcmn.c pos2ecef/ecef2pos/xyz2enu
#define WGS84_A 6378137.0
#define WGS84_F (1.0 / 298.257223563)
#define WGS84_E2 (WGS84_F * (2.0 - WGS84_F))

#define DEG_TO_RAD (M_PI / 180.0)

void geodesy_geodetic_to_ecef(double latitude, double longitude, double height, double ecef[3]) {
    double sinp = sin(latitude * DEG_TO_RAD), cosp = cos(latitude * DEG_TO_RAD);
    double sinl = sin(longitude * DEG_TO_RAD), cosl = cos(longitude * DEG_TO_RAD);
    double v = WGS84_A / sqrt(1.0 - WGS84_E2 * sinp * sinp);

    ecef[0] = (v + height) * cosp * cosl;
    ecef[1] = (v + height) * cosp * sinl;
    ecef[2] = (v * (1.0 - WGS84_E2) + height) * sinp;
}

void geodesy_ecef_to_geodetic(const double ecef[3], double *latitude, double *longitude, double *height) {
    double r2 = ecef[0] * ecef[0] + ecef[1] * ecef[1];
    double z = ecef[2], zk = 0.0, v = WGS84_A;

    while (fabs(z - zk) >= 1e-4) {
        zk = z;
        double sinp = z / sqrt(r2 + z * z);
        v = WGS84_A / sqrt(1.0 - WGS84_E2 * sinp * sinp);
        z = ecef[2] + v * WGS84_E2 * sinp;
    }

    *latitude = (r2 > 1e-12 ? atan(z / sqrt(r2)) : (ecef[2] > 0.0 ? M_PI / 2.0 : -M_PI / 2.0)) / DEG_TO_RAD;
    *longitude = (r2 > 1e-12 ? atan2(ecef[1], ecef[0]) : 0.0) / DEG_TO_RAD;
    *height = sqrt(r2 + z * z) - v;
}

void geodesy_ecef_to_enu(const double origin[3], const double ecef[3], double enu[3]) {
    double latitude, longitude, height;
    geodesy_ecef_to_geodetic(origin, &latitude, &longitude, &height);

    double sinp = sin(latitude * DEG_TO_RAD), cosp = cos(latitude * DEG_TO_RAD);
    double sinl = sin(longitude * DEG_TO_RAD), cosl = cos(longitude * DEG_TO_RAD);

    double dx = ecef[0] - origin[0], dy = ecef[1] - origin[1], dz = ecef[2] - origin[2];

    enu[0] = -sinl * dx + cosl * dy;
    enu[1] = -sinp * cosl * dx - sinp * sinl * dy + cosp * dz;
    enu[2] = cosp * cosl * dx + cosp * sinl * dy + sinp * dz;
}
