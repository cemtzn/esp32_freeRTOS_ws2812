#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define TOKEN_LEN       32
#define TOKEN_NS        "tokens"
// key formatı: "t0", "t1", "t2" (uid'e göre)

// Token üret ve NVS'e kaydet, token buffera yaz
esp_err_t token_generate(int uid, char *out_token);

// Token doğrula — uid döner, bulunamazsa -1
int       token_verify(const char *token);

// Token sil (logout)
esp_err_t token_delete(int uid);