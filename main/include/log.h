#ifndef ESP32_XBEE_LOG_H
#define ESP32_XBEE_LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>

esp_err_t log_init();
int log_vprintf(const char * format, va_list arg);
size_t log_read(uint32_t since, char *out, size_t max, uint32_t *start, uint32_t *end, uint32_t *head, uint32_t *boot_id);

#endif //ESP32_XBEE_LOG_H
