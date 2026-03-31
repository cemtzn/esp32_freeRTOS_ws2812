#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "shared_types.h"

extern QueueHandle_t log_queue;

void log_send(LogLevel_t level, const char *tag, const char *fmt, ...);
void log_task(void *pvParameters);

// Variadic macro — printf gibi format desteği
#define LOG_INFO(tag, fmt, ...)  log_send(LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)  log_send(LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) log_send(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)