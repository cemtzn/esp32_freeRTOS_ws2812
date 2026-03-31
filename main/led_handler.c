#include "led_handler.h"
#include "log_handler.h"
#include "shared_types.h"
#include "def.h"

#include "led_strip.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "math.h"
#include "string.h"
#include "stdlib.h"

static const char *TAG = "LED";

QueueHandle_t      led_cmd_queue;
EventGroupHandle_t led_event_group;

static LedConfig_t s_cfg = {
    .rows = LED_DEFAULT_ROWS,
    .cols = LED_DEFAULT_COLS,
};

// ── Config ─────────────────────────────────────────

void led_config_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        LOG_INFO(TAG, "NVS kayıt yok, varsayılan config kullanılıyor");
        return;
    }
    uint16_t rows, cols;
    if (nvs_get_u16(h, NVS_KEY_ROWS, &rows) == ESP_OK) s_cfg.rows = rows;
    if (nvs_get_u16(h, NVS_KEY_COLS, &cols) == ESP_OK) s_cfg.cols = cols;
    nvs_close(h);
    LOG_INFO(TAG, "Config yüklendi");
}

void led_config_save(LedConfig_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        LOG_ERROR(TAG, "NVS açılamadı");
        return;
    }
    nvs_set_u16(h, NVS_KEY_ROWS, cfg->rows);
    nvs_set_u16(h, NVS_KEY_COLS, cfg->cols);
    nvs_commit(h);
    nvs_close(h);
    s_cfg = *cfg;
    LOG_INFO(TAG, "Config kaydedildi");
}

LedConfig_t led_config_get(void) { return s_cfg; }

// ── Son state ──────────────────────────────────────

static void save_last_command(const LedCommand_t *cmd)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_LAST_CMD, cmd, sizeof(LedCommand_t));
    nvs_commit(h);
    nvs_close(h);
}

static bool load_last_command(LedCommand_t *cmd)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t size = sizeof(LedCommand_t);
    bool ok = (nvs_get_blob(h, NVS_KEY_LAST_CMD, cmd, &size) == ESP_OK);
    nvs_close(h);
    return ok;
}

// ── Koordinat sistemi ──────────────────────────────
// Yılan (serpentine) sarımı — silindir için
// x = sütun (açısal, 0..cols-1)
// y = satır (dikey, 0..rows-1)

static uint16_t xy_to_index(uint8_t x, uint8_t y, uint8_t cols)
{
    if (y % 2 == 0) {
        return (uint16_t)y * cols + x;              // çift satır: soldan sağa
    } else {
        return (uint16_t)y * cols + (cols - 1 - x); // tek satır: sağdan sola
    }
}

// ── LCG Pseudo-random ──────────────────────────────
static uint32_t s_rand_seed = 42731;
static uint8_t fast_rand(void)
{
    s_rand_seed = s_rand_seed * 1103515245 + 12345;
    return (uint8_t)((s_rand_seed >> 16) & 0xFF);
}

// ── Gamma düzeltme ─────────────────────────────────
static uint8_t scale(uint8_t value, uint8_t brightness)
{
    if (brightness == 0 || value == 0) return 0;
    float b = brightness / 100.0f;
    b = b * b; // gamma 2.0
    return (uint8_t)(value * b);
}

// ── WS2811/WS2812 renk sırası wrapper ─────────────
// WS2812 → GRB (led_strip kütüphanesi halleder)
// WS2811 → RGB sırası (R ve G swap gerekir)
static void set_pixel(led_strip_handle_t strip, int idx,
                      uint8_t r, uint8_t g, uint8_t b)
{
    if (s_cfg.led_model == 1) {
        led_strip_set_pixel(strip, idx, g, r, b);
    } else {
        led_strip_set_pixel(strip, idx, r, g, b);
    }
}

// ── Tüm LED'leri aynı renge ayarla ────────────────
static void set_all(led_strip_handle_t strip,
                    uint8_t r, uint8_t g, uint8_t b,
                    uint8_t brightness, uint16_t count)
{
    for (int i = 0; i < count; i++) {
        set_pixel(strip, i, scale(r, brightness), scale(g, brightness), scale(b, brightness));
    }
    led_strip_refresh(strip);
}

// ── Tek piksel ayarla (koordinat ile) ─────────────
static void set_xy(led_strip_handle_t strip,
                   uint8_t x, uint8_t y,
                   uint8_t r, uint8_t g, uint8_t b,
                   uint8_t cols)
{
    uint16_t idx = xy_to_index(x, y, cols);
    set_pixel(strip, idx, r, g, b);
}

// ── Reconfig kontrolü ──────────────────────────────
static bool reconfig_requested(void)
{
    return (xEventGroupGetBits(led_event_group) & RECONFIG_BIT) != 0;
}

// ── HSV → RGB ──────────────────────────────────────
static void hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) { *r = *g = *b = v; return; }
    uint8_t region    = h / 43;
    uint8_t remainder = (h - (region * 43)) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    switch (region) {
        case 0: *r=v; *g=t; *b=p; break;
        case 1: *r=q; *g=v; *b=p; break;
        case 2: *r=p; *g=v; *b=t; break;
        case 3: *r=p; *g=q; *b=v; break;
        case 4: *r=t; *g=p; *b=v; break;
        default:*r=v; *g=p; *b=q; break;
    }
}

// ══════════════════════════════════════════════════
// ── Mevcut Efektler ────────────────────────────────
// ══════════════════════════════════════════════════

static void effect_solid(led_strip_handle_t strip, LedCommand_t *cmd, uint16_t count)
{
    set_all(strip, cmd->r, cmd->g, cmd->b, cmd->brightness, count);
    while (1) {
        LedCommand_t next;
        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(100)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

static void effect_fade(led_strip_handle_t strip, LedCommand_t *cmd, uint16_t count)
{
    uint32_t step_ms = SPEED_TO_FADE_MS(cmd->speed);
    LedCommand_t next;
    for (int b = 100; b >= FADE_MIN_BRIGHTNESS; b--) {
        set_all(strip, cmd->r, cmd->g, cmd->b, b, count);
        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
    for (int b = FADE_MIN_BRIGHTNESS; b <= 100; b++) {
        set_all(strip, cmd->r, cmd->g, cmd->b, b, count);
        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

static void effect_rainbow(led_strip_handle_t strip, LedCommand_t *cmd, uint16_t count)
{
    uint32_t step_ms = SPEED_TO_RAINBOW_MS(cmd->speed);
    uint8_t  base_hue = 0;
    float    base_offset = 0.0f;
    LedCommand_t next;
    uint8_t rs[3] = {cmd->r,  cmd->r2, cmd->r3};
    uint8_t gs[3] = {cmd->g,  cmd->g2, cmd->g3};
    uint8_t bs[3] = {cmd->b,  cmd->b2, cmd->b3};
    uint8_t n = cmd->color_count < 1 ? 1 : cmd->color_count;
    while (1) {
        for (int i = 0; i < count; i++) {
            uint8_t r, g, b;
            if (n == 1) {
                uint8_t hue = base_hue + (uint8_t)((i * 256) / count);
                hsv_to_rgb(hue, 255, (uint8_t)(cmd->brightness * 255 / 100), &r, &g, &b);
            } else {
                float pos = fmodf((float)i / count * n + base_offset, n);
                int   idx = (int)pos % (n - 1);
                float frac = pos - (int)pos;
                int   idx2 = (idx + 1) % n;
                float bscale = cmd->brightness / 100.0f; bscale *= bscale;
                r = (uint8_t)((rs[idx]*(1-frac) + rs[idx2]*frac) * bscale);
                g = (uint8_t)((gs[idx]*(1-frac) + gs[idx2]*frac) * bscale);
                b = (uint8_t)((bs[idx]*(1-frac) + bs[idx2]*frac) * bscale);
            }
            led_strip_set_pixel(strip, i, r, g, b);
        }
        led_strip_refresh(strip);
        base_hue    += RAINBOW_HUE_STEP;
        base_offset += 0.05f;
        if (base_offset >= n) base_offset -= n;
        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

static void effect_chase(led_strip_handle_t strip, LedCommand_t *cmd, uint16_t count)
{
    if (count == 0) { xQueueReceive(led_cmd_queue, cmd, portMAX_DELAY); return; }
    uint32_t step_ms = SPEED_TO_CHASE_MS(cmd->speed);
    LedCommand_t next;
    int pos = 0;
    while (1) {
        for (int i = 0; i < count; i++) led_strip_set_pixel(strip, i, 0, 0, 0);
        led_strip_set_pixel(strip, pos, scale(cmd->r, cmd->brightness),
                            scale(cmd->g, cmd->brightness), scale(cmd->b, cmd->brightness));
        led_strip_refresh(strip);
        pos = (pos + 1) % count;
        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

static void effect_flash(led_strip_handle_t strip, LedCommand_t *cmd, uint16_t count)
{
    uint32_t step_ms = SPEED_TO_CHASE_MS(cmd->speed) / 2;
    if (step_ms < 40) step_ms = 40;
    LedCommand_t next;
    bool on = true;
    while (1) {
        if (on) set_all(strip, cmd->r, cmd->g, cmd->b, cmd->brightness, count);
        else {
            for (int i = 0; i < count; i++) led_strip_set_pixel(strip, i, 0, 0, 0);
            led_strip_refresh(strip);
        }
        on = !on;
        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

static void effect_random(led_strip_handle_t strip, LedCommand_t *cmd, uint16_t count)
{
    uint32_t step_ms = SPEED_TO_RAINBOW_MS(cmd->speed);
    LedCommand_t next;
    while (1) {
        for (int i = 0; i < count; i++) {
            uint8_t r = fast_rand(), g = fast_rand(), b = fast_rand();
            led_strip_set_pixel(strip, i, scale(r, cmd->brightness),
                                scale(g, cmd->brightness), scale(b, cmd->brightness));
        }
        led_strip_refresh(strip);
        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

// ══════════════════════════════════════════════════
// ── Silindir Animasyonları ─────────────────────────
// ══════════════════════════════════════════════════

// ── COMET — parlak baş + solan kuyruk, dikey kayar ─
// Kuyruk uzunluğu = satır sayısının yarısı
// Halka şeklinde kayar: baş her sütunda aynı anda
static void effect_comet(led_strip_handle_t strip, LedCommand_t *cmd,
                          uint8_t rows, uint8_t cols)
{
    uint32_t step_ms = SPEED_TO_CHASE_MS(cmd->speed);
    LedCommand_t next;
    int head = 0;
    int direction = 1;
    int tail_len = rows / 2;
    if (tail_len < 3) tail_len = 3;

    while (1) {
        // Tüm LEDleri söndür
        for (int i = 0; i < rows * cols; i++) led_strip_set_pixel(strip, i, 0, 0, 0);

        // Baş ve kuyruk çiz — tüm sütunlarda aynı anda (halka efekti)
        for (int t = 0; t < tail_len; t++) {
            int row = head - t * direction;
            if (row < 0 || row >= rows) continue;

            // Kuyruk sönüyor: başa yakın parlak, uzaklaştıkça sönük
            float fade  = (float)(tail_len - t) / tail_len;
            fade        = fade * fade; // gamma
            uint8_t br  = (uint8_t)(cmd->brightness * fade);

            for (int x = 0; x < cols; x++) {
                set_xy(strip, x, row,
                       scale(cmd->r, br),
                       scale(cmd->g, br),
                       scale(cmd->b, br), cols);
            }
        }
        led_strip_refresh(strip);

        // Başı ilerlet
        head += direction;
        if (head >= rows) { head = rows - 2; direction = -1; }
        if (head < 0)     { head = 1;        direction =  1; }

        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

// ── WAVE — sinüs dalgası parlaklığı dikey kayar ────
// Her sütun açısal offset alır → silindir boyunca döner gibi görünür
static void effect_wave(led_strip_handle_t strip, LedCommand_t *cmd,
                        uint8_t rows, uint8_t cols)
{
    uint32_t step_ms = SPEED_TO_RAINBOW_MS(cmd->speed);
    LedCommand_t next;
    float phase = 0.0f;

    while (1) {
        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                // Her sütun için açısal offset (silindir etkisi)
                float col_offset = (float)x / cols * 2.0f * 3.14159f;
                float wave_val   = sinf(phase + (float)y / rows * 2.0f * 3.14159f + col_offset);

                // -1..1 → 0..1
                float norm = (wave_val + 1.0f) / 2.0f;
                uint8_t br = (uint8_t)(cmd->brightness * norm);

                set_xy(strip, x, y,
                       scale(cmd->r, br),
                       scale(cmd->g, br),
                       scale(cmd->b, br), cols);
            }
        }
        led_strip_refresh(strip);
        phase += 0.25f;
        if (phase > 2.0f * 3.14159f) phase -= 2.0f * 3.14159f;

        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

// ── TWINKLE — rastgele yıldız parıltısı ────────────
// Her LED bağımsız parlar ve söner, yıldızlı gökyüzü gibi
static void effect_twinkle(led_strip_handle_t strip, LedCommand_t *cmd,
                            uint16_t count)
{
    uint32_t step_ms = SPEED_TO_RAINBOW_MS(cmd->speed);
    LedCommand_t next;

    // Her LED'in mevcut parlaklığı + yönü
    int16_t *brightness = calloc(count, sizeof(int16_t));
    int8_t  *delta      = calloc(count, sizeof(int8_t));
    if (!brightness || !delta) {
        if (brightness) free(brightness);
        if (delta) free(delta);
        xQueueReceive(led_cmd_queue, cmd, portMAX_DELAY);
        return;
    }

    // Başlangıçta rastgele dağıt
    for (int i = 0; i < count; i++) {
        brightness[i] = fast_rand() % 60;
        delta[i]      = (fast_rand() & 1) ? 3 : -3;
    }

    while (1) {
        for (int i = 0; i < count; i++) {
            brightness[i] += delta[i];

            if (brightness[i] >= cmd->brightness) {
                brightness[i] = cmd->brightness;
                delta[i] = -(2 + fast_rand() % 4); // sönme hızı
            } else if (brightness[i] <= 0) {
                brightness[i] = 0;
                // Kısa bekleme: bazı LED'ler sönük kalır
                if (fast_rand() < 60) {
                    delta[i] = 2 + fast_rand() % 5; // yanma hızı
                }
            }

            uint8_t br = (uint8_t)brightness[i];
            led_strip_set_pixel(strip, i,
                                scale(cmd->r, br),
                                scale(cmd->g, br),
                                scale(cmd->b, br));
        }
        led_strip_refresh(strip);

        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            free(brightness); free(delta);
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) {
            free(brightness); free(delta);
            cmd->effect = LED_CMD_OFF; return;
        }
    }
}

// ── FIRE — ateş simülasyonu alttan yukarı ──────────
// cmd->r/g/b = ateş rengi (kırmızı ateş için r=255,g=30,b=0)
// Palet: siyah → koyu renk → ana renk → turuncu/sarı (az beyaz)
// Beyaz bölge çok kısa, ağırlık ana renk + turuncu/sarıda
static void effect_fire(led_strip_handle_t strip, LedCommand_t *cmd,
                        uint8_t rows, uint8_t cols)
{
    uint32_t step_ms = 30 - (cmd->speed * 4);
    if (step_ms < 6) step_ms = 6;
    LedCommand_t next;

    uint8_t *heat = calloc((rows + 2) * cols, sizeof(uint8_t));
    if (!heat) { xQueueReceive(led_cmd_queue, cmd, portMAX_DELAY); return; }

    while (1) {
        // 1. Soğuma
        for (int i = 0; i < (rows + 2) * cols; i++) {
            uint8_t cool = fast_rand() % (255 / rows + 2);
            heat[i] = heat[i] > cool ? heat[i] - cool : 0;
        }

        // 2. Diffüzyon — çevresel (silindir)
        for (int y = rows + 1; y >= 2; y--) {
            for (int x = 0; x < cols; x++) {
                int left  = (x + cols - 1) % cols;
                int right = (x + 1) % cols;
                heat[y * cols + x] = (
                    heat[(y-1) * cols + left]  +
                    heat[(y-1) * cols + x]     * 2 +
                    heat[(y-1) * cols + right] +
                    heat[(y-2) * cols + x]
                ) / 5;
            }
        }

        // 3. Alev kaynağı
        for (int x = 0; x < cols; x++) {
            uint8_t flame  = 190 + fast_rand() % 66;
            heat[x]        = flame;
            heat[cols + x] = (heat[cols + x] + flame) / 2;
        }

        // 4. Sıcaklık → renk — onaylı palet
        // 0..84  : siyah → kırmızı
        // 85..169: kırmızı → turuncu
        // 170..239: turuncu → sarı
        // 240..255: sarı → beyaz (az)
        float bscale = cmd->brightness / 100.0f;
        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                uint8_t t = heat[(y + 2) * cols + x];
                uint8_t r, g, b;

                if (t < 85) {
                    r = (uint8_t)(t * 3 * bscale);
                    g = 0; b = 0;
                } else if (t < 170) {
                    uint8_t tt = t - 85;
                    r = (uint8_t)(255 * bscale);
                    g = (uint8_t)(tt * bscale);
                    b = 0;
                } else if (t < 240) {
                    uint8_t tt = t - 170;
                    r = (uint8_t)(255 * bscale);
                    g = (uint8_t)((85 + tt) * bscale);
                    b = 0;
                } else {
                    r = (uint8_t)(255 * bscale);
                    g = (uint8_t)(255 * bscale);
                    b = (uint8_t)((t - 240) * bscale);
                }

                // y=0 alt, ısı alttan yükseliyor
                set_xy(strip, x, rows - 1 - y, r, g, b, cols);
            }
        }
        led_strip_refresh(strip);

        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            free(heat);
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) {
            free(heat);
            cmd->effect = LED_CMD_OFF; return;
        }
    }
}

// ── BOUNCE — yatay dönen halka, silindir çevresinde döner ──
// Comet'ten farkı: dikey değil yatay/açısal hareket
// Birden fazla parlak nokta birlikte döner, iz bırakır
// r2/g2/b2 varsa ikinci renk renkli iz oluşturur
static void effect_bounce(led_strip_handle_t strip, LedCommand_t *cmd,
                          uint8_t rows, uint8_t cols)
{
    uint32_t step_ms = SPEED_TO_CHASE_MS(cmd->speed) / 2;
    if (step_ms < 10) step_ms = 10;
    LedCommand_t next;

    float angle  = 0.0f;
    float speed  = 0.5f;
    bool  color2 = (cmd->r2 || cmd->g2 || cmd->b2);
    int   tail   = cols / 3;
    if (tail < 2) tail = 2;

    while (1) {
        for (int i = 0; i < rows * cols; i++) led_strip_set_pixel(strip, i, 0, 0, 0);

        for (int y = 0; y < rows; y++) {
            float row_offset = (float)y / rows * (cols / 2.0f);
            int   head_x     = (int)(angle + row_offset) % cols;

            for (int t = 0; t < tail; t++) {
                int tx = ((head_x - t) % cols + cols) % cols;
                float fade = (float)(tail - t) / tail;
                fade = fade * fade;
                uint8_t br = (uint8_t)(cmd->brightness * fade);
                uint8_t cr, cg, cb;
                if (color2 && t > tail / 2) {
                    cr = cmd->r2; cg = cmd->g2; cb = cmd->b2;
                } else {
                    cr = cmd->r; cg = cmd->g; cb = cmd->b;
                }
                set_xy(strip, tx, y, scale(cr, br), scale(cg, br), scale(cb, br), cols);
            }
        }
        led_strip_refresh(strip);

        angle += speed;
        if (angle >= cols) angle -= cols;

        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

// ══════════════════════════════════════════════════
// ── Pattern Bitmaplar (16×16) ─────────────────────
// ══════════════════════════════════════════════════

// Kalp — y=0 alt, y=15 üst — dikey çevrilmiş
static const uint8_t HEART_MAP[16][16] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // 0
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // 1
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // 2
    {0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0},  // 3
    {0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0},  // 4
    {0,0,0,1,1,1,1,1,1,1,0,0,0,0,0,0},  // 5
    {0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0},  // 6
    {0,0,1,1,1,1,1,1,1,1,1,0,0,0,0,0},  // 7
    {0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0},  // 8
    {0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},  // 9
    {0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},  // 10
    {0,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0},  // 11
    {0,0,1,1,1,1,1,1,1,1,1,1,0,0,0,0},  // 12
    {0,0,0,1,1,1,0,0,1,1,1,0,0,0,0,0},  // 13 iki tepe
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // 14
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // 15
};

// Yıldırım — kullanıcı tasarımı
static const uint8_t LIGHTNING_MAP[16][16] = {
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // satır 0
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // satır 1
    {0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0},  // satır 2
    {0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0},  // satır 3
    {0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0},  // satır 4
    {0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0},  // satır 5
    {0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0},  // satır 6
    {0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0},  // satır 7
    {0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0},  // satır 8
    {0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0},  // satır 9
    {0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0},  // satır 10
    {0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0},  // satır 11
    {0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0},  // satır 12
    {0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0},  // satır 13
    {0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0},  // satır 14
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},  // satır 15
};

// ── Pattern Scroll Yardımcısı ──────────────────────
// pat[y][x] bitmap'ini X yönünde kaydırır
// pulse=true → brightness sinüs dalgasıyla atar
static void effect_scroll_pattern(led_strip_handle_t strip, LedCommand_t *cmd,
                                   uint8_t rows, uint8_t cols,
                                   const uint8_t pat[16][16], bool pulse)
{
    uint32_t step_ms = SPEED_TO_CHASE_MS(cmd->speed) / 3;
    if (step_ms < 20) step_ms = 20;
    LedCommand_t next;
    int   scroll = 0;
    float phase  = 0.0f;

    while (1) {
        float bmod = 1.0f;
        if (pulse) {
            phase += 0.12f;
            // Kalp atışı: hızlı yükseliş, yavaş düşüş
            float s = sinf(phase);
            bmod = s > 0 ? 0.25f + 0.75f * s : 0.25f;
        }

        for (int y = 0; y < rows && y < 16; y++) {
            for (int x = 0; x < cols; x++) {
                int px = ((x + scroll) % 16 + 16) % 16;
                if (pat[y][px]) {
                    uint8_t br = (uint8_t)(cmd->brightness * bmod);
                    set_xy(strip, x, y,
                           scale(cmd->r, br),
                           scale(cmd->g, br),
                           scale(cmd->b, br), cols);
                } else {
                    set_xy(strip, x, y, 0, 0, 0, cols);
                }
            }
        }
        led_strip_refresh(strip);
        scroll = (scroll + 1) % 16;

        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

// ── HEART — kalp, scroll + pulse ──────────────────
static void effect_heart(led_strip_handle_t strip, LedCommand_t *cmd,
                         uint8_t rows, uint8_t cols)
{
    effect_scroll_pattern(strip, cmd, rows, cols, HEART_MAP, true);
}

// ── LIGHTNING — yıldırım, scroll ──────────────────
static void effect_lightning(led_strip_handle_t strip, LedCommand_t *cmd,
                              uint8_t rows, uint8_t cols)
{
    effect_scroll_pattern(strip, cmd, rows, cols, LIGHTNING_MAP, false);
}

// ── RAIN — yağmur damlaları ────────────────────────
// Sütunlarda damla aşağı düşer, iz bırakır
#define MAX_RAIN_DROPS  10
#define RAIN_TRAIL_LEN   4

static void effect_rain(led_strip_handle_t strip, LedCommand_t *cmd,
                        uint8_t rows, uint8_t cols)
{
    uint32_t step_ms = SPEED_TO_RAINBOW_MS(cmd->speed);
    if (step_ms < 15) step_ms = 15;
    LedCommand_t next;

    typedef struct { float y; uint8_t x; float speed; } Drop;
    Drop *drops = calloc(MAX_RAIN_DROPS, sizeof(Drop));
    if (!drops) { xQueueReceive(led_cmd_queue, cmd, portMAX_DELAY); return; }

    // Damlaları yayılmış başlat — y=15 üst, y=0 alt
    for (int i = 0; i < MAX_RAIN_DROPS; i++) {
        drops[i].x     = fast_rand() % cols;
        drops[i].y     = (float)(fast_rand() % rows);  // üstte rastgele başla
        drops[i].speed = 0.4f + (fast_rand() % 5) * 0.25f;
    }

    while (1) {
        for (int i = 0; i < rows * cols; i++) led_strip_set_pixel(strip, i, 0, 0, 0);

        for (int d = 0; d < MAX_RAIN_DROPS; d++) {
            drops[d].y -= drops[d].speed;  // y azalır = yukarıdan aşağı

            if (drops[d].y < -RAIN_TRAIL_LEN) {
                drops[d].x     = fast_rand() % cols;
                drops[d].y     = (float)(rows - 1);  // üstten yeniden başla
                drops[d].speed = 0.4f + (fast_rand() % 5) * 0.25f;
            }

            int head = (int)drops[d].y;
            for (int t = 0; t < RAIN_TRAIL_LEN; t++) {
                int row = head + t;  // iz yukarıda (daha büyük y)
                if (row < 0 || row >= rows) continue;
                float fade = (float)(RAIN_TRAIL_LEN - t) / RAIN_TRAIL_LEN;
                fade = fade * fade;
                uint8_t br = (uint8_t)(cmd->brightness * fade);
                set_xy(strip, drops[d].x, row,
                       scale(cmd->r, br),
                       scale(cmd->g, br),
                       scale(cmd->b, br), cols);
            }
        }
        led_strip_refresh(strip);

        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            free(drops); save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { free(drops); cmd->effect = LED_CMD_OFF; return; }
    }
}

// ── DNA — iki sinüs dalgası sarmalı ───────────────
// İki renkli strand silindir çevresinde döner
// r2/g2/b2 ikinci strand rengi (yoksa tamamlayıcı renk)
static void effect_dna(led_strip_handle_t strip, LedCommand_t *cmd,
                       uint8_t rows, uint8_t cols)
{
    uint32_t step_ms = SPEED_TO_RAINBOW_MS(cmd->speed);
    LedCommand_t next;
    float phase = 0.0f;

    // İkinci renk
    uint8_t r2 = (cmd->r2 || cmd->g2 || cmd->b2) ? cmd->r2 : (255 - cmd->r);
    uint8_t g2 = (cmd->r2 || cmd->g2 || cmd->b2) ? cmd->g2 : (255 - cmd->g);
    uint8_t b2 = (cmd->r2 || cmd->g2 || cmd->b2) ? cmd->b2 : (255 - cmd->b);

    float amplitude = rows / 2.0f - 1.0f;
    float center    = rows / 2.0f - 0.5f;

    while (1) {
        for (int i = 0; i < rows * cols; i++) led_strip_set_pixel(strip, i, 0, 0, 0);

        for (int x = 0; x < cols; x++) {
            float angle = phase + (float)x / cols * 2.0f * 3.14159f;

            // Strand 1
            int y1 = (int)(center + amplitude * sinf(angle));
            if (y1 >= 0 && y1 < rows) {
                set_xy(strip, x, y1,
                       scale(cmd->r, cmd->brightness),
                       scale(cmd->g, cmd->brightness),
                       scale(cmd->b, cmd->brightness), cols);
            }

            // Strand 2 — karşı faz (π offset)
            int y2 = (int)(center + amplitude * sinf(angle + 3.14159f));
            if (y2 >= 0 && y2 < rows) {
                set_xy(strip, x, y2,
                       scale(r2, cmd->brightness),
                       scale(g2, cmd->brightness),
                       scale(b2, cmd->brightness), cols);
            }

            // Kesişim noktaları — iki strand yakınsa köprü noktası
            if (abs(y1 - y2) <= 1 && y1 >= 0 && y1 < rows) {
                uint8_t br = cmd->brightness / 2;
                set_xy(strip, x, y1,
                       scale((cmd->r + r2) / 2, br),
                       scale((cmd->g + g2) / 2, br),
                       scale((cmd->b + b2) / 2, br), cols);
            }
        }
        led_strip_refresh(strip);

        phase += 0.18f;
        if (phase > 2.0f * 3.14159f) phase -= 2.0f * 3.14159f;

        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { cmd->effect = LED_CMD_OFF; return; }
    }
}

// ── STAR — kayan yıldız, aşağıdan yukarı + zigzag ──
#define MAX_STARS    6
#define STAR_TRAIL   5

static void effect_star(led_strip_handle_t strip, LedCommand_t *cmd,
                        uint8_t rows, uint8_t cols)
{
    uint32_t step_ms = SPEED_TO_RAINBOW_MS(cmd->speed); // yağmurla aynı hız
    if (step_ms < 15) step_ms = 15;
    LedCommand_t next;

    typedef struct {
        float   y;       // mevcut dikey pozisyon
        uint8_t x;       // mevcut sütun
        float   speed;
        int     zx;      // zigzag yönü: +1 veya -1
        int     ztimer;  // zigzag değişim sayacı
    } Star;

    Star *stars = calloc(MAX_STARS, sizeof(Star));
    if (!stars) { xQueueReceive(led_cmd_queue, cmd, portMAX_DELAY); return; }

    // Başlangıçta farklı yüksekliklere dağıt
    for (int i = 0; i < MAX_STARS; i++) {
        stars[i].x      = fast_rand() % cols;
        stars[i].y      = (float)(fast_rand() % rows);
        stars[i].speed  = 0.4f + (fast_rand() % 5) * 0.25f; // yağmurla aynı
        stars[i].zx     = (fast_rand() & 1) ? 1 : -1;
        stars[i].ztimer = 2 + fast_rand() % 4;
    }

    while (1) {
        for (int i = 0; i < rows * cols; i++) led_strip_set_pixel(strip, i, 0, 0, 0);

        for (int s = 0; s < MAX_STARS; s++) {
            // Aşağıdan yukarı: y artar
            stars[s].y += stars[s].speed;

            // Zigzag: belirli adımda x kayar
            stars[s].ztimer--;
            if (stars[s].ztimer <= 0) {
                stars[s].zx     = -stars[s].zx;
                stars[s].ztimer = 2 + fast_rand() % 4;
                int nx = (int)stars[s].x + stars[s].zx;
                if (nx >= 0 && nx < cols) stars[s].x = (uint8_t)nx;
            }

            // Üste çıkınca alt taraftan yeniden başla
            if (stars[s].y > rows + STAR_TRAIL) {
                stars[s].x      = fast_rand() % cols;
                stars[s].y      = 0.0f;
                stars[s].speed  = 0.4f + (fast_rand() % 5) * 0.25f;
                stars[s].zx     = (fast_rand() & 1) ? 1 : -1;
                stars[s].ztimer = 2 + fast_rand() % 4;
            }

            int head = (int)stars[s].y;
            for (int t = 0; t <= STAR_TRAIL; t++) {
                int row = head - t;  // iz altta (daha küçük y)
                if (row < 0 || row >= rows) continue;
                float fade = (float)(STAR_TRAIL - t + 1) / (STAR_TRAIL + 1);
                fade = fade * fade;
                uint8_t br = (uint8_t)(cmd->brightness * fade);
                set_xy(strip, stars[s].x, row,
                       scale(cmd->r, br),
                       scale(cmd->g, br),
                       scale(cmd->b, br), cols);
            }
        }
        led_strip_refresh(strip);

        if (xQueueReceive(led_cmd_queue, &next, pdMS_TO_TICKS(step_ms)) == pdTRUE) {
            free(stars); save_last_command(&next); *cmd = next; return;
        }
        if (reconfig_requested()) { free(stars); cmd->effect = LED_CMD_OFF; return; }
    }
}

// ── RMT init ───────────────────────────────────────
static led_strip_handle_t led_init(uint16_t count)
{
    if (count == 0) {
        LOG_WARN(TAG, "LED sayısı 0 — RMT başlatılmadı");
        return NULL;
    }
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_DEFAULT_GPIO,
        .max_leds       = count,
        .led_model      = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = RMT_RESOLUTION_HZ,
        .flags.with_dma = false,
    };
    led_strip_handle_t strip;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip));
    led_strip_clear(strip);
    LOG_INFO(TAG, "RMT init tamamlandı");
    return strip;
}

// ── led_task ───────────────────────────────────────
void led_task(void *pvParameters)
{
    led_config_load();
    uint16_t count = s_cfg.rows * s_cfg.cols;
    uint8_t  rows  = (uint8_t)s_cfg.rows;
    uint8_t  cols  = (uint8_t)s_cfg.cols;
    led_strip_handle_t strip = led_init(count);
    LOG_INFO(TAG, "LED task başladı");

    LedCommand_t cmd = {
        .effect      = LED_CMD_OFF,
        .brightness  = LED_MIN_BRIGHTNESS,
        .speed       = LED_DEFAULT_SPEED,
        .color_count = 1,
    };

    if (load_last_command(&cmd)) {
        LOG_INFO(TAG, "Son state yüklendi — effect:%d", cmd.effect);
    } else {
        LOG_INFO(TAG, "Son state yok, OFF ile başlıyoruz");
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));

        if (xEventGroupGetBits(led_event_group) & RECONFIG_BIT) {
            xEventGroupClearBits(led_event_group, RECONFIG_BIT);
            if (strip) led_strip_del(strip);
            count = s_cfg.rows * s_cfg.cols;
            rows  = (uint8_t)s_cfg.rows;
            cols  = (uint8_t)s_cfg.cols;
            strip = led_init(count);
            cmd.effect = LED_CMD_OFF;
            LOG_INFO(TAG, "LED yeniden yapılandırıldı — %dx%d", rows, cols);
        }

        switch (cmd.effect) {
            case LED_CMD_SOLID:
                if (strip) effect_solid(strip, &cmd, count);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_FADE:
                if (strip) effect_fade(strip, &cmd, count);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_RAINBOW:
                if (strip) effect_rainbow(strip, &cmd, count);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_CHASE:
                if (strip) effect_chase(strip, &cmd, count);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_FLASH:
                if (strip) effect_flash(strip, &cmd, count);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_RANDOM:
                if (strip) effect_random(strip, &cmd, count);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_COMET:
                if (strip && rows > 0 && cols > 0)
                    effect_comet(strip, &cmd, rows, cols);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_WAVE:
                if (strip && rows > 0 && cols > 0)
                    effect_wave(strip, &cmd, rows, cols);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_TWINKLE:
                if (strip) effect_twinkle(strip, &cmd, count);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_FIRE:
                if (strip && rows > 0 && cols > 0)
                    effect_fire(strip, &cmd, rows, cols);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_BOUNCE:
                if (strip && rows > 0 && cols > 0)
                    effect_bounce(strip, &cmd, rows, cols);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_HEART:
                if (strip && rows > 0 && cols > 0)
                    effect_heart(strip, &cmd, rows, cols);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_LIGHTNING:
                if (strip && rows > 0 && cols > 0)
                    effect_lightning(strip, &cmd, rows, cols);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_RAIN:
                if (strip && rows > 0 && cols > 0)
                    effect_rain(strip, &cmd, rows, cols);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_DNA:
                if (strip && rows > 0 && cols > 0)
                    effect_dna(strip, &cmd, rows, cols);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_STAR:
                if (strip && rows > 0 && cols > 0)
                    effect_star(strip, &cmd, rows, cols);
                else xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
            case LED_CMD_OFF:
            default:
                if (strip) {
                    led_strip_clear(strip);
                    led_strip_refresh(strip);
                }
                xQueueReceive(led_cmd_queue, &cmd, portMAX_DELAY);
                break;
        }
    }
}