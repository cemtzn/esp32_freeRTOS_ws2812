#include "ota_handler.h"
#include "log_handler.h"
#include "def.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "string.h"
#include "stdlib.h"

static const char *TAG = "OTA";

// ── Dosya kapsamlı state — dışarıya kapalı ─────────
static esp_ota_handle_t      s_ota_handle  = 0;
static const esp_partition_t *s_ota_part   = NULL;
static size_t                 s_ota_written = 0;
static size_t                 s_ota_total   = 0;

// ── Yardımcı: WS yanıt gönder ─────────────────────
static void ota_send(httpd_req_t *req, const char *json)
{
    httpd_ws_frame_t f = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };
    httpd_ws_send_frame(req, &f);
}

// ── Admin şifre doğrulama ──────────────────────────
// http_handler.c'deki verify_admin_pass ile aynı mantık
// NVS'den okur, default ile karşılaştırır
static bool ota_verify_pass(const char *pass)
{
    nvs_handle_t h;
    char stored[32] = ADMIN_DEFAULT_PASS;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(stored);
        nvs_get_str(h, NVS_KEY_ADMIN_PASS, stored, &len);
        nvs_close(h);
    }
    return strcmp(pass, stored) == 0;
}

// ── OTA Begin ──────────────────────────────────────
// Gelen: { type:"ota_begin", password:"...", size: 123456 }
// Giden: { status:"ok", msg:"ota_ready" }
void handle_ota_begin(httpd_req_t *req, cJSON *root)
{
    // Şifre kontrolü
    cJSON *pass = cJSON_GetObjectItem(root, "password");
    if (!pass || !pass->valuestring || !ota_verify_pass(pass->valuestring)) {
        ota_send(req, "{\"status\":\"error\",\"msg\":\"unauthorized\"}");
        LOG_WARN(TAG, "OTA: yetkisiz erisim");
        return;
    }

    // Dosya boyutu
    cJSON *size_item = cJSON_GetObjectItem(root, "size");
    if (!size_item || size_item->valueint <= 0) {
        ota_send(req, "{\"status\":\"error\",\"msg\":\"size required\"}");
        return;
    }

    // Önceki yarım kalan OTA varsa temizle
    if (s_ota_handle) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }

    s_ota_total   = (size_t)size_item->valueint;
    s_ota_written = 0;

    // Hedef partition — ota_1
    s_ota_part = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_part) {
        ota_send(req, "{\"status\":\"error\",\"msg\":\"no OTA partition found\"}");
        LOG_ERROR(TAG, "OTA partition bulunamadi");
        return;
    }

    esp_err_t err = esp_ota_begin(s_ota_part, s_ota_total, &s_ota_handle);
    if (err != ESP_OK) {
        s_ota_handle = 0;
        LOG_ERROR(TAG, "esp_ota_begin hatasi: %s", esp_err_to_name(err));
        ota_send(req, "{\"status\":\"error\",\"msg\":\"ota begin failed\"}");
        return;
    }

    LOG_INFO(TAG, "OTA basladi — hedef: %s, boyut: %d byte",
             s_ota_part->label, s_ota_total);
    ota_send(req, "{\"status\":\"ok\",\"msg\":\"ota_ready\"}");
}

// ── OTA Chunk ──────────────────────────────────────
// Gelen: { type:"ota_chunk", data:"base64..." }
// Giden: { status:"ok", msg:"chunk_ok", pct: 42 }
void handle_ota_chunk(httpd_req_t *req, cJSON *root)
{
    if (!s_ota_handle) {
        ota_send(req, "{\"status\":\"error\",\"msg\":\"ota not started\"}");
        return;
    }

    cJSON *data_item = cJSON_GetObjectItem(root, "data");
    if (!data_item || !data_item->valuestring) {
        ota_send(req, "{\"status\":\"error\",\"msg\":\"no data\"}");
        return;
    }

    const char *b64    = data_item->valuestring;
    size_t      b64_len = strlen(b64);

    // Base64 decode için buffer
    size_t   bin_max = (b64_len / 4) * 3 + 4;
    uint8_t *buf     = malloc(bin_max);
    if (!buf) {
        ota_send(req, "{\"status\":\"error\",\"msg\":\"out of memory\"}");
        return;
    }

    size_t out_len = 0;
    int ret = mbedtls_base64_decode(buf, bin_max, &out_len,
                                    (const unsigned char *)b64, b64_len);
    if (ret != 0) {
        free(buf);
        LOG_ERROR(TAG, "Base64 decode hatasi: %d", ret);
        ota_send(req, "{\"status\":\"error\",\"msg\":\"base64 decode failed\"}");
        return;
    }

    esp_err_t err = esp_ota_write(s_ota_handle, buf, out_len);
    free(buf);

    if (err != ESP_OK) {
        LOG_ERROR(TAG, "esp_ota_write hatasi: %s", esp_err_to_name(err));
        ota_send(req, "{\"status\":\"error\",\"msg\":\"write failed\"}");
        return;
    }

    s_ota_written += out_len;

    int pct = (s_ota_total > 0)
              ? (int)(s_ota_written * 100 / s_ota_total)
              : 0;

    char resp[80];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"msg\":\"chunk_ok\",\"pct\":%d}", pct);
    ota_send(req, resp);
}

// ── OTA End ────────────────────────────────────────
// Gelen: { type:"ota_end" }
// Giden: { status:"ok", msg:"ota_done" } → ardından restart
void handle_ota_end(httpd_req_t *req, cJSON *root)
{
    if (!s_ota_handle) {
        ota_send(req, "{\"status\":\"error\",\"msg\":\"ota not started\"}");
        return;
    }

    esp_err_t err = esp_ota_end(s_ota_handle);
    s_ota_handle = 0;

    if (err != ESP_OK) {
        LOG_ERROR(TAG, "esp_ota_end hatasi: %s", esp_err_to_name(err));
        ota_send(req, "{\"status\":\"error\",\"msg\":\"verification failed\"}");
        return;
    }

    err = esp_ota_set_boot_partition(s_ota_part);
    if (err != ESP_OK) {
        LOG_ERROR(TAG, "esp_ota_set_boot_partition hatasi: %s", esp_err_to_name(err));
        ota_send(req, "{\"status\":\"error\",\"msg\":\"set boot failed\"}");
        return;
    }

    LOG_INFO(TAG, "OTA tamamlandi — %d byte yazildi, yeniden baslatiliyor...",
             s_ota_written);

    ota_send(req, "{\"status\":\"ok\",\"msg\":\"ota_done\"}");

    // Mesajın gönderilmesi için kısa bekle
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

// ── OTA Abort ──────────────────────────────────────
// Gelen: { type:"ota_abort" }
// Giden: { status:"ok", msg:"aborted" }
void handle_ota_abort(httpd_req_t *req, cJSON *root)
{
    if (s_ota_handle) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle  = 0;
        s_ota_written = 0;
        s_ota_total   = 0;
        s_ota_part    = NULL;
    }
    LOG_WARN(TAG, "OTA iptal edildi");
    ota_send(req, "{\"status\":\"ok\",\"msg\":\"aborted\"}");
}