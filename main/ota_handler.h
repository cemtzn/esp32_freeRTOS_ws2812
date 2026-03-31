#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"

// WebSocket handler'larından çağrılır
void handle_ota_begin (httpd_req_t *req, cJSON *root);
void handle_ota_chunk (httpd_req_t *req, cJSON *root);
void handle_ota_end   (httpd_req_t *req, cJSON *root);
void handle_ota_abort (httpd_req_t *req, cJSON *root);