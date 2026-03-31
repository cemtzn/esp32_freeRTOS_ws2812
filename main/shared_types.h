#pragma once
#include <stdint.h>
#include <stdbool.h>

// ── LED Efektler ───────────────────────────────────
typedef enum {
    LED_CMD_SOLID,      // 0
    LED_CMD_FADE,       // 1
    LED_CMD_RAINBOW,    // 2
    LED_CMD_OFF,        // 3
    LED_CMD_CHASE,      // 4
    LED_CMD_FLASH,      // 5
    LED_CMD_RANDOM,     // 6
    LED_CMD_COMET,      // 7 - parlak baş + solan kuyruk
    LED_CMD_WAVE,       // 8 - sinüs dalgası dikey kayar
    LED_CMD_TWINKLE,    // 9 - rastgele yıldız parıltısı
    LED_CMD_FIRE,       // 10 - ateş simülasyonu alttan yukarı
    LED_CMD_BOUNCE,     // 11 - zıplayan halka
    LED_CMD_HEART,      // 12 - kalp, scroll + pulse
    LED_CMD_LIGHTNING,  // 13 - yıldırım, scroll
    LED_CMD_RAIN,       // 14 - yagmur damlaları
    LED_CMD_DNA,        // 15 - DNA sarmalı
    LED_CMD_STAR,       // 16 - kayan yıldız
} LedEffect_t;

typedef struct {
    LedEffect_t effect;
    uint8_t  r, g, b;
    uint8_t  brightness;
    uint8_t  speed;
    uint8_t  r2, g2, b2;
    uint8_t  r3, g3, b3;
    uint8_t  color_count;
} LedCommand_t;

// ── LED Config ─────────────────────────────────────
typedef struct {
    uint16_t rows;
    uint16_t cols;
    uint8_t  gpio;
    uint8_t  led_model; 
} LedConfig_t;

// ── Kullanıcı Profili ──────────────────────────────
typedef struct {
    char         name[16];
    LedCommand_t cmd;
    bool         used;
} LedProfile_t;

// ── Event Group Bitleri ────────────────────────────
#define RECONFIG_BIT    (1 << 0)
#define WIFI_CONN_BIT   (1 << 1)

// ── Log ────────────────────────────────────────────
typedef enum {
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} LogLevel_t;

typedef struct {
    LogLevel_t level;
    char tag[16];
    char message[96];
} LogEntry_t;