#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "def.h"
#include "shared_types.h"
#include "led_handler.h"
#include "log_handler.h"
#include "wifi_handler.h"
#include "http_handler.h"

static const char *TAG = "MAIN";

// ── app_main ───────────────────────────────────────
void app_main(void)
{
    // 0. // OTA'dan boot ediliyorsa geçerli say
    esp_ota_mark_app_valid_cancel_rollback();

    // 1. NVS init
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. Queue ve event group
    led_cmd_queue   = xQueueCreate(LED_QUEUE_SIZE, sizeof(LedCommand_t));
    log_queue       = xQueueCreate(LOG_QUEUE_SIZE, sizeof(LogEntry_t));
    led_event_group = xEventGroupCreate();
    configASSERT(led_cmd_queue);
    configASSERT(log_queue);
    configASSERT(led_event_group);

    // 3. Log
    LOG_INFO(TAG, "Sistem başladı — FW v%s", FIRMWARE_VERSION);

    // 4. WiFi AP — task'lardan önce başlat
    wifi_init_ap();

    // 5. HTTP server
    http_server_start();

    // 6. Task'lar
    xTaskCreate(log_task,  "log_task",  STACK_LOG,  NULL, PRIORITY_LOG,  NULL);
    // YENİ — CPU0'a pinle, IDLE1 (CPU1) rahat nefes alsın
    xTaskCreatePinnedToCore(led_task, "led_task", STACK_LED, NULL, PRIORITY_LED, NULL, 1);
    // xTaskCreate(test_task, "test_task", STACK_TEST, NULL, PRIORITY_TEST, NULL);
}

