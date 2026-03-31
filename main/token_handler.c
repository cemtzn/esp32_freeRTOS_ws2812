#include "token_handler.h"
#include "log_handler.h"
#include "def.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "string.h"
#include "stdio.h"

static const char *TAG = "TOKEN";

// Hex karakterleri
static const char HEX[] = "0123456789abcdef";

esp_err_t token_generate(int uid, char *out_token)
{
    if (uid < 0 || uid >= MAX_USERS || !out_token) return ESP_ERR_INVALID_ARG;

    // 32 hex karakterlik rastgele token
    uint8_t rand_bytes[TOKEN_LEN / 2];
    esp_fill_random(rand_bytes, sizeof(rand_bytes));

    for (int i = 0; i < TOKEN_LEN / 2; i++) {
        out_token[i * 2]     = HEX[(rand_bytes[i] >> 4) & 0xF];
        out_token[i * 2 + 1] = HEX[rand_bytes[i] & 0xF];
    }
    out_token[TOKEN_LEN] = '\0';

    // NVS'e kaydet
    nvs_handle_t h;
    if (nvs_open(TOKEN_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;

    char key[4];
    snprintf(key, sizeof(key), "t%d", uid);
    nvs_set_str(h, key, out_token);
    nvs_commit(h);
    nvs_close(h);

    LOG_INFO(TAG, "Token uretildi: uid=%d", uid);
    return ESP_OK;
}

int token_verify(const char *token)
{
    if (!token || strlen(token) != TOKEN_LEN) return -1;

    nvs_handle_t h;
    if (nvs_open(TOKEN_NS, NVS_READONLY, &h) != ESP_OK) return -1;

    char key[4];
    char stored[TOKEN_LEN + 1];
    size_t len;
    int result = -1;

    for (int i = 0; i < MAX_USERS; i++) {
        snprintf(key, sizeof(key), "t%d", i);
        len = sizeof(stored);
        if (nvs_get_str(h, key, stored, &len) != ESP_OK) continue;
        if (strcmp(stored, token) == 0) {
            result = i;
            break;
        }
    }

    nvs_close(h);
    return result;
}

esp_err_t token_delete(int uid)
{
    if (uid < 0 || uid >= MAX_USERS) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(TOKEN_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;

    char key[4];
    snprintf(key, sizeof(key), "t%d", uid);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}