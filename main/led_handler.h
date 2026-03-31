#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <math.h>
#include "shared_types.h"

extern QueueHandle_t      led_cmd_queue;
extern EventGroupHandle_t led_event_group;

// Config yönetimi
void        led_config_load(void);
void        led_config_save(LedConfig_t *cfg);
LedConfig_t led_config_get(void);

// Task
void led_task(void *pvParameters);