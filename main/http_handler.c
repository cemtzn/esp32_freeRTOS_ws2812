#include "http_handler.h"
#include "led_handler.h"
#include "log_handler.h"
#include "user_handler.h"
#include "token_handler.h"
#include "ota_handler.h"
#include "shared_types.h"
#include "def.h"

#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "string.h"
#include "stdlib.h"

static const char *TAG = "HTTP";

// ── SPIFFS ─────────────────────────────────────────

static void spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        LOG_ERROR(TAG, "SPIFFS mount hatasi: %s", esp_err_to_name(err));
    } else {
        LOG_INFO(TAG, "SPIFFS mount tamam");
    }
}

// ── Dosya sunucu ───────────────────────────────────

static esp_err_t serve_file(httpd_req_t *req, const char *path, const char *ct)
{
    FILE *f = fopen(path, "r");
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
    httpd_resp_set_type(req, ct);
    static char buf[4096];  // ← static olunca stack değil, .bss segmentinde
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{ return serve_file(req, "/spiffs/index.html", "text/html; charset=utf-8"); }

static esp_err_t css_get_handler(httpd_req_t *req)
{ return serve_file(req, "/spiffs/style.css", "text/css"); }

static esp_err_t js_get_handler(httpd_req_t *req)
{ return serve_file(req, "/spiffs/app.js", "application/javascript"); }

// ── WS yardımcılar ─────────────────────────────────

static void ws_send_str(httpd_req_t *req, const char *str)
{
    httpd_ws_frame_t f = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)str,
        .len     = strlen(str),
    };
    httpd_ws_send_frame(req, &f);
}

static void ws_send_cjson(httpd_req_t *req, cJSON *obj)
{
    char *str = cJSON_PrintUnformatted(obj);
    if (str) { ws_send_str(req, str); free(str); }
    cJSON_Delete(obj);
}

// ── Admin şifre doğrulama ──────────────────────────

static bool verify_admin_pass(const char *pass)
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

// ── Admin token üret ───────────────────────────────

static void generate_admin_token(char *out)
{
    uint8_t rb[TOKEN_LEN / 2];
    const char hx[] = "0123456789abcdef";
    esp_fill_random(rb, sizeof(rb));
    for (int i = 0; i < TOKEN_LEN / 2; i++) {
        out[i * 2]     = hx[(rb[i] >> 4) & 0xF];
        out[i * 2 + 1] = hx[rb[i] & 0xF];
    }
    out[TOKEN_LEN] = '\0';

    nvs_handle_t h;
    if (nvs_open(TOKEN_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ta", out);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ── Profil JSON yardımcısı ─────────────────────────

static cJSON *build_profile_json(int uid)
{
    cJSON *arr = cJSON_CreateArray();
    for (int s = 0; s < MAX_PROFILES; s++) {
        LedProfile_t prof;
        cJSON *pobj = cJSON_CreateObject();
        cJSON_AddNumberToObject(pobj, "slot", s);
        if (profile_load(uid, s, &prof) && prof.used) {
            cJSON_AddBoolToObject(pobj,   "used",        true);
            cJSON_AddStringToObject(pobj, "name",        prof.name);
            cJSON_AddNumberToObject(pobj, "effect",      prof.cmd.effect);
            cJSON_AddNumberToObject(pobj, "r",           prof.cmd.r);
            cJSON_AddNumberToObject(pobj, "g",           prof.cmd.g);
            cJSON_AddNumberToObject(pobj, "b",           prof.cmd.b);
            cJSON_AddNumberToObject(pobj, "brightness",  prof.cmd.brightness);
            cJSON_AddNumberToObject(pobj, "speed",       prof.cmd.speed);
            cJSON_AddNumberToObject(pobj, "color_count", prof.cmd.color_count);
            cJSON_AddNumberToObject(pobj, "r2",          prof.cmd.r2);
            cJSON_AddNumberToObject(pobj, "g2",          prof.cmd.g2);
            cJSON_AddNumberToObject(pobj, "b2",          prof.cmd.b2);
            cJSON_AddNumberToObject(pobj, "r3",          prof.cmd.r3);
            cJSON_AddNumberToObject(pobj, "g3",          prof.cmd.g3);
            cJSON_AddNumberToObject(pobj, "b3",          prof.cmd.b3);
        } else {
            cJSON_AddBoolToObject(pobj, "used", false);
        }
        cJSON_AddItemToArray(arr, pobj);
    }
    return arr;
}

// ══════════════════════════════════════════════════
// ── Handler'lar ────────────────────────────────────
// ══════════════════════════════════════════════════

static void handle_login(httpd_req_t *req, cJSON *root)
{
    cJSON *un = cJSON_GetObjectItem(root, "username");
    cJSON *pw = cJSON_GetObjectItem(root, "password");

    if (!un || !pw || !un->valuestring || !pw->valuestring) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"missing fields\"}");
        return;
    }

    if (strcmp(un->valuestring, "admin") == 0) {
        if (!verify_admin_pass(pw->valuestring)) {
            ws_send_str(req, "{\"status\":\"error\",\"msg\":\"wrong password\"}");
            LOG_WARN(TAG, "Admin sifre hatali");
            return;
        }
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "role",   "admin");
        cJSON_AddNumberToObject(resp, "uid",    -1);
        cJSON_AddStringToObject(resp, "fw",     FIRMWARE_VERSION);

        char token[TOKEN_LEN + 1];
        generate_admin_token(token);
        cJSON_AddStringToObject(resp, "token", token);

        ws_send_cjson(req, resp);
        LOG_INFO(TAG, "Admin girisi");
        return;
    }

    int uid = user_find(un->valuestring, pw->valuestring);
    if (uid < 0) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"wrong password\"}");
        LOG_WARN(TAG, "Hatali giris: %s", un->valuestring);
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status",   "ok");
    cJSON_AddStringToObject(resp, "role",     "user");
    cJSON_AddNumberToObject(resp, "uid",      uid);
    cJSON_AddStringToObject(resp, "username", un->valuestring);
    cJSON_AddItemToObject(resp, "profiles",   build_profile_json(uid));

    char token[TOKEN_LEN + 1];
    if (token_generate(uid, token) == ESP_OK)
        cJSON_AddStringToObject(resp, "token", token);

    ws_send_cjson(req, resp);
    LOG_INFO(TAG, "Kullanici girisi: %s (uid=%d)", un->valuestring, uid);
}

static void handle_token_login(httpd_req_t *req, cJSON *root)
{
    cJSON *tok = cJSON_GetObjectItem(root, "token");
    if (!tok || !tok->valuestring) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"no token\"}");
        return;
    }

    // Admin token kontrolü
    nvs_handle_t h;
    char stored[TOKEN_LEN + 1] = {0};
    size_t len = sizeof(stored);
    bool is_admin = false;
    if (nvs_open(TOKEN_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "ta", stored, &len) == ESP_OK)
            if (strcmp(stored, tok->valuestring) == 0) is_admin = true;
        nvs_close(h);
    }

    if (is_admin) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "role",   "admin");
        cJSON_AddNumberToObject(resp, "uid",    -1);
        cJSON_AddStringToObject(resp, "fw",     FIRMWARE_VERSION);
        ws_send_cjson(req, resp);
        LOG_INFO(TAG, "Admin token ile giris");
        return;
    }

    int uid = token_verify(tok->valuestring);
    if (uid < 0) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"invalid token\"}");
        return;
    }

    char uname[17] = {0};
    nvs_handle_t uh;
    if (nvs_open(NVS_NS_USERS, NVS_READONLY, &uh) == ESP_OK) {
        char key[16];
        snprintf(key, sizeof(key), "u%dname", uid);
        size_t ulen = sizeof(uname);
        nvs_get_str(uh, key, uname, &ulen);
        nvs_close(uh);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status",   "ok");
    cJSON_AddStringToObject(resp, "role",     "user");
    cJSON_AddNumberToObject(resp, "uid",      uid);
    cJSON_AddStringToObject(resp, "username", uname);
    cJSON_AddItemToObject(resp, "profiles",   build_profile_json(uid));
    ws_send_cjson(req, resp);
    LOG_INFO(TAG, "Token ile giris: uid=%d", uid);
}

static void handle_logout(httpd_req_t *req, cJSON *root)
{
    cJSON *uid_item = cJSON_GetObjectItem(root, "uid");
    if (uid_item && uid_item->valueint >= 0) {
        token_delete(uid_item->valueint);
    } else {
        nvs_handle_t h;
        if (nvs_open(TOKEN_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, "ta");
            nvs_commit(h);
            nvs_close(h);
        }
    }
    ws_send_str(req, "{\"status\":\"ok\"}");
    LOG_INFO(TAG, "Cikis yapildi");
}

static void handle_led_cmd(httpd_req_t *req, cJSON *root)
{
    LedCommand_t cmd = {
        .speed = LED_DEFAULT_SPEED, .brightness = 50, .color_count = 1,
    };

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "effect")))      cmd.effect      = (LedEffect_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "r")))           cmd.r           = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "g")))           cmd.g           = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "b")))           cmd.b           = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "brightness")))  cmd.brightness  = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "speed")))       cmd.speed       = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "color_count"))) cmd.color_count = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "r2")))          cmd.r2          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "g2")))          cmd.g2          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "b2")))          cmd.b2          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "r3")))          cmd.r3          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "g3")))          cmd.g3          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "b3")))          cmd.b3          = (uint8_t)item->valueint;

    if (cmd.effect > LED_CMD_STAR) cmd.effect = LED_CMD_OFF;

    if (xQueueSend(led_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE)
        LOG_WARN(TAG, "LED queue dolu");

    ws_send_str(req, "{\"status\":\"ok\"}");
}

static void handle_test(httpd_req_t *req, cJSON *root)
{
    cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    if (!mode_item || !mode_item->valuestring) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"mode required\"}");
        return;
    }

    const char *mode = mode_item->valuestring;
    LedCommand_t cmd = {
        .brightness = 100, .speed = 3, .color_count = 1,
        .r = 255, .g = 255, .b = 255,
    };

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "r")))     cmd.r     = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "g")))     cmd.g     = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "b")))     cmd.b     = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "speed"))) cmd.speed = (uint8_t)item->valueint;

    if      (strcmp(mode, "all_white") == 0) { cmd.effect=LED_CMD_SOLID; cmd.r=255; cmd.g=255; cmd.b=255; }
    else if (strcmp(mode, "all_off")   == 0) { cmd.effect=LED_CMD_OFF; }
    else if (strcmp(mode, "chase")     == 0) { cmd.effect=LED_CMD_CHASE; }
    else if (strcmp(mode, "flash")     == 0) { cmd.effect=LED_CMD_FLASH; }
    else if (strcmp(mode, "random")    == 0) { cmd.effect=LED_CMD_RANDOM; }
    else { ws_send_str(req, "{\"status\":\"error\",\"msg\":\"unknown mode\"}"); return; }

    xQueueSend(led_cmd_queue, &cmd, pdMS_TO_TICKS(100));
    ws_send_str(req, "{\"status\":\"ok\"}");
    LOG_INFO(TAG, "Test modu: %s", mode);
}

static void handle_config(httpd_req_t *req, cJSON *root)
{
    cJSON *pass_item = cJSON_GetObjectItem(root, "password");
    if (!pass_item || !pass_item->valuestring ||
        !verify_admin_pass(pass_item->valuestring)) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"wrong password\"}");
        return;
    }

    cJSON *item;
    LedConfig_t cfg = led_config_get();
    if ((item = cJSON_GetObjectItem(root, "rows")))      cfg.rows      = (uint16_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "cols")))      cfg.cols      = (uint16_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "gpio")))      cfg.gpio      = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "led_model"))) cfg.led_model = (uint8_t)item->valueint;

    led_config_save(&cfg);
    xEventGroupSetBits(led_event_group, RECONFIG_BIT);
    ws_send_str(req, "{\"status\":\"ok\",\"msg\":\"config saved\"}");
    LOG_INFO(TAG, "Config guncellendi — %dx%d gpio:%d model:%d",
             cfg.rows, cfg.cols, cfg.gpio, cfg.led_model);
}

static void handle_save_profile(httpd_req_t *req, cJSON *root)
{
    cJSON *uid_item  = cJSON_GetObjectItem(root, "uid");
    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");
    cJSON *name_item = cJSON_GetObjectItem(root, "name");

    if (!uid_item || !slot_item || !name_item) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"missing fields\"}");
        return;
    }

    LedProfile_t prof = {0};
    strncpy(prof.name, name_item->valuestring, sizeof(prof.name) - 1);

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "effect")))      prof.cmd.effect      = (LedEffect_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "r")))           prof.cmd.r           = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "g")))           prof.cmd.g           = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "b")))           prof.cmd.b           = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "brightness")))  prof.cmd.brightness  = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "speed")))       prof.cmd.speed       = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "color_count"))) prof.cmd.color_count = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "r2")))          prof.cmd.r2          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "g2")))          prof.cmd.g2          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "b2")))          prof.cmd.b2          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "r3")))          prof.cmd.r3          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "g3")))          prof.cmd.g3          = (uint8_t)item->valueint;
    if ((item = cJSON_GetObjectItem(root, "b3")))          prof.cmd.b3          = (uint8_t)item->valueint;

    int uid  = uid_item->valueint;
    int slot = slot_item->valueint;

    if (profile_save(uid, slot, &prof) != ESP_OK) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"save failed\"}");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "type",   "profiles_update");
    cJSON_AddItemToObject(resp, "profiles", build_profile_json(uid));
    ws_send_cjson(req, resp);
}

static void handle_delete_profile(httpd_req_t *req, cJSON *root)
{
    cJSON *uid_item  = cJSON_GetObjectItem(root, "uid");
    cJSON *slot_item = cJSON_GetObjectItem(root, "slot");

    if (!uid_item || !slot_item) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"missing fields\"}");
        return;
    }

    int uid  = uid_item->valueint;
    int slot = slot_item->valueint;
    profile_delete(uid, slot);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "type",   "profiles_update");
    cJSON_AddItemToObject(resp, "profiles", build_profile_json(uid));
    ws_send_cjson(req, resp);
}

static void handle_get_users(httpd_req_t *req, cJSON *root)
{
    cJSON *pass = cJSON_GetObjectItem(root, "admin_pass");
    if (!pass || !pass->valuestring || !verify_admin_pass(pass->valuestring)) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"unauthorized\"}");
        return;
    }

    UserInfo_t users[MAX_USERS];
    int count = user_get_all(users, MAX_USERS);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "type",   "users_list");
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "uid",      users[i].uid);
        cJSON_AddStringToObject(u, "username", users[i].username);
        cJSON_AddItemToArray(arr, u);
    }
    cJSON_AddItemToObject(resp, "users", arr);
    cJSON_AddNumberToObject(resp, "max", MAX_USERS);
    ws_send_cjson(req, resp);
}

static void handle_create_user(httpd_req_t *req, cJSON *root)
{
    cJSON *pass = cJSON_GetObjectItem(root, "admin_pass");
    if (!pass || !pass->valuestring || !verify_admin_pass(pass->valuestring)) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"unauthorized\"}");
        return;
    }

    cJSON *un = cJSON_GetObjectItem(root, "username");
    cJSON *pw = cJSON_GetObjectItem(root, "password");

    if (!un || !pw || !un->valuestring || !pw->valuestring) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"missing fields\"}");
        return;
    }

    esp_err_t err = user_create(un->valuestring, pw->valuestring);
    if      (err == ESP_ERR_INVALID_STATE) ws_send_str(req, "{\"status\":\"error\",\"msg\":\"username taken\"}");
    else if (err == ESP_ERR_NO_MEM)        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"max users reached\"}");
    else if (err != ESP_OK)                ws_send_str(req, "{\"status\":\"error\",\"msg\":\"create failed\"}");
    else handle_get_users(req, root);
}

static void handle_delete_user(httpd_req_t *req, cJSON *root)
{
    cJSON *pass = cJSON_GetObjectItem(root, "admin_pass");
    if (!pass || !pass->valuestring || !verify_admin_pass(pass->valuestring)) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"unauthorized\"}");
        return;
    }

    cJSON *uid_item = cJSON_GetObjectItem(root, "uid");
    if (!uid_item) { ws_send_str(req, "{\"status\":\"error\",\"msg\":\"uid required\"}"); return; }

    user_delete(uid_item->valueint);
    handle_get_users(req, root);
}

static void handle_change_pass(httpd_req_t *req, cJSON *root)
{
    cJSON *uid_item = cJSON_GetObjectItem(root, "uid");
    cJSON *old_item = cJSON_GetObjectItem(root, "old_pass");
    cJSON *new_item = cJSON_GetObjectItem(root, "new_pass");

    if (!uid_item || !old_item || !new_item ||
        !old_item->valuestring || !new_item->valuestring) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"missing fields\"}");
        return;
    }

    if (strlen(new_item->valuestring) < 4) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"pass too short\"}");
        return;
    }

    esp_err_t err = user_change_pass(uid_item->valueint,
                                     old_item->valuestring,
                                     new_item->valuestring);
    if      (err == ESP_ERR_INVALID_STATE) ws_send_str(req, "{\"status\":\"error\",\"msg\":\"wrong password\"}");
    else if (err != ESP_OK)                ws_send_str(req, "{\"status\":\"error\",\"msg\":\"change failed\"}");
    else                                   ws_send_str(req, "{\"status\":\"ok\",\"msg\":\"pass_changed\"}");
}

static void handle_change_admin_pass(httpd_req_t *req, cJSON *root)
{
    cJSON *old_item = cJSON_GetObjectItem(root, "old_pass");
    cJSON *new_item = cJSON_GetObjectItem(root, "new_pass");

    if (!old_item || !new_item || !old_item->valuestring || !new_item->valuestring) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"missing fields\"}");
        return;
    }

    if (strlen(new_item->valuestring) < 4) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"pass too short\"}");
        return;
    }

    if (!verify_admin_pass(old_item->valuestring)) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"wrong password\"}");
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"nvs error\"}");
        return;
    }
    nvs_set_str(h, NVS_KEY_ADMIN_PASS, new_item->valuestring);
    nvs_commit(h);
    nvs_close(h);
    ws_send_str(req, "{\"status\":\"ok\",\"msg\":\"admin_pass_changed\"}");
    LOG_INFO(TAG, "Admin sifresi degistirildi");
}

// ── WebSocket ana handler ──────────────────────────

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        LOG_INFO(TAG, "WebSocket baglantisi kuruldu");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK || frame.len == 0) return err;

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    frame.payload = buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) { free(buf); return err; }

    cJSON *root = cJSON_Parse((char *)buf);
    free(buf);

    if (!root) {
        ws_send_str(req, "{\"status\":\"error\",\"msg\":\"parse failed\"}");
        return ESP_OK;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    const char *type = (type_item && type_item->valuestring)
                       ? type_item->valuestring : "led_cmd";

    if      (strcmp(type, "login")             == 0) handle_login(req, root);
    else if (strcmp(type, "token_login")       == 0) handle_token_login(req, root);
    else if (strcmp(type, "logout")            == 0) handle_logout(req, root);
    else if (strcmp(type, "led_cmd")           == 0) handle_led_cmd(req, root);
    else if (strcmp(type, "test")              == 0) handle_test(req, root);
    else if (strcmp(type, "config")            == 0) handle_config(req, root);
    else if (strcmp(type, "save_profile")      == 0) handle_save_profile(req, root);
    else if (strcmp(type, "delete_profile")    == 0) handle_delete_profile(req, root);
    else if (strcmp(type, "get_users")         == 0) handle_get_users(req, root);
    else if (strcmp(type, "create_user")       == 0) handle_create_user(req, root);
    else if (strcmp(type, "delete_user")       == 0) handle_delete_user(req, root);
    else if (strcmp(type, "change_pass")       == 0) handle_change_pass(req, root);
    else if (strcmp(type, "change_admin_pass") == 0) handle_change_admin_pass(req, root);
    else if (strcmp(type, "ota_begin")         == 0) handle_ota_begin(req, root);
    else if (strcmp(type, "ota_chunk")         == 0) handle_ota_chunk(req, root);
    else if (strcmp(type, "ota_end")           == 0) handle_ota_end(req, root);
    else if (strcmp(type, "ota_abort")         == 0) handle_ota_abort(req, root);
    else ws_send_str(req, "{\"status\":\"error\",\"msg\":\"unknown type\"}");

    cJSON_Delete(root);
    return ESP_OK;
}

// ── http_server_start ──────────────────────────────

esp_err_t http_server_start(void)
{
    spiffs_init();

    httpd_handle_t server = NULL;
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = HTTP_PORT;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) != ESP_OK) {
        LOG_ERROR(TAG, "HTTP server baslatılamadi");
        return ESP_FAIL;
    }

    httpd_uri_t root_uri = {.uri="/",          .method=HTTP_GET, .handler=root_get_handler};
    httpd_uri_t css_uri  = {.uri="/style.css",  .method=HTTP_GET, .handler=css_get_handler};
    httpd_uri_t js_uri   = {.uri="/app.js",     .method=HTTP_GET, .handler=js_get_handler};
    httpd_uri_t ws_uri   = {
        .uri="/ws", .method=HTTP_GET, .handler=ws_handler,
        .is_websocket=true, .handle_ws_control_frames=false,
    };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &css_uri);
    httpd_register_uri_handler(server, &js_uri);
    httpd_register_uri_handler(server, &ws_uri);

    LOG_INFO(TAG, "HTTP server basladi — port: %d", HTTP_PORT);
    return ESP_OK;
}