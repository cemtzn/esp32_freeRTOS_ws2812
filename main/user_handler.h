#pragma once

#include "esp_err.h"
#include "shared_types.h"
#include <stdbool.h>

typedef struct {
    int  uid;
    char username[17];
} UserInfo_t;

// ── Kullanıcı CRUD ─────────────────────────────────
int       user_find(const char *username, const char *password);
int       user_get_all(UserInfo_t *users, int max);
esp_err_t user_create(const char *username, const char *password);
esp_err_t user_delete(int uid);
esp_err_t user_change_pass(int uid, const char *old_pass, const char *new_pass);

// ── Profil CRUD ────────────────────────────────────
bool      profile_load(int uid, int slot, LedProfile_t *out);
esp_err_t profile_save(int uid, int slot, LedProfile_t *prof);
esp_err_t profile_delete(int uid, int slot);
