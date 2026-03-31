#include "wifi_handler.h"
#include "log_handler.h"
#include "def.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "WIFI";

// ── Event handler ──────────────────────────────────

static void wifi_event_handler(void *arg,
                                esp_event_base_t event_base,
                                int32_t event_id,
                                void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *e =
                    (wifi_event_ap_staconnected_t *)event_data;
                LOG_INFO(TAG, "Cihaz baglandi — AID: %d", e->aid);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *e =
                    (wifi_event_ap_stadisconnected_t *)event_data;
                LOG_INFO(TAG, "Cihaz ayrildi — AID: %d", e->aid);
                break;
            }
            default:
                break;
        }
    }
}

// ── wifi_init_ap ───────────────────────────────────

esp_err_t wifi_init_ap(void)
{
    // 1. TCP/IP stack başlat — bir kez çağrılmalı
    ESP_ERROR_CHECK(esp_netif_init());

    // 2. Default event loop oluştur — WiFi olayları buradan geçer
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3. AP için default network interface oluştur
    esp_netif_create_default_wifi_ap();

    // 4. WiFi driver'ı default config ile başlat
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 5. Event handler'ı kaydet
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,         // hangi event base
        ESP_EVENT_ANY_ID,   // tüm WiFi olayları
        &wifi_event_handler,
        NULL,
        NULL));

    // 6. AP konfigürasyonu
    wifi_config_t wifi_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .password       = WIFI_AP_PASS,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .channel        = 1,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    LOG_INFO(TAG, "AP başladı — SSID: " WIFI_AP_SSID);
    LOG_INFO(TAG, "IP: 192.168.4.1");

    return ESP_OK;
}