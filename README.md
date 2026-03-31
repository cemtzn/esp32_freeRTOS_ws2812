# ESP32 WS2812B LED Matrix Controller

A fully-featured WiFi-based control system for a 16×16 WS2812B LED matrix, running on ESP32 with FreeRTOS. Features a real-time web interface, 16 animation effects, user management, and OTA firmware update support.

---

## About

This project controls a 16×16 WS2812B LED matrix (256 LEDs) wrapped around a cylinder, providing a 360° view. The ESP32 operates as a WiFi Access Point — no external network required. Users connect directly to the device and control it through a web browser.

The project grew from a learning exercise in embedded systems and FreeRTOS architecture into a complete IoT lighting controller with user authentication, OTA updates, and persistent session tokens.

---

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-WROOM |
| LEDs | 16×16 WS2812B matrix (256 LEDs) |
| Layout | Serpentine wiring, wrapped 360° around a cylinder |
| GPIO | GPIO18 (default, configurable via web UI) |
| Power | 5V 3A power adapter (minimum) |
| LED Protocol | WS2812B or WS2811 (selectable) |

### Wiring

```
5V Adapter (+) ───────┬──── LED Matrix VCC
                      └──── ESP32 VIN

5V Adapter (-) ───────┬──── LED Matrix GND
                      └──── ESP32 GND (common ground required)

ESP32 GPIO18  ────────────► LED Matrix DATA IN
```

> **Note:** The LED matrix must be powered directly from the adapter, not through the ESP32. 256 LEDs can theoretically draw up to 15A at full brightness; a 5V 3A supply is sufficient for typical usage patterns.

---

## Features

### 16 LED Animation Effects

| Effect | Description |
|--------|-------------|
| **Solid** | Static single color |
| **Fade** | Breathing effect with sinusoidal brightness |
| **Rainbow** | Smooth 2-3 color gradient transition |
| **Comet** | Bright head with fading tail, scrolls along cylinder |
| **Wave** | Sinusoidal brightness with angular column offset |
| **Twinkle** | Independent twinkling LEDs, starfield effect |
| **Fire** | Realistic fire simulation rising from the bottom |
| **Bounce** | Spiral rotating ring around the cylinder |
| **Heart** | Heart shape with pulse effect, horizontal scroll |
| **Lightning** | Lightning bolt shape, scrolls around cylinder |
| **Rain** | Raindrops falling from top to bottom |
| **DNA** | Two-color double helix rotating along cylinder |
| **Star** | Shooting stars with zigzag path, bottom to top |
| **Chase** | Single LED chasing sequence |
| **Flash** | Strobe effect |
| **Random** | Random color changes |

Every effect supports independent **color**, **brightness**, and **speed** control.

### User & Profile Management
- 1 admin account + up to 3 user accounts
- Each user can save up to 3 effect profiles
- Profile stores: effect type, color(s), brightness, speed
- Users can change their own password
- Admin can create and delete user accounts

### OTA (Over-The-Air) Updates
- Upload `.bin` firmware files directly from the admin panel
- Dual A/B slot system: current firmware is preserved during update
- ESP32 automatically restarts after successful update
- Rollback protection if update fails

### Device Recognition (Token)
- A 32-character random token is generated on successful login
- Token is stored in the browser's `localStorage` and ESP32's NVS
- Subsequent connections auto-login without requiring credentials
- Logout clears the token from both sides

### Web Interface
- Mobile-optimized for iOS and Android
- Google Fonts typography (DM Mono + Syne)
- Real-time LED strip preview animation
- Color picker with 8 preset colors
- Brightness and speed sliders
- Admin panel: LED control, test modes, user management, config, logs, OTA

---

## Software Architecture

### FreeRTOS Task Structure

```
app_main (CPU0)
├── log_task     — Priority: 2, CPU0, Stack: 4KB
│                  Queue-based logging
├── led_task     — Priority: 5, CPU1, Stack: 4KB
│                  RMT peripheral LED driving
└── http (built-in) — ESP-IDF HTTP server tasks
                      WebSocket + file serving
```

```
IPC Mechanisms:
  led_cmd_queue  (5 items)  — HTTP handler → LED task
  log_queue      (20 items) — all tasks → log task
  led_event_group           — RECONFIG_BIT, WIFI_CONN_BIT
```

### Coordinate System

LEDs are wired in serpentine (snake) pattern:
- `y=0` is the bottom row, `y=15` is the top row
- Even rows go left-to-right, odd rows go right-to-left
- `xy_to_index(x, y, cols)` converts coordinates to LED index

```
y=15: ← 255 254 253 ... 240
y=14: → 224 225 226 ... 239
...
y=1:  ← 31  30  29  ...  16
y=0:  → 0   1   2   ...  15
```

### File Structure

```
main/
├── main.c              — app_main, queue/event group init
├── shared_types.h      — LedEffect_t, LedCommand_t, LedConfig_t, LedProfile_t
├── def.h               — all constants and NVS keys
├── led_handler.h/c     — animation effects, RMT init, xy_to_index
├── wifi_handler.h/c    — WiFi AP mode, event handler
├── http_handler.h/c    — HTTP/WebSocket server, all message handlers
├── user_handler.h/c    — user and profile CRUD operations
├── ota_handler.h/c     — OTA firmware update, base64 decode
├── token_handler.h/c   — session token generation, verification, deletion
├── log_handler.h/c     — queue-based logging system
└── CMakeLists.txt

spiffs_data/
├── index.html          — single page application
├── style.css           — iOS/Android optimized styles
└── app.js              — WebSocket client, UI logic

partitions.csv          — custom partition table
sdkconfig.defaults      — persistent build configuration
```

### NVS Data Layout

```
Namespace "led_cfg":
  rows, cols         → uint16_t  (matrix dimensions)
  gpio               → uint8_t   (data pin number)
  led_model          → uint8_t   (0=WS2812, 1=WS2811)
  last_cmd           → blob      (last LED command, restored on power-on)
  admin_pass         → str       (admin password)

Namespace "users":
  u0name, u0pass     → str       (user 0)
  u1name, u1pass     → str       (user 1)
  u2name, u2pass     → str       (user 2)

Namespace "u0profs" / "u1profs" / "u2profs":
  s0, s1, s2         → blob      (LedProfile_t, 3 slots per user)

Namespace "tokens":
  t0, t1, t2         → str       (user session tokens)
  ta                 → str       (admin session token)
```

### Partition Table

```
# Name     Offset    Size    Description
nvs        0x9000    16KB    NVS storage
ota_data   0xD000    8KB     OTA state info
phy_init   0xF000    4KB     RF calibration
ota_0      0x10000   1.5MB   Active firmware slot
ota_1      0x190000  1.5MB   OTA target slot
spiffs     0x310000  960KB   Web files
```

Total: 4MB flash. Factory partition intentionally omitted in favor of A/B slot cycling.

---

## Getting Started

### Prerequisites

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- Python 3.8+
- `espressif/led_strip` managed component (auto-fetched via idf_component.yml)

### Build & Flash

```bash
# Clone the repository
git clone https://github.com/yourusername/esp32-led-matrix
cd esp32-led-matrix

# Full clean build (first time or after partition changes)
idf.py fullclean
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

# Web files only (when only HTML/CSS/JS changed)
idf.py -p /dev/ttyUSB0 spiffs-flash

# Lower baud rate if USB issues occur
idf.py -p /dev/ttyUSB0 -b 115200 flash monitor
```

### sdkconfig.defaults

```ini
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

---

## Usage

### First Connection

1. Power the ESP32 with a 5V 3A adapter
2. Connect to the `LED_Matrix` WiFi network — Password: `test1234`
3. Open `http://192.168.4.1` in a browser
   - On iOS Safari, you must type the full `http://` prefix
   - Chrome or Firefox recommended
4. Log in — Admin: username `admin`, password `admin1234`
5. After first login, a token is saved — subsequent connections auto-login

### WiFi QR Code

Generate a WiFi QR code at [qifi.org](https://qifi.org) for quick connections:

```
WIFI:T:WPA;S:LED_Matrix;P:test1234;;
```

Print and attach it near the lamp. Scanning connects the phone automatically.

### Home Screen Shortcut

Use "Add to Home Screen" from the browser to create a shortcut. The token system ensures every launch auto-logs in.

---

## Default Settings

| Setting | Value |
|---------|-------|
| WiFi SSID | `LED_Matrix` |
| WiFi Password | `test1234` |
| IP Address | `192.168.4.1` |
| Admin Username | `admin` |
| Admin Password | `admin1234` |
| GPIO Pin | `18` |
| LED Model | `WS2812B` |
| LED Matrix | `16×16` |

> All passwords can be changed from the admin panel after first login.

---

## Technical Notes

### WS2811 Support
WS2812B uses GRB color order while WS2811 uses RGB. Since ESP-IDF's `led_strip` library doesn't define `LED_MODEL_WS2811`, this is solved with a software R/G channel swap:

```c
static void set_pixel(led_strip_handle_t strip, int idx,
                      uint8_t r, uint8_t g, uint8_t b) {
    if (s_cfg.led_model == 1)
        led_strip_set_pixel(strip, idx, g, r, b); // WS2811: swap R↔G
    else
        led_strip_set_pixel(strip, idx, r, g, b); // WS2812B: normal
}
```

### OTA Flow
```
Browser → base64 chunks → WebSocket → esp_ota_write()
                                            ↓
                                  esp_ota_end() + SHA256 verify
                                            ↓
                                  esp_ota_set_boot_partition()
                                            ↓
                                       esp_restart()
```

### Fire Effect Color Palette
```
Temperature   0 - 84  → Black  → Red
Temperature  85 - 169 → Red    → Orange
Temperature 170 - 239 → Orange → Yellow
Temperature 240 - 255 → Yellow → White (minimal)
```

---

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Firmware | ESP-IDF v5.x |
| RTOS | FreeRTOS |
| LED driver | RMT peripheral, espressif/led_strip |
| Storage | NVS (settings/users/tokens), SPIFFS (web files) |
| Communication | WiFi AP, HTTP server, WebSocket |
| OTA | esp_ota_ops, mbedTLS base64 |
| Frontend | Vanilla JS, CSS3 |
| Fonts | DM Mono, Syne (Google Fonts) |

---

## License

MIT License — free to use, modify, and distribute.
