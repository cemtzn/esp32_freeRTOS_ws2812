#pragma once


// ── LED varsayılan config (NVS'de kayıt yoksa) ────
#define LED_DEFAULT_ROWS       1
#define LED_DEFAULT_COLS       16
#define LED_DEFAULT_GPIO       18
#define LED_DEFAULT_MODEL      0   //0:WS2812 , 1=WA2811
#define NVS_KEY_GPIO           "gpio"
#define NVS_KEY_LED_MODEL      "led_model"

// ── RMT ────────────────────────────────────────────
#define RMT_RESOLUTION_HZ       20000000

// ── Speed → ms dönüşümü ────────────────────────────
#define SPEED_TO_FADE_MS(s)     ((6 - (s)) * 25)
#define SPEED_TO_RAINBOW_MS(s)  ((6 - (s)) * 8)
#define SPEED_TO_CHASE_MS(s)    ((6 - (s)) * 30)

// ── Efekt parametreleri ────────────────────────────
#define FADE_BRIGHTNESS_STEP    1
#define FADE_MIN_BRIGHTNESS     20
#define RAINBOW_HUE_STEP        2
#define RAINBOW_SPREAD          32

// ── Queue ──────────────────────────────────────────
#define LED_QUEUE_SIZE          5
#define LOG_QUEUE_SIZE          20

// ── Priority ───────────────────────────────────────
#define PRIORITY_LED            5
#define PRIORITY_TEST           3
#define PRIORITY_LOG            2
#define PRIORITY_WIFI           4
#define PRIORITY_HTTP           3

// ── Stack ──────────────────────────────────────────
#define STACK_LED               4096
#define STACK_TEST              2048
#define STACK_LOG               4096
#define STACK_WIFI              4096
#define STACK_HTTP              8192

// ── Test task ──────────────────────────────────────
#define TEST_STEP_MS            3000
#define LED_MIN_BRIGHTNESS      30
#define LED_DEFAULT_SPEED       2

// ── NVS — LED Config ───────────────────────────────
#define NVS_NAMESPACE           "led_cfg"
#define NVS_KEY_ROWS            "rows"
#define NVS_KEY_COLS            "cols"
#define NVS_KEY_LAST_CMD        "last_cmd"
#define NVS_KEY_ADMIN_PASS      "admin_pass"
#define ADMIN_DEFAULT_PASS      "admin1234"

// ── NVS — Kullanıcılar ─────────────────────────────
#define MAX_USERS               3
#define MAX_PROFILES            3
#define NVS_NS_USERS            "users"
// Kullanıcı key formatı: "u{n}name", "u{n}pass"  (n = 0,1,2)
// Profil namespace: "u0profs", "u1profs", "u2profs"
// Profil key formatı: "s0", "s1", "s2"

// ── WiFi AP ────────────────────────────────────────
#define WIFI_AP_SSID            "LED_Matrix"
#define WIFI_AP_PASS            "test1234"
#define WIFI_AP_MAX_CONN        4

// ── HTTP ───────────────────────────────────────────
#define HTTP_PORT               80

#define TOKEN_NS    "tokens"

// ── Version ────────────────────────────────────────
#define FIRMWARE_VERSION        "4.5.5" // new color effects added, last state saved, config persistence, user profiles, improved web UI