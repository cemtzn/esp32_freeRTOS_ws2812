#pragma once

#include "esp_err.h"
#include "led_handler.h"
#include "log_handler.h"
#include "shared_types.h"
#include "def.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "string.h"
#include "stdlib.h"

esp_err_t http_server_start(void);