#include "log_handler.h"
#include "esp_log.h"
#include "string.h"
#include "stdio.h"
#include "stdarg.h"

QueueHandle_t log_queue;

void log_send(LogLevel_t level, const char *tag, const char *fmt, ...)
{
    LogEntry_t entry;
    entry.level = level;

    strncpy(entry.tag, tag, sizeof(entry.tag) - 1);
    entry.tag[sizeof(entry.tag) - 1] = '\0';

    // Format string'i işle — printf gibi
    va_list args;
    va_start(args, fmt);
    vsnprintf(entry.message, sizeof(entry.message), fmt, args);
    va_end(args);

    xQueueSend(log_queue, &entry, 0);
}

void log_task(void *pvParameters)
{
    ESP_LOGI("LOG", "Log task başladı");
    LogEntry_t entry;
    while (1) {
        if (xQueueReceive(log_queue, &entry, portMAX_DELAY) == pdTRUE) {
            switch (entry.level) {
                case LOG_LEVEL_INFO:
                    ESP_LOGI(entry.tag, "%s", entry.message);
                    break;
                case LOG_LEVEL_WARN:
                    ESP_LOGW(entry.tag, "%s", entry.message);
                    break;
                case LOG_LEVEL_ERROR:
                    ESP_LOGE(entry.tag, "%s", entry.message);
                    break;
            }
        }
    }
}