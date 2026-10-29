#ifndef ESP32_XBEE_TASKS_H
#define ESP32_XBEE_TASKS_H

#include <freertos/FreeRTOS.h>

#define TASK_PRIORITY_STATUS_LED 0
#define TASK_PRIORITY_RESET_BUTTON 0
#define TASK_PRIORITY_WIFI_STATUS 0
#define TASK_PRIORITY_STATS 0
#define TASK_PRIORITY_INTERFACE 5
#define TASK_PRIORITY_UART 10
// Highest priority FreeRTOS accepts, anything above fails an assert in xTaskCreate
#define TASK_PRIORITY_MAX (configMAX_PRIORITIES - 1)

#endif //ESP32_XBEE_TASKS_H
