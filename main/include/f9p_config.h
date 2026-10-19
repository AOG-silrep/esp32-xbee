#ifndef ESP32_XBEE_F9P_CONFIG_H
#define ESP32_XBEE_F9P_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

typedef enum {
    F9P_MODE_DUAL = 0, // Dual GPS moving base, RTCM out on UART2
    F9P_MODE_RS232 = 1 // NMEA out on UART2 for an RS-232 device
} f9p_mode_t;

typedef struct f9p_settings {
    f9p_mode_t mode;

    // RS-232 mode UART2 (and USB) output rates in Hz, 0 = off
    int gga_hz;
    int vtg_hz;
    int rmc_hz;
    int gsa_hz;

    // RS-232 mode UART2 baud rate
    int uart2_baud;
} f9p_settings_t;

// Highest RS-232 rate of each message, all of them together fit UART2 at 19200 baud
#define F9P_GGA_MAX_HZ 10
#define F9P_VTG_MAX_HZ 5
#define F9P_RMC_MAX_HZ 2
#define F9P_GSA_MAX_HZ 1

#define F9P_REJECTED_MAX 16

typedef struct f9p_result {
    int keys; // Keys the receiver accepted
    int total; // Keys in the config

    // Keys the receiver refused
    int rejected_count;
    uint32_t rejected[F9P_REJECTED_MAX];

    const char *error; // NULL if the whole config was written
} f9p_result_t;

// Panel settings as found in one layer of the receiver
typedef struct f9p_state {
    bool present; // False if the layer doesn't hold all of them, e.g. nothing saved to flash

    int mode; // F9P_MODE_DUAL, F9P_MODE_RS232 or -1 if UART2 output matches neither
    bool out_ubx;
    bool out_nmea;
    bool out_rtcm3;
    uint32_t uart2_baud;

    // Measurement rate, and UART2 output rates (0 = off)
    double nav_hz;
    double gga_hz;
    double vtg_hz;
    double rmc_hz;
    double gsa_hz;
} f9p_state_t;

#define F9P_DIFFERENCES_MAX 16

typedef struct f9p_difference {
    uint32_t key;
    uint64_t expected;
    uint64_t actual;
    bool missing; // Not in this layer of the receiver
} f9p_difference_t;

// Comparison of one layer of the receiver with what a write would set
typedef struct f9p_check {
    int total;
    int differ;
    int missing;

    // The first differences
    int count;
    f9p_difference_t keys[F9P_DIFFERENCES_MAX];
} f9p_check_t;

void f9p_config_init();

// Rates must divide the 10 Hz navigation rate and stay within their maximum, baud 19200 or 38400
bool f9p_settings_valid(const f9p_settings_t *settings);

// Writes the config to the receiver's RAM, BBR and flash over the UART, blocks until done
esp_err_t f9p_config_write(const f9p_settings_t *settings, f9p_result_t *result);

// Reads the panel settings from the receiver's RAM (active) and flash (saved)
esp_err_t f9p_config_read(f9p_state_t *ram, f9p_state_t *flash, const char **error);

// Compares every key in the receiver's RAM and flash with what writing these settings would set
esp_err_t f9p_config_check(const f9p_settings_t *settings, f9p_check_t *ram, f9p_check_t *flash, const char **error);

#endif //ESP32_XBEE_F9P_CONFIG_H
