# BambuLCD

# BambuLCD V1

A real-time Bambu Lab printer status display built with an ESP32 DEVKIT V1 and a 1602 I2C LCD. Connects directly to your printer over your local network using the same LAN MQTT method as [PrintSphere](https://github.com/cptkirki/PrintSphere) — no cloud dependency, no extra software, no app required.

Tested with the **Bambu Lab A1 Mini**. Should work with other Bambu models that expose a local MQTT broker (X1C, P1P, P1S, A1).

---

## Features

- Live print progress with visual block bar
- Time remaining (shows `<1m` in the final minute)
- Nozzle and bed temperatures with targets
- Auto screen off when printer is idle, auto on when printing starts
- Paused state clearly shown on screen
- Print Done notification — dismiss with button or auto-off after 60 seconds
- Error screens for print failures and printer hardware faults (HMS alerts, AMS jams)
- Connection error screen if the printer can't be reached
- Hold button to force screen off at any time; press to wake
- Anti-flicker rendering — static screens only redraw when something actually changes

---

## Hardware

| Part | Details |
|---|---|
| Microcontroller | ESP32 DEVKIT V1 |
| Display | 1602 I2C LCD (16×2) with PCF8574 I2C backpack |
| Button | 4-pin momentary push button |
| Other | Breadboard, jumper wires |

---

## Wiring

### LCD → ESP32

| LCD Pin | ESP32 Pin | Notes |
|---------|-----------|-------|
| GND | GND | Any GND pin |
| VCC | VIN | Must be 5V — do not use 3V3 |
| SDA | D21 | I2C data |
| SCL | D22 | I2C clock |

### Button → ESP32

| Button Pin | ESP32 Pin | Notes |
|------------|-----------|-------|
| Leg 1 | D4 | Signal |
| Leg 2 | GND | Any GND pin |

> 4-pin buttons have 2 pairs of legs — use one pair (same side of the button). No resistor needed; the ESP32 internal pull-up is enabled in code.

---

## Setup

### 1. Install the ESP32 Board Package

In Arduino IDE go to **File → Preferences → Additional Board Manager URLs** and add:

```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

Then go to **Tools → Board → Boards Manager**, search `esp32`, and install **esp32 by Espressif Systems**.

Select board: **Tools → Board → ESP32 Arduino → ESP32 Dev Module**

### 2. Install Libraries

Go to **Sketch → Include Library → Manage Libraries** and install:

| Library | Author |
|---------|--------|
| LiquidCrystal I2C | Frank de Brabander |
| PubSubClient | Nick O'Leary |
| ArduinoJson | Benoit Blanchon |

`WiFi` and `WiFiClientSecure` are included with the ESP32 board package — do not install separately.

### 3. Configure

Open `config.h` and fill in your details:

```cpp
#define WIFI_SSID         "YourNetworkName"    // 2.4 GHz only
#define WIFI_PASS         "YourPassword"

#define BAMBU_IP          "192.168.1.100"      // printer's local IP
#define BAMBU_SERIAL      "01A00C123456789"    // printer serial number
#define BAMBU_ACCESS_CODE "XXXXXXXX"           // 8-character access code
```

#### Where to find these on your Bambu printer:

| Value | Location on printer touchscreen |
|-------|----------------------------------|
| IP Address | Settings → Network → IP Address |
| Serial Number | Settings → Device → Serial Number |
| Access Code | Settings → Network → Access Code |

> You do **not** need to enable LAN Only Mode or Developer Mode to read printer data. The access code works while the printer stays on Bambu Cloud normally.

### 4. Upload

- Connect ESP32 via USB
- Select the correct port under **Tools → Port**
- Click Upload

---

## Screen Reference

### Page 0 — Print Progress

```
 45%[██████────]
1h23m   PRINTING
```

```
 45%[██████────]
1h23m    PAUSED
```

| Element | Description |
|---------|-------------|
| `45%` | Progress percentage, right-aligned |
| `[██████────]` | 10-segment visual progress bar |
| `1h23m` | Time remaining (`<1m` when under a minute) |
| `PRINTING` / `PAUSED` | Current printer state |

### Page 1 — Temperatures

```
Nozzle:210/220
Bed:60/60
```

Shows current temperature / target temperature for nozzle and bed.

---

## Button Behaviour

| Action | What happens |
|--------|--------------|
| Press (idle, screen off) | Shows IDLE screen |
| Press (IDLE screen showing) | Screen turns off |
| Press (printing or paused) | Switches between Page 0 and Page 1 |
| Press (PRINT DONE!) | Dismisses and turns screen off |
| Press (error popup) | Dismisses and turns screen off |
| **Hold 0.8 s** (any state) | **Forces screen off** |
| Press (screen force-off) | Wakes screen back up |

---

## All Screen States

| State | Display | How it clears |
|-------|---------|---------------|
| Booting / connecting | `BambuLCD V1` / `Connecting...` | Automatically when MQTT connects |
| Idle | Screen off | Automatically when print starts |
| Idle (button pressed) | `IDLE` / `Btn to sleep` | Press button again |
| Printing | Page 0 or Page 1 | Print ends / pauses |
| Paused | Page 0 shows PAUSED, Page 1 works normally | Print resumes or ends |
| Print done | `PRINT DONE!` / `Btn to dismiss` | Button press or 60 second timeout |
| Print failed | `PRINT FAILED!` / `Btn to dismiss` | Button press |
| Printer error | `PRINTER ERROR!` / `Btn to dismiss` | Button press |
| Connection error | `CONNECTION ERR!` / `Check config.h` | Automatically retries every 5 s |
| Force off (hold) | Screen off | Any button press |

> **PRINTER ERROR** covers hardware faults detected by Bambu's HMS system — AMS jams, clog detection, runout, temperature failures, and similar.

---

## How the Connection Works

The ESP32 connects **directly to your printer over your local WiFi network** — no Bambu Cloud involved at this step. Your cloud connection on the printer is completely unaffected and keeps working at the same time.

The connection method is the same used by [PrintSphere](https://github.com/cptkirki/PrintSphere):

| Setting | Value |
|---------|-------|
| Protocol | MQTT over TLS |
| Port | 8883 |
| Username | `bblp` |
| Password | Your 8-character LAN access code |
| Topic | `device/{SERIAL}/report` |
| Cert verification | Skipped (Bambu uses self-signed certs) |

The printer pushes a JSON update roughly every second while printing. The ESP32 parses the relevant fields and updates the display.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| Screen completely blank after upload | Change `LCD_I2C_ADDR` to `0x3F` in config.h |
| Solid rectangles instead of characters | Adjust the contrast trimmer pot on the back of the I2C backpack |
| Stuck on `Connecting...` | Wrong WiFi credentials, or on 5 GHz — ESP32 needs 2.4 GHz |
| `CONNECTION ERR!` on screen | Wrong IP or access code in config.h; check printer screen for current values |
| MQTT `rc=-2` in Serial Monitor | Printer unreachable — check IP, ensure printer and ESP32 are on same network |
| MQTT `rc=-4` in Serial Monitor | TLS handshake failed — double-check access code; try power-cycling the printer |
| Temps show 0/0 | Normal when printer is idle and not heating — start a print to verify |
| Button does nothing | Check wiring — one leg to D4, other leg to GND |
| Characters look wrong or corrupted | Use VIN (5V) for LCD power, not 3V3 |

Open **Tools → Serial Monitor** at **115200 baud** after uploading to see live connection logs and MQTT error codes.

---

## File Structure

```
BambuLCD/
├── BambuLCD.ino       Main sketch
├── config.h           User configuration (WiFi, printer credentials, pin assignments)
├── WIRING.md          Wiring tables and pin reference
└── README.md          This file
```

---

## Built With

- [Arduino ESP32 Core](https://github.com/espressif/arduino-esp32) — Espressif Systems
- [PubSubClient](https://github.com/knolleary/pubsubclient) — Nick O'Leary
- [ArduinoJson](https://arduinojson.org) — Benoit Blanchon
- [LiquidCrystal_I2C](https://github.com/johnrickman/LiquidCrystal_I2C) — Frank de Brabander
- Connection method adapted from [PrintSphere](https://github.com/cptkirki/PrintSphere) by cptkirki
- Bambu MQTT protocol documented by [OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI)
