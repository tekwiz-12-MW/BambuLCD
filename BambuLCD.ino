// ============================================================
//  BambuLCD V1
//  ESP32 DEVKIT V1  +  1602 I2C LCD (16×2)  +  Push Button
//  Bambu Lab A1 Mini — LAN MQTT over TLS (same method as PrintSphere)
//
//  Pages (button cycles while printing):
//    Page 0 – Print : progress %, bar, time left, state
//    Page 1 – Info  : nozzle temp / target, bed temp / target
//
//  States:
//    Connecting       → "BambuLCD V1 / Connecting..."
//    Idle             → screen OFF (auto on when print starts)
//    Idle Screen      → button pressed from idle: shows "IDLE" until btn again
//    Printing         → Page 0 / Page 1 (button switches; page stays put)
//    Paused           → Page 0 shows "PAUSED" / Page 1 still works
//    Print Done       → "PRINT DONE!" — btn to dismiss or auto-off after 60 s
//    Print Failed     → "PRINT FAILED! / Btn to dismiss" until button press
//    Printer Error    → "PRINTER ERROR! / Btn to dismiss" until button press
//                       (HMS alerts, AMS jams, hardware faults, etc.)
//    Connection Error → "CONNECTION ERR! / Check config.h"
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include "config.h"

// ── Custom character: solid block for progress bar ───────────────────────
byte BLOCK[8] = {
  0b11111, 0b11111, 0b11111, 0b11111,
  0b11111, 0b11111, 0b11111, 0b11111
};

// ── LCD  (address 0x27 is most common; change to 0x3F in config.h if blank)
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);

// ── MQTT ─────────────────────────────────────────────────────────────────
WiFiClientSecure secureClient;
PubSubClient     mqtt(secureClient);

// ── Application state machine ─────────────────────────────────────────────
enum AppState {
  S_CONNECTING,        // booting / waiting for first MQTT connection
  S_IDLE,              // printer idle → screen off
  S_IDLE_SCREEN,       // button pressed from idle → shows "IDLE", btn again → screen off
  S_PRINTING,          // gcode_state == RUNNING
  S_PAUSED,            // gcode_state == PAUSE
  S_DONE,              // gcode_state == FINISH  (brief message, then idle)
  S_FAILED_POPUP,      // gcode_state == FAILED  (dismiss with button)
  S_PRINTER_ERR_POPUP, // print_error != 0 / HMS alert (dismiss with button)
  S_CONNECTION_ERR     // MQTT unreachable
};

AppState appState = S_CONNECTING;

// ── Printer data ──────────────────────────────────────────────────────────
float  nozzleTemp   = 0.0f,  nozzleTarget = 0.0f;
float  bedTemp      = 0.0f,  bedTarget    = 0.0f;
int    progress     = 0;      // 0–100 %
int    remainMin    = 0;      // minutes remaining
int    printError   = 0;      // non-zero = HMS / AMS / hardware fault
String gcodeState   = "IDLE";
bool   dataReceived = false;

// ── UI ────────────────────────────────────────────────────────────────────
int  page     = 0;      // 0 = print page, 1 = temps page
bool screenOn = false;

// ── Button ────────────────────────────────────────────────────────────────
bool          lastReading   = HIGH;
bool          btnState      = HIGH;
unsigned long lastDebounce  = 0;

// ── Timing ────────────────────────────────────────────────────────────────
unsigned long lastRefresh    = 0;
unsigned long lastMqttRetry  = 0;
unsigned long doneSince      = 0;
unsigned long connectStart   = 0;

const unsigned long REFRESH_MS       =  500;   // LCD redraw interval
const unsigned long MQTT_RETRY_MS    = 5000;   // reconnect attempt interval
const unsigned long DONE_MS          = 60000;  // "PRINT DONE!" auto-off after 60 s
const unsigned long CONNECT_TIMEOUT  = 15000;  // before showing conn error
const unsigned long HOLD_MS          =  800;   // hold duration to force screen off

// ── Hold / forced-off ─────────────────────────────────────────────────────
bool          forcedOff     = false;  // true = screen held off by long-press
unsigned long btnPressStart = 0;      // when button went LOW
bool          holdFired     = false;  // prevents double-firing hold + press

// ── Anti-flicker: only redraw static screens when state actually changes ──
AppState lastDrawnState = (AppState)255;
int      lastDrawnPage  = -1;

// ── MQTT topic ────────────────────────────────────────────────────────────
String reportTopic;


// ══════════════════════════════════════════════════════════════════════════
//  LCD helpers
// ══════════════════════════════════════════════════════════════════════════

void lcdOn() {
  if (!screenOn) {
    lcd.backlight();
    lcd.display();
    screenOn = true;
  }
}

void lcdOff() {
  if (screenOn) {
    lcd.noBacklight();
    lcd.noDisplay();
    screenOn = false;
  }
}

// Write exactly 16 chars to a row — pads with spaces / truncates as needed
void row(uint8_t r, String s) {
  while ((int)s.length() < 16) s += ' ';
  lcd.setCursor(0, r);
  lcd.print(s.substring(0, 16));
}

// Left-pad a string to <width> chars
String padL(String s, int width) {
  while ((int)s.length() < width) s = ' ' + s;
  return s;
}

// Format minutes → "Xh XXm", "XXm", "<1m" (under 1 min), "--" (unknown)
String fmtTime(int mins) {
  if (mins <  0)  return "--";
  if (mins == 0)  return "<1m";
  if (mins < 60)  return String(mins) + "m";
  int h = mins / 60;
  int m = mins % 60;
  return String(h) + "h" + (m < 10 ? "0" : "") + String(m) + "m";
}


// ══════════════════════════════════════════════════════════════════════════
//  Page renderers
// ══════════════════════════════════════════════════════════════════════════

// ── Page 0 — Print progress ───────────────────────────────────────────────
//
//  Row 0:  " 45%[██████────]"   (4 + 1 + 10 + 1 = 16)
//  Row 1:  "1h23m   PRINTING"  or  "1h23m    PAUSED "
//
void drawPrintPage() {
  // ── Row 0: percent + bar ──────────────────────────────────────────────
  String pct    = padL(String(progress) + "%", 4);   // e.g. " 45%"
  int    filled = (int)((progress / 100.0f) * 10.0f);
  filled = constrain(filled, 0, 10);

  lcd.setCursor(0, 0);
  lcd.print(pct + "[");                  // 5 chars printed so far
  for (int i = 0; i < 10; i++) {
    if (i < filled) lcd.write(byte(0)); // custom solid block
    else            lcd.print('-');
  }
  lcd.print("]");                        // total: 5 + 10 + 1 = 16 ✓

  // ── Row 1: time remaining + state ─────────────────────────────────────
  String timeStr  = fmtTime(remainMin);
  String stateStr = (appState == S_PAUSED) ? "PAUSED" : "PRINTING";
  String r1       = timeStr;
  int    fill     = 16 - (int)timeStr.length() - (int)stateStr.length();
  for (int i = 0; i < max(1, fill); i++) r1 += ' ';
  r1 += stateStr;
  row(1, r1);
}

// ── Page 1 — Temperatures ─────────────────────────────────────────────────
//
//  Row 0:  "Nozzle:210/220  "   nozzle current / target
//  Row 1:  "Bed:60/60       "   bed    current / target
//
void drawTempsPage() {
  String noz = "Nozzle:" + String((int)nozzleTemp) + "/" + String((int)nozzleTarget);
  String bed = "Bed:"    + String((int)bedTemp)    + "/" + String((int)bedTarget);
  row(0, noz);
  row(1, bed);
}

// ── Status / error screens ────────────────────────────────────────────────

void drawConnecting() {
  row(0, "  BambuLCD V1   ");
  row(1, "  Connecting... ");
}

void drawConnectionErr() {
  row(0, "CONNECTION ERR! ");
  row(1, "Check config.h  ");
}

void drawPrinterErr() {
  row(0, "PRINTER ERROR!  ");  // HMS alert, AMS jam, hardware fault, etc.
  row(1, "Btn to dismiss  ");
}

void drawFailed() {
  row(0, "PRINT FAILED!   ");
  row(1, "Btn to dismiss  ");
}

void drawDone() {
  row(0, "  PRINT DONE!   ");
  row(1, " Btn to dismiss ");
}

void drawIdle() {
  row(0, "     IDLE       ");
  row(1, " Btn to sleep   ");
}


// ══════════════════════════════════════════════════════════════════════════
//  MQTT message callback
// ══════════════════════════════════════════════════════════════════════════

void onMessage(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<8192> doc;
  if (deserializeJson(doc, payload, length)) return;

  JsonObject pr = doc["print"];
  if (pr.isNull()) return;

  dataReceived = true;

  // ── Parse fields ──────────────────────────────────────────────────────
  if (!pr["gcode_state"].isNull())
    gcodeState = pr["gcode_state"].as<String>();
  if (!pr["mc_percent"].isNull())
    progress = pr["mc_percent"].as<int>();
  if (!pr["mc_remaining_time"].isNull())
    remainMin = pr["mc_remaining_time"].as<int>();
  if (!pr["nozzle_temper"].isNull())
    nozzleTemp = pr["nozzle_temper"].as<float>();
  if (!pr["nozzle_target_temper"].isNull())
    nozzleTarget = pr["nozzle_target_temper"].as<float>();
  if (!pr["bed_temper"].isNull())
    bedTemp = pr["bed_temper"].as<float>();
  if (!pr["bed_target_temper"].isNull())
    bedTarget = pr["bed_target_temper"].as<float>();
  if (!pr["print_error"].isNull())
    printError = pr["print_error"].as<int>();

  // ── Do NOT change state when user is dismissing a popup ───────────────
  if (appState == S_FAILED_POPUP || appState == S_PRINTER_ERR_POPUP) return;

  // ── Update state ──────────────────────────────────────────────────────
  // Printer hardware / HMS / AMS error takes top priority
  if (printError != 0) {
    appState = S_PRINTER_ERR_POPUP;
    return;
  }

  if      (gcodeState == "RUNNING") {
    // Only reset to page 0 when first entering printing state
    if (appState != S_PRINTING && appState != S_PAUSED) page = 0;
    appState = S_PRINTING;
  }
  else if (gcodeState == "PAUSE")   { appState = S_PAUSED;             }
  else if (gcodeState == "FAILED")  { appState = S_FAILED_POPUP;       }
  else if (gcodeState == "FINISH")  {
    if (appState != S_DONE) { appState = S_DONE; doneSince = millis(); }
  }
  else if (gcodeState == "IDLE")    {
    // Only go to S_IDLE if we're not already showing the idle screen
    if (appState != S_IDLE_SCREEN) appState = S_IDLE;
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  MQTT reconnect (non-blocking, called every loop)
// ══════════════════════════════════════════════════════════════════════════

void mqttReconnect() {
  if (mqtt.connected()) return;
  unsigned long now = millis();
  if (now - lastMqttRetry < MQTT_RETRY_MS) return;
  lastMqttRetry = now;

  String id = "BambuLCD-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  if (mqtt.connect(id.c_str(), "bblp", BAMBU_ACCESS_CODE)) {
    mqtt.subscribe(reportTopic.c_str());
    Serial.println("[MQTT] Connected → " + reportTopic);
    if (appState == S_CONNECTION_ERR) appState = S_CONNECTING;
  } else {
    Serial.printf("[MQTT] Failed rc=%d\n", mqtt.state());
    if ((appState == S_CONNECTING || appState == S_CONNECTION_ERR) &&
        (now - connectStart > CONNECT_TIMEOUT)) {
      appState = S_CONNECTION_ERR;
    }
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  Button handler
// ══════════════════════════════════════════════════════════════════════════

void onHold() {
  forcedOff = !forcedOff;
  if (forcedOff) {
    lcdOff();
  } else {
    lastDrawnState = (AppState)255; // force a full redraw when waking
  }
}

void onPress() {
  // If screen was force-held off, any press wakes it back up
  if (forcedOff) {
    forcedOff = false;
    lastDrawnState = (AppState)255;
    return;
  }

  switch (appState) {

    // Idle screen off → press once to see IDLE screen
    case S_IDLE:
      appState = S_IDLE_SCREEN;
      break;

    // Idle screen showing → press again to turn screen back off
    case S_IDLE_SCREEN:
      appState = S_IDLE;
      break;

    // Dismiss PRINT DONE! → go idle
    case S_DONE:
      appState = S_IDLE;
      break;

    // Dismiss error popups → go idle (screen will turn off)
    case S_FAILED_POPUP:
    case S_PRINTER_ERR_POPUP:
      printError = 0;
      gcodeState = "IDLE";
      appState   = S_IDLE;
      break;

    // While printing / paused → cycle pages; page stays until pressed again
    case S_PRINTING:
    case S_PAUSED:
      page = (page + 1) % 2;
      break;

    // Anything else: no action needed
    default:
      break;
  }
}

void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) lastDebounce = millis();
  lastReading = reading;
  if ((millis() - lastDebounce) < 50) return;   // debounce

  if (reading != btnState) {
    btnState = reading;
    if (btnState == LOW) {
      // Button just pressed down — start hold timer
      btnPressStart = millis();
      holdFired     = false;
    } else {
      // Button just released — fire press only if hold didn't already fire
      if (!holdFired) onPress();
    }
  }

  // Check for hold while button is still held down
  if (btnState == LOW && !holdFired && (millis() - btnPressStart >= HOLD_MS)) {
    holdFired = true;
    onHold();
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  LCD update  (called every REFRESH_MS)
// ══════════════════════════════════════════════════════════════════════════

void updateLcd() {
  // Long-press forced the screen off — stay off regardless of printer state
  if (forcedOff) { lcdOff(); return; }

  // Anti-flicker: skip redrawing static screens if nothing has changed.
  // Dynamic screens (printing/paused) always redraw since values change.
  bool dynamic = (appState == S_PRINTING || appState == S_PAUSED);
  bool changed = (appState != lastDrawnState) || (page != lastDrawnPage);
  if (!dynamic && !changed) return;
  lastDrawnState = appState;
  lastDrawnPage  = page;

  switch (appState) {

    case S_CONNECTING:
      lcdOn();
      drawConnecting();
      break;

    case S_CONNECTION_ERR:
      lcdOn();
      drawConnectionErr();
      break;

    case S_IDLE:
      lcdOff();
      break;

    case S_IDLE_SCREEN:
      lcdOn();
      drawIdle();
      break;

    case S_PRINTING:
    case S_PAUSED:
      lcdOn();
      (page == 0) ? drawPrintPage() : drawTempsPage();
      break;

    case S_DONE:
      lcdOn();
      drawDone();
      if (millis() - doneSince >= DONE_MS) appState = S_IDLE;
      break;

    case S_FAILED_POPUP:
      lcdOn();
      drawFailed();
      break;

    case S_PRINTER_ERR_POPUP:
      lcdOn();
      drawPrinterErr();
      break;
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  Setup & Loop
// ══════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  Serial.println("\n[BambuLCD] V1 — starting up");

  // Button: wired between GPIO pin and GND; internal pull-up → reads LOW when pressed
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // LCD init
  lcd.init();
  lcd.createChar(0, BLOCK);   // register custom solid-block character
  lcd.backlight();
  lcd.clear();
  drawConnecting();

  // WiFi (blocks until connected)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  Serial.println("\n[WiFi] IP: " + WiFi.localIP().toString());

  // TLS: skip cert verification — Bambu printers use self-signed certs
  secureClient.setInsecure();

  // MQTT setup
  reportTopic = "device/" + String(BAMBU_SERIAL) + "/report";
  mqtt.setServer(BAMBU_IP, 8883);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(8192);   // Bambu JSON payloads are 4–8 KB; default 256 is too small

  connectStart = millis();
}

void loop() {
  // Keep MQTT alive / reconnect if dropped
  if (!mqtt.connected()) mqttReconnect();
  mqtt.loop();

  handleButton();

  // Throttle LCD redraws to avoid flicker
  if (millis() - lastRefresh >= REFRESH_MS) {
    lastRefresh = millis();
    updateLcd();
  }
}
