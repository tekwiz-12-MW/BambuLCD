// ============================================================
//  BambuLCD V1.1
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
//    Print Failed     → "PRINT FAILED! / Btn to dismiss" (once per session)
//    Printer Error    → "PRINTER ERROR! / Btn to dismiss" (once per error code)
//    Connection Error → "CONNECTION ERR! / Check config.h"
//
//  Fixes in V1.1:
//    [1] State machine only fires when gcode_state was in that specific message
//    [2] MQTT buffer raised to 16384, heap-allocated to avoid stack overflow
//    [3] Filters by push_status command — ignores heartbeats/info/AMS messages
//        that falsely carry gcode_state IDLE while printer is printing
//    [4] Non-blocking WiFi reconnect in main loop
//    [5] Dismissed popups never re-appear this session; new error code still shows
//    [6] Stale data indicator (~) if no MQTT message received for 30 s
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

// ── LCD ───────────────────────────────────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);

// ── MQTT ──────────────────────────────────────────────────────────────────
WiFiClientSecure secureClient;
PubSubClient     mqtt(secureClient);

// ── Application state machine ─────────────────────────────────────────────
enum AppState {
  S_CONNECTING,        // booting / waiting for first MQTT connection
  S_IDLE,              // printer idle → screen off
  S_IDLE_SCREEN,       // button pressed from idle → shows "IDLE", btn again → off
  S_PRINTING,          // gcode_state == RUNNING
  S_PAUSED,            // gcode_state == PAUSE
  S_DONE,              // gcode_state == FINISH (brief message then idle)
  S_FAILED_POPUP,      // gcode_state == FAILED (dismiss with button, once per session)
  S_PRINTER_ERR_POPUP, // print_error != 0 (dismiss, won't reshow same error code)
  S_CONNECTION_ERR     // MQTT unreachable
};

AppState appState = S_CONNECTING;

// ── Printer data ──────────────────────────────────────────────────────────
float  nozzleTemp   = 0.0f, nozzleTarget = 0.0f;
float  bedTemp      = 0.0f, bedTarget    = 0.0f;
int    progress     = 0;
int    remainMin    = 0;
int    printError   = 0;
String gcodeState   = "";      // intentionally empty — unset until first real message
bool   dataReceived = false;

// ── Dismiss tracking (persists until power cycle) ─────────────────────────
int  dismissedErrorCode = 0;    // error code user dismissed — won't reshow same code
bool failedDismissed    = false; // true after user dismisses PRINT FAILED this session

// ── Stale data detection ──────────────────────────────────────────────────
unsigned long lastMsgMs = 0;
const unsigned long STALE_MS = 30000; // 30 s without a message = stale

bool isStale() {
  return dataReceived && (millis() - lastMsgMs > STALE_MS);
}

// ── UI ────────────────────────────────────────────────────────────────────
int  page     = 0;
bool screenOn = false;

// ── Button ────────────────────────────────────────────────────────────────
bool          lastReading   = HIGH;
bool          btnState      = HIGH;
unsigned long lastDebounce  = 0;
unsigned long btnPressStart = 0;
bool          holdFired     = false;
bool          forcedOff     = false;

// ── Timing ────────────────────────────────────────────────────────────────
unsigned long lastRefresh   = 0;
unsigned long lastMqttRetry = 0;
unsigned long lastWifiCheck = 0;
unsigned long doneSince     = 0;
unsigned long connectStart  = 0;

const unsigned long REFRESH_MS      =  500;
const unsigned long MQTT_RETRY_MS   = 5000;
const unsigned long WIFI_CHECK_MS   = 10000;
const unsigned long DONE_MS         = 60000;
const unsigned long CONNECT_TIMEOUT = 15000;
const unsigned long HOLD_MS         =  800;

// ── Anti-flicker ──────────────────────────────────────────────────────────
AppState lastDrawnState = (AppState)255;
int      lastDrawnPage  = -1;

// ── MQTT topic ────────────────────────────────────────────────────────────
String reportTopic;


// ══════════════════════════════════════════════════════════════════════════
//  LCD helpers
// ══════════════════════════════════════════════════════════════════════════

void lcdOn()  { if (!screenOn) { lcd.backlight(); lcd.display();   screenOn = true;  } }
void lcdOff() { if (screenOn)  { lcd.noBacklight(); lcd.noDisplay(); screenOn = false; } }

void row(uint8_t r, String s) {
  while ((int)s.length() < 16) s += ' ';
  lcd.setCursor(0, r);
  lcd.print(s.substring(0, 16));
}

String padL(String s, int width) {
  while ((int)s.length() < width) s = ' ' + s;
  return s;
}

String fmtTime(int mins) {
  if (mins <  0) return "--";
  if (mins == 0) return "<1m";
  if (mins < 60) return String(mins) + "m";
  int h = mins / 60, m = mins % 60;
  return String(h) + "h" + (m < 10 ? "0" : "") + String(m) + "m";
}


// ══════════════════════════════════════════════════════════════════════════
//  Page renderers
// ══════════════════════════════════════════════════════════════════════════

//  Row 0:  " 45%[██████────]"
//  Row 1:  "1h23m   PRINTING"  or  "1h23m  ~PRINTNG" when stale
void drawPrintPage() {
  String pct    = padL(String(progress) + "%", 4);
  int    filled = constrain((int)((progress / 100.0f) * 10.0f), 0, 10);

  lcd.setCursor(0, 0);
  lcd.print(pct + "[");
  for (int i = 0; i < 10; i++) {
    if (i < filled) lcd.write(byte(0));
    else            lcd.print('-');
  }
  lcd.print("]");

  // ~ prefix on state when data hasn't updated in 30 s
  String stateStr = isStale()
                    ? ((appState == S_PAUSED) ? "~PAUSED"  : "~PRINTNG")
                    : ((appState == S_PAUSED) ? "PAUSED"   : "PRINTING");
  String timeStr = fmtTime(remainMin);
  String r1      = timeStr;
  int    fill    = 16 - (int)timeStr.length() - (int)stateStr.length();
  for (int i = 0; i < max(1, fill); i++) r1 += ' ';
  r1 += stateStr;
  row(1, r1);
}

//  Row 0:  "Nozzle:210/220  "
//  Row 1:  "Bed:60/60       "
void drawTempsPage() {
  row(0, "Nozzle:" + String((int)nozzleTemp) + "/" + String((int)nozzleTarget));
  row(1, "Bed:"    + String((int)bedTemp)    + "/" + String((int)bedTarget));
}

void drawConnecting()    { row(0, "  BambuLCD V1   "); row(1, "  Connecting... "); }
void drawConnectionErr() { row(0, "CONNECTION ERR! "); row(1, "Check config.h  "); }
void drawPrinterErr()    { row(0, "PRINTER ERROR!  "); row(1, "Btn to dismiss  "); }
void drawFailed()        { row(0, "PRINT FAILED!   "); row(1, "Btn to dismiss  "); }
void drawDone()          { row(0, "  PRINT DONE!   "); row(1, " Btn to dismiss "); }
void drawIdle()          { row(0, "     IDLE       "); row(1, " Btn to sleep   "); }


// ══════════════════════════════════════════════════════════════════════════
//  MQTT message callback
// ══════════════════════════════════════════════════════════════════════════

void onMessage(char* topic, byte* payload, unsigned int length) {

  // FIX [2]: DynamicJsonDocument allocates on the heap, not the stack.
  // StaticJsonDocument<16384> would overflow the ESP32's 8 KB task stack.
  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[MQTT] Parse error: %s  (msg size: %u bytes)\n",
                  err.c_str(), length);
    return;
  }

  JsonObject pr = doc["print"];
  if (pr.isNull()) return;

  dataReceived = true;
  lastMsgMs    = millis();

  // Always update temperatures — safe from any message type ──────────────
  if (!pr["nozzle_temper"].isNull())        nozzleTemp   = pr["nozzle_temper"].as<float>();
  if (!pr["nozzle_target_temper"].isNull()) nozzleTarget = pr["nozzle_target_temper"].as<float>();
  if (!pr["bed_temper"].isNull())           bedTemp      = pr["bed_temper"].as<float>();
  if (!pr["bed_target_temper"].isNull())    bedTarget    = pr["bed_target_temper"].as<float>();

  // FIX [3]: Only process print state from push_status messages.
  // Non-status messages (heartbeat, push_info, AMS updates, get_version)
  // can carry gcode_state "IDLE" even while the printer is actively printing.
  String cmd = pr["command"].isNull() ? "" : pr["command"].as<String>();
  if (cmd != "push_status" && cmd != "") {
    Serial.printf("[MQTT] Skipping non-status command: '%s'\n", cmd.c_str());
    return;
  }

  // FIX [1]: Only update gcodeState if THIS message explicitly contains it.
  // If absent, do not use the stale cached value to drive state transitions.
  bool hasGcodeState = !pr["gcode_state"].isNull();
  if (hasGcodeState) gcodeState = pr["gcode_state"].as<String>();

  if (!pr["mc_percent"].isNull())        progress  = pr["mc_percent"].as<int>();
  if (!pr["mc_remaining_time"].isNull()) remainMin = pr["mc_remaining_time"].as<int>();
  if (!pr["print_error"].isNull())       printError = pr["print_error"].as<int>();

  // FIX [1] continued: bail if no state info was in this message
  if (!hasGcodeState) return;

  // Don't override a popup the user is actively viewing
  if (appState == S_FAILED_POPUP || appState == S_PRINTER_ERR_POPUP) return;

  // Printer hardware error — top priority.
  // FIX [5]: Only shows if it's a NEW error code (not one already dismissed)
  if (printError != 0 && printError != dismissedErrorCode) {
    appState = S_PRINTER_ERR_POPUP;
    return;
  }

  // State transitions
  if (gcodeState == "RUNNING") {
    failedDismissed    = false; // new print → reset dismissed flags
    dismissedErrorCode = 0;
    if (appState != S_PRINTING && appState != S_PAUSED) page = 0;
    appState = S_PRINTING;
  }
  else if (gcodeState == "PAUSE") {
    appState = S_PAUSED;
  }
  else if (gcodeState == "FAILED") {
    // FIX [5]: don't re-show after user dismissed it this session
    if (!failedDismissed) appState = S_FAILED_POPUP;
  }
  else if (gcodeState == "FINISH") {
    if (appState != S_DONE) { appState = S_DONE; doneSince = millis(); }
  }
  else if (gcodeState == "IDLE") {
    if (appState != S_IDLE_SCREEN) appState = S_IDLE;
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  MQTT reconnect (non-blocking)
// ══════════════════════════════════════════════════════════════════════════

void mqttReconnect() {
  if (mqtt.connected()) return;
  unsigned long now = millis();
  if (now - lastMqttRetry < MQTT_RETRY_MS) return;
  lastMqttRetry = now;

  String id = "BambuLCD-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.printf("[MQTT] Connecting to %s ...\n", BAMBU_IP);

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
//  Button
// ══════════════════════════════════════════════════════════════════════════

void onHold() {
  forcedOff = !forcedOff;
  if (forcedOff) {
    lcdOff();
  } else {
    lastDrawnState = (AppState)255; // force full redraw on wake
  }
}

void onPress() {
  if (forcedOff) {
    forcedOff = false;
    lastDrawnState = (AppState)255;
    return;
  }
  switch (appState) {
    case S_IDLE:
      appState = S_IDLE_SCREEN;
      break;
    case S_IDLE_SCREEN:
      appState = S_IDLE;
      break;
    case S_DONE:
      appState = S_IDLE;
      break;
    case S_FAILED_POPUP:
      failedDismissed = true;  // FIX [5]: mark dismissed, won't reappear this session
      gcodeState      = "IDLE";
      appState        = S_IDLE;
      break;
    case S_PRINTER_ERR_POPUP:
      dismissedErrorCode = printError; // FIX [5]: suppress this code until power cycle
      gcodeState         = "IDLE";
      appState           = S_IDLE;
      break;
    case S_PRINTING:
    case S_PAUSED:
      page = (page + 1) % 2;
      break;
    default:
      break;
  }
}

void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) lastDebounce = millis();
  lastReading = reading;
  if ((millis() - lastDebounce) < 50) return;

  if (reading != btnState) {
    btnState = reading;
    if (btnState == LOW) {
      btnPressStart = millis();
      holdFired     = false;
    } else {
      if (!holdFired) onPress();
    }
  }

  if (btnState == LOW && !holdFired && (millis() - btnPressStart >= HOLD_MS)) {
    holdFired = true;
    onHold();
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  LCD update (called every REFRESH_MS)
// ══════════════════════════════════════════════════════════════════════════

void updateLcd() {
  if (forcedOff) { lcdOff(); return; }

  bool dynamic = (appState == S_PRINTING || appState == S_PAUSED);
  bool changed  = (appState != lastDrawnState) || (page != lastDrawnPage);
  if (!dynamic && !changed) return;
  lastDrawnState = appState;
  lastDrawnPage  = page;

  switch (appState) {
    case S_CONNECTING:      lcdOn();  drawConnecting();    break;
    case S_CONNECTION_ERR:  lcdOn();  drawConnectionErr(); break;
    case S_IDLE:            lcdOff();                      break;
    case S_IDLE_SCREEN:     lcdOn();  drawIdle();          break;
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
    case S_FAILED_POPUP:      lcdOn(); drawFailed();     break;
    case S_PRINTER_ERR_POPUP: lcdOn(); drawPrinterErr(); break;
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  Setup & Loop
// ══════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  Serial.println("\n[BambuLCD] V1.1 — starting up");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  lcd.init();
  lcd.createChar(0, BLOCK);
  lcd.backlight();
  lcd.clear();
  drawConnecting();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print('.'); }
  Serial.println("\n[WiFi] IP: " + WiFi.localIP().toString());

  secureClient.setInsecure(); // Bambu uses self-signed certs; no CA to verify against
  reportTopic = "device/" + String(BAMBU_SERIAL) + "/report";
  mqtt.setServer(BAMBU_IP, 8883);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(16384); // FIX [2]: raised from 8192

  connectStart = millis();
}

void loop() {
  // FIX [4]: non-blocking WiFi reconnect
  if (millis() - lastWifiCheck >= WIFI_CHECK_MS) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Lost — reconnecting...");
      WiFi.reconnect();
    }
  }

  if (!mqtt.connected()) mqttReconnect();
  mqtt.loop();

  handleButton();

  if (millis() - lastRefresh >= REFRESH_MS) {
    lastRefresh = millis();
    updateLcd();
  }
}
