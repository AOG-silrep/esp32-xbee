#ifndef ESP32_XBEE_WIFI_H
#define ESP32_XBEE_WIFI_H

#include <esp_wifi.h>

typedef struct wifi_ap_status {
    bool active;
    bool auto_disabled;

    char ssid[33];
    wifi_auth_mode_t authmode;
    uint8_t devices;

    esp_ip4_addr_t ip4_addr;
    esp_ip6_addr_t ip6_addr;
} wifi_ap_status_t;

typedef enum wifi_internet {
    WIFI_INTERNET_CHECKING,
    WIFI_INTERNET_OK,
    WIFI_INTERNET_NO_DNS,
    WIFI_INTERNET_NO_REPLY, // NTRIP caster did not answer ping
    WIFI_INTERNET_UNREACHABLE,
    WIFI_INTERNET_PORTAL // Web check answered with something other than 204, e.g. a login page
} wifi_internet_t;

typedef struct wifi_sta_status {
    bool active;
    bool connected;

    char ssid[33];
    wifi_auth_mode_t authmode;
    int8_t rssi;

    esp_ip4_addr_t ip4_addr;
    esp_ip6_addr_t ip6_addr;

    wifi_internet_t internet;
    int internet_ms; // -1 when unknown
    char internet_host[72];
} wifi_sta_status_t;

void net_init();
void wifi_init();

wifi_ap_record_t * wifi_scan(uint16_t *number);

wifi_sta_list_t *wifi_ap_sta_list();

void wifi_ap_status(wifi_ap_status_t *status);
void wifi_sta_status(wifi_sta_status_t *status);

void wifi_internet_set(wifi_internet_t state, int ms, const char *host);

void wait_for_ip();
void wait_for_network();

const char *esp_netif_name(esp_netif_t *esp_netif);
const char * wifi_auth_mode_name(wifi_auth_mode_t auth_mode);
const char *wifi_internet_name(wifi_internet_t internet);

#endif //ESP32_XBEE_WIFI_H
