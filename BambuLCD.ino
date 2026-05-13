// ============================================================
//  BambuLCD V1.2
//  ESP32 DEVKIT V1  +  1602 I2C LCD (16×2)  +  Push Button
//  Bambu Lab A1 Mini — LAN MQTT over TLS (same method as PrintSphere)
//
//  Pages (button cycles while printing):
//    Page 0 – Print : progress %, bar, time left, stage/state
//    Page 1 – Info  : nozzle temp / target, bed temp / target
//
//  Pre-print stage display (Page 0, row 1):
//    LEVELING   — auto bed leveling
//    HTG BED    — heatbed preheating
//    HEATING    — hotend heating
//    HOMING     — toolhead homing
//    CLEANING   — nozzle tip cleaning
//    DYN FLOW   — dynamic flow calibration
//    FLOW CAL   — extrusion calibration
//    SCAN BED   — bed surface scan
//    1ST LAYER  — first layer inspection
//    CHK TEMP   — extruder temp check
//    LOADING    — filament loading/change
//    PRINTING   — actively printing (no special stage)
//
//  Changes in V1.2:
//    [FIX] Stuck at "Connecting..." — on MQTT connect, publishes a pushall
//          request so the printer immediately sends a full status update.
//          10-second fallback to IDLE if printer still doesn't respond.
//    [NEW] Pre-print stage display via stg_cur field
//    [NEW] Stage-aware stale indicator (~LEVELING, ~HEATING, etc.)
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
  S_CONNECTING,        // booting / waiting for first MQTT data
  S_IDLE,              // printer idle → screen off
  S_IDLE_SCREEN,       // button from idle → shows "IDLE", btn again → off
  S_PRINTING,          // gcode_state == RUNNING
  S_PAUSED,            // gcode_state == PAUSE
  S_DONE,              // gcode_state == FINISH
  S_FAILED_POPUP,      // gcode_state == FAILED (once per session)
  S_PRINTER_ERR_POPUP, // print_error != 0 (once per error code)
  S_CONNECTION_ERR     // MQTT unreachable
};

AppState appState = S_CONNECTING;

// ── Printer data ──────────────────────────────────────────────────────────
float  nozzleTemp   = 0.0f, nozzleTarget = 0.0f;
float  bedTemp      = 0.0f, bedTarget    = 0.0f;
int    progress     = 0;
int    remainMin    = 0;
int    printError   = 0;
int    stgCur       = 255;  // current pre-print stage; 255 = none
String gcodeState   = "";   // intentionally empty until first real message
bool   dataReceived = false;

// ── Dismiss tracking (persists until power cycle) ─────────────────────────
int  dismissedErrorCode = 0;
bool failedDismissed    = false;

// ── Stale data detection ──────────────────────────────────────────────────
unsigned long lastMsgMs = 0;
const unsigned long STALE_MS = 30000;

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
unsigned long lastRefresh      = 0;
unsigned long lastMqttRetry    = 0;
unsigned long lastWifiCheck    = 0;
unsigned long doneSince        = 0;
unsigned long connectStart     = 0;
unsigned long mqttConnectedAt  = 0;  // when MQTT last connected successfully

const unsigned long REFRESH_MS        =  500;
const unsigned long MQTT_RETRY_MS     = 5000;
const unsigned long WIFI_CHECK_MS     = 10000;
const unsigned long DONE_MS           = 60000;
const unsigned long CONNECT_TIMEOUT   = 15000;
const unsigned long PUSHALL_TIMEOUT   = 10000;  // give up waiting for pushall response
const unsigned long HOLD_MS           =  800;

// ── Anti-flicker ──────────────────────────────────────────────────────────
AppState lastDrawnState = (AppState)255;
int      lastDrawnPage  = -1;

// ── MQTT topics ───────────────────────────────────────────────────────────
String reportTopic;
String requestTopic;


// ══════════════════════════════════════════════════════════════════════════
//  LCD helpers
// ══════════════════════════════════════════════════════════════════════════

void lcdOn()  { if (!screenOn) { lcd.backlight(); lcd.display();    screenOn = true;  } }
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
//  Pre-print stage → display string
//  Mapped for A1 Mini (no LIDAR / chamber scanning hardware)
//  All stage names kept to 9 chars max so they always fit row 1
// ══════════════════════════════════════════════════════════════════════════

String getStageStr() {
  if (appState == S_PAUSED) return "PAUSED";

  switch (stgCur) {
    case 1:   return "LEVELING";   // auto bed leveling
    case 2:   return "HTG BED";    // heatbed preheating
    case 4:   return "LOADING";    // filament loading / change
    case 7:   return "HEATING";    // hotend heating
    case 8:   return "FLOW CAL";   // extrusion calibration
    case 9:   return "SCAN BED";   // bed surface scan
    case 10:  return "1ST LAYER";  // first layer inspection
    case 13:  return "HOMING";     // toolhead homing
    case 14:  return "CLEANING";   // nozzle tip cleaning
    case 15:  return "CHK TEMP";   // extruder temperature check
    case 19:  return "DYN FLOW";   // dynamic flow calibration
    default:  return "PRINTING";   // no special stage / unknown
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  Page renderers
// ══════════════════════════════════════════════════════════════════════════

//  Row 0:  " 45%[██████────]"
//  Row 1:  "1h23m   LEVELING"  or  "1h23m  ~LEVELING" when stale
void drawPrintPage() {
  // ── Row 0: progress bar ───────────────────────────────────────────────
  String pct    = padL(String(progress) + "%", 4);
  int    filled = constrain((int)((progress / 100.0f) * 10.0f), 0, 10);

  lcd.setCursor(0, 0);
  lcd.print(pct + "[");
  for (int i = 0; i < 10; i++) {
    if (i < filled) lcd.write(byte(0));
    else            lcd.print('-');
  }
  lcd.print("]");

  // ── Row 1: time + stage label ─────────────────────────────────────────
  String stage    = getStageStr();
  String stateStr = isStale() ? ("~" + stage) : stage;
  String timeStr  = fmtTime(remainMin);
  String r1       = timeStr;
  int    fill     = 16 - (int)timeStr.length() - (int)stateStr.length();
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

  // Always update temperatures — valid from any message type
  if (!pr["nozzle_temper"].isNull())        nozzleTemp   = pr["nozzle_temper"].as<float>();
  if (!pr["nozzle_target_temper"].isNull()) nozzleTarget = pr["nozzle_target_temper"].as<float>();
  if (!pr["bed_temper"].isNull())           bedTemp      = pr["bed_temper"].as<float>();
  if (!pr["bed_target_temper"].isNull())    bedTarget    = pr["bed_target_temper"].as<float>();

  // Only process print state from push_status messages
  String cmd = pr["command"].isNull() ? "" : pr["command"].as<String>();
  if (cmd != "push_status" && cmd != "") {
    Serial.printf("[MQTT] Skipping non-status command: '%s'\n", cmd.c_str());
    return;
  }

  // Only update gcodeState if this specific message contains it
  bool hasGcodeState = !pr["gcode_state"].isNull();
  if (hasGcodeState) gcodeState = pr["gcode_state"].as<String>();

  if (!pr["mc_percent"].isNull())        progress  = pr["mc_percent"].as<int>();
  if (!pr["mc_remaining_time"].isNull()) remainMin = pr["mc_remaining_time"].as<int>();
  if (!pr["print_error"].isNull())       printError = pr["print_error"].as<int>();

  // Parse current pre-print stage (255 = none / between stages)
  if (!pr["stg_cur"].isNull()) stgCur = pr["stg_cur"].as<int>();
  // Some firmware sends stg as an array; stg_cur is the scalar version
  // If your firmware doesn't send stg_cur, stgCur stays 255 (shows "PRINTING")

  // Bail if no state info was in this message
  if (!hasGcodeState) return;

  // Don't override a popup the user is viewing
  if (appState == S_FAILED_POPUP || appState == S_PRINTER_ERR_POPUP) return;

  // Hardware error — top priority, only if new/different error code
  if (printError != 0 && printError != dismissedErrorCode) {
    appState = S_PRINTER_ERR_POPUP;
    return;
  }

  if (gcodeState == "RUNNING") {
    failedDismissed    = false;
    dismissedErrorCode = 0;
    if (appState != S_PRINTING && appState != S_PAUSED) page = 0;
    appState = S_PRINTING;
  }
  else if (gcodeState == "PAUSE") {
    appState = S_PAUSED;
  }
  else if (gcodeState == "FAILED") {
    if (!failedDismissed) appState = S_FAILED_POPUP;
  }
  else if (gcodeState == "FINISH") {
    if (appState != S_DONE) { appState = S_DONE; doneSince = millis(); }
  }
  else if (gcodeState == "IDLE") {
    stgCur = 255; // clear stage on idle
    if (appState != S_IDLE_SCREEN) appState = S_IDLE;
  }
}


// ══════════════════════════════════════════════════════════════════════════
//  MQTT reconnect + pushall request
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
    mqttConnectedAt = millis();
    Serial.println("[MQTT] Connected → " + reportTopic);

    // ── FIX: Stuck at connecting ──────────────────────────────────────
    // Request a full status dump from the printer right now.
    // Without this, an idle printer won't send push_status on its own,
    // and the display would sit at "Connecting..." forever.
    // The printer responds with a complete push_status message within ~1 s.
    String payload = "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}";
    bool sent = mqtt.publish(requestTopic.c_str(), payload.c_str());
    Serial.printf("[MQTT] pushall request %s\n", sent ? "sent" : "FAILED");

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
    lastDrawnState = (AppState)255;
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
      failedDismissed = true;
      gcodeState      = "IDLE";
      appState        = S_IDLE;
      break;
    case S_PRINTER_ERR_POPUP:
      dismissedErrorCode = printError;
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
  // ── Pushall fallback: if connected but no push_status after 10s → IDLE ─
  // This fires if the printer is idle and doesn't respond to pushall,
  // or if the pushall response didn't contain gcode_state.
  if (appState == S_CONNECTING && mqtt.connected() &&
      mqttConnectedAt > 0 && (millis() - mqttConnectedAt > PUSHALL_TIMEOUT)) {
    Serial.println("[BambuLCD] No status after pushall — assuming IDLE");
    appState = S_IDLE;
  }

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
  Serial.begin(115200);  // ← Serial Monitor must be set to 115200 baud
  Serial.println("\n[BambuLCD] V1.2 — starting up");

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

  secureClient.setInsecure();

  reportTopic  = "device/" + String(BAMBU_SERIAL) + "/report";
  requestTopic = "device/" + String(BAMBU_SERIAL) + "/request";

  mqtt.setServer(BAMBU_IP, 8883);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(16384);

  connectStart = millis();
}

void loop() {
  // Non-blocking WiFi reconnect check
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
