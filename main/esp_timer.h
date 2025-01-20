#ifndef ESP_TIMER_H
#define ESP_TIMER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Returns microseconds since boot
static inline int64_t esp_timer_get_time(void) {
    return (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS * 1000);
}

#endif // ESP_TIMER_H
