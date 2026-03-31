#include "user_handler.h"
#include "log_handler.h"
#include "def.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "string.h"
#include "stdio.h"

static const char *TAG = "USER";

// Namespace'ler profil başına sabit
static const char *s_prof_ns[MAX_USERS] = {"u0profs", "u1profs", "u2profs"};

// ── Kullanıcı işlemleri ────────────────────────────

int user_find(const char *username, const char *password)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_USERS, NVS_READONLY, &h) != ESP_OK) return -1;

    char key[16];
    char stored_name[17] = {0};
    char stored_pass[17] = {0};
    size_t len;
    int result = -1;

    for (int i = 0; i < MAX_USERS; i++) {
        snprintf(key, sizeof(key), "u%dname", i);
        len = sizeof(stored_name);
        if (nvs_get_str(h, key, stored_name, &len) != ESP_OK) continue;
        if (stored_name[0] == '\0') continue; // silinmiş slot

        if (strcmp(stored_name, username) != 0) continue;

        // Kullanıcı adı eşleşti, şifreyi kontrol et
        snprintf(key, sizeof(key), "u%dpass", i);
        len = sizeof(stored_pass);
        if (nvs_get_str(h, key, stored_pass, &len) != ESP_OK) break;

        if (strcmp(stored_pass, password) == 0) result = i;
        break; // kullanıcı bulundu (doğru veya yanlış şifre)
    }

    nvs_close(h);
    return result;
}

int user_get_all(UserInfo_t *users, int max)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_USERS, NVS_READONLY, &h) != ESP_OK) return 0;

    char key[16];
    size_t len;
    int found = 0;

    for (int i = 0; i < MAX_USERS && found < max; i++) {
        snprintf(key, sizeof(key), "u%dname", i);
        len = sizeof(users[found].username);
        if (nvs_get_str(h, key, users[found].username, &len) == ESP_OK
            && users[found].username[0] != '\0') {
            users[found].uid = i;
            found++;
        }
    }

    nvs_close(h);
    return found;
}

esp_err_t user_create(const char *username, const char *password)
{
    if (!username || !password || strlen(username) == 0 || strlen(username) > 16) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS_USERS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;

    char key[16];
    char name[17] = {0};
    size_t len;
    int free_slot = -1;

    for (int i = 0; i < MAX_USERS; i++) {
        snprintf(key, sizeof(key), "u%dname", i);
        len = sizeof(name);
        bool has_user = (nvs_get_str(h, key, name, &len) == ESP_OK && name[0] != '\0');

        if (!has_user && free_slot < 0) free_slot = i;

        if (has_user && strcmp(name, username) == 0) {
            nvs_close(h);
            return ESP_ERR_INVALID_STATE; // kullanıcı adı zaten alınmış
        }
    }

    if (free_slot < 0) {
        nvs_close(h);
        return ESP_ERR_NO_MEM; // boş slot yok
    }

    snprintf(key, sizeof(key), "u%dname", free_slot);
    nvs_set_str(h, key, username);
    snprintf(key, sizeof(key), "u%dpass", free_slot);
    nvs_set_str(h, key, password);
    nvs_commit(h);
    nvs_close(h);

    LOG_INFO(TAG, "Kullanıcı oluşturuldu: %s (uid=%d)", username, free_slot);
    return ESP_OK;
}

esp_err_t user_delete(int uid)
{
    if (uid < 0 || uid >= MAX_USERS) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_USERS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;

    char key[16];
    snprintf(key, sizeof(key), "u%dname", uid);
    nvs_set_str(h, key, ""); // boş = silinmiş
    snprintf(key, sizeof(key), "u%dpass", uid);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);

    // Profilleri de sil
    nvs_handle_t ph;
    if (nvs_open(s_prof_ns[uid], NVS_READWRITE, &ph) == ESP_OK) {
        nvs_erase_all(ph);
        nvs_commit(ph);
        nvs_close(ph);
    }

    LOG_INFO(TAG, "Kullanıcı silindi: uid=%d", uid);
    return ESP_OK;
}

esp_err_t user_change_pass(int uid, const char *old_pass, const char *new_pass)
{
    if (uid < 0 || uid >= MAX_USERS) return ESP_ERR_INVALID_ARG;
    if (!old_pass || !new_pass || strlen(new_pass) < 4) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    if (nvs_open(NVS_NS_USERS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;

    char key[16];
    char stored[17] = {0};
    size_t len = sizeof(stored);

    // Eski şifreyi doğrula
    snprintf(key, sizeof(key), "u%dpass", uid);
    if (nvs_get_str(h, key, stored, &len) != ESP_OK) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }

    if (strcmp(stored, old_pass) != 0) {
        nvs_close(h);
        return ESP_ERR_INVALID_STATE; // yanlış şifre
    }

    // Yeni şifreyi yaz
    nvs_set_str(h, key, new_pass);
    nvs_commit(h);
    nvs_close(h);

    LOG_INFO("USER", "Sifre degistirildi: uid=%d", uid);
    return ESP_OK;
}

// ── Profil işlemleri ───────────────────────────────

bool profile_load(int uid, int slot, LedProfile_t *out)
{
    if (uid < 0 || uid >= MAX_USERS || slot < 0 || slot >= MAX_PROFILES || !out) return false;

    nvs_handle_t h;
    if (nvs_open(s_prof_ns[uid], NVS_READONLY, &h) != ESP_OK) return false;

    char key[16];
    snprintf(key, sizeof(key), "s%d", slot);
    size_t len = sizeof(LedProfile_t);
    bool ok = (nvs_get_blob(h, key, out, &len) == ESP_OK && out->used);
    nvs_close(h);
    return ok;
}

esp_err_t profile_save(int uid, int slot, LedProfile_t *prof)
{
    if (uid < 0 || uid >= MAX_USERS || slot < 0 || slot >= MAX_PROFILES || !prof) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    if (nvs_open(s_prof_ns[uid], NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;

    char key[16];
    snprintf(key, sizeof(key), "s%d", slot);
    prof->used = true;
    nvs_set_blob(h, key, prof, sizeof(LedProfile_t));
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t profile_delete(int uid, int slot)
{
    if (uid < 0 || uid >= MAX_USERS || slot < 0 || slot >= MAX_PROFILES) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    if (nvs_open(s_prof_ns[uid], NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;

    char key[16];
    snprintf(key, sizeof(key), "s%d", slot);
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    return ESP_OK;
}