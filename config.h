// ============================================================
//  config.h — BambuLCD V1  —  EDIT THIS FILE BEFORE UPLOADING
// ============================================================

#ifndef CONFIG_H
#define CONFIG_H

// ── WiFi ─────────────────────────────────────────────────────────────────
// Use your 2.4 GHz network — ESP32 does NOT support 5 GHz
#define WIFI_SSID         "Saturn"
#define WIFI_PASS         "Yl0esmtts!"

// ── Bambu A1 Mini — LAN MQTT credentials ─────────────────────────────────
//
//  BAMBU_IP          Touchscreen → Settings → Network → IP Address
//                    Tip: set a static IP in your router so it never changes
//
//  BAMBU_SERIAL      Touchscreen → Settings → Device → Serial Number
//                    Looks like: 01A00C123456789
//
//  BAMBU_ACCESS_CODE Touchscreen → Settings → Network → Access Code
//                    8-character code (letters + numbers)
//                    NOTE: you do NOT need to enable "LAN Only Mode" —
//                    the access code works while staying on Bambu Cloud
//
#define BAMBU_IP          "192.168.212.228"
#define BAMBU_SERIAL      "0309CA482100428"
#define BAMBU_ACCESS_CODE "61f10ca3"

// ── Hardware ──────────────────────────────────────────────────────────────
//  LCD I2C address: try 0x27 first.
//  If the screen stays completely blank after upload, change to 0x3F.
#define LCD_I2C_ADDR  0x27

//  Button GPIO pin.  Wired between this pin and GND.
//  Internal pull-up is enabled in code — no resistor needed.
#define BUTTON_PIN    4

#endif // CONFIG_H
