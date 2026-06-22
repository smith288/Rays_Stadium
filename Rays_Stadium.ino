#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <string.h>
#include <time.h>

#include "web_assets.h"

// Install the ArduinoJson library from Library Manager if it is not already
// installed. ESP32 boards include WiFi, HTTPClient, and WiFiClientSecure.

// Wi-Fi credentials are provisioned at runtime via the setup AP and stored in
// NVS, so they are no longer hardcoded here.
const char* OTA_HOSTNAME = "rays-stadium";
const char* OTA_PASSWORD = "splach78";  // Optional: set a password before uploading.

constexpr int RAYS_TEAM_ID = 139;
constexpr unsigned long CHECK_INTERVAL_MS = 60UL * 1000UL;
constexpr unsigned long BACKOFF_CHECK_INTERVAL_MS = 30UL * 60UL * 1000UL;
constexpr unsigned long WIFI_RETRY_MS = 5000UL;
constexpr unsigned long HTTP_TIMEOUT_MS = 10000UL;
constexpr int SCORE_FLASH_SECONDS = 15;
constexpr int WIN_FLASH_SECONDS = 20;
constexpr int FLASH_INTERVAL_MS = 500;
constexpr int MAX_API_FLASH_SECONDS = 120;
constexpr int PREGAME_LIGHT_OFF_LEAD_SECONDS = 60 * 60;
constexpr unsigned long SETUP_LIGHT_INTERVAL_MS = 10UL * 1000UL;
// Re-check the most recent final while waiting for today's game, so a prior
// game that crossed midnight is reflected once it actually goes Final.
constexpr unsigned long PREVIOUS_FINAL_RECHECK_MS = 10UL * 60UL * 1000UL;

#ifndef D10
#define D10 18
#endif

#ifndef D2
#define D2 2
#endif

#ifndef D4
#define D4 22
#endif

// Connect this pin to a relay, MOSFET, or USB power switch input. Do not power
// the light directly from a GPIO pin.
constexpr int LIGHT_CONTROL_PIN = D4;
constexpr bool LIGHT_ACTIVE_HIGH = true;
constexpr int STARTUP_LED_PIN = 15;  // XIAO ESP32C6 user LED / "Light" pin.
constexpr bool STARTUP_LED_ACTIVE_LOW = true;
constexpr int STARTUP_LED_BLINKS = 3;
constexpr int STARTUP_LED_BLINK_MS = 150;

const char* MLB_SCHEDULE_URL = "https://statsapi.mlb.com/api/v1/schedule";
const char* MLB_LIVE_FEED_BASE_URL = "https://statsapi.mlb.com/api/v1.1/game/";

// Tampa Bay / Eastern time. This keeps the "today" query aligned with the Rays.
const char* EASTERN_TIMEZONE = "EST5EDT,M3.2.0,M11.1.0";

// Open AP exposed for first-time / recovery Wi-Fi provisioning.
const char* SETUP_AP_SSID = "Rays-Stadium-Setup";
constexpr byte DNS_PORT = 53;
constexpr char WIFI_PREFS_NAMESPACE[] = "wifi";

struct GameInfo {
  bool hasGame = false;
  int gamePk = -1;
  bool raysHome = false;
  bool final = false;
  bool live = false;
  bool raysWon = false;
  int raysScore = -1;
  int opponentScore = -1;
  time_t startEpoch = 0;
  String detailedState;
};

struct ScheduleInfo {
  bool fetchOk = false;
  GameInfo currentGame;
  GameInfo nextGame;
};

int currentGamePk = -1;
int lastRaysRunsSeen = -1;
bool finalHandledForCurrentGame = false;
bool clockSynced = false;
bool otaStarted = false;
unsigned long currentPollIntervalMs = CHECK_INTERVAL_MS;
unsigned long lastPollMs = 0;
unsigned long lastWifiAttemptMs = 0;
time_t backoffUntilEpoch = 0;
int pregameLightOffGamePk = -1;
bool pregameLightOffApplied = false;
int previousFinalForGamePk = -1;
int previousFinalAppliedPk = -1;
unsigned long lastPreviousFinalCheckMs = 0;
GameInfo latestGame;
GameInfo latestNextGame;
WebServer webServer(80);
DNSServer dnsServer;
Preferences preferences;
bool webServerStarted = false;
bool apSetupMode = false;
bool lightIsOn = false;
bool automaticLightOn = false;
bool manualLightOverride = false;
bool otaUpdateInProgress = false;
unsigned long manualLightExpiresAtMs = 0;
unsigned long setupLightToggledAtMs = 0;
unsigned long flashEndsAtMs = 0;
unsigned long flashNextToggleAtMs = 0;
bool flashPatternOn = false;
String wifiSsid;
String wifiPassword;

String formatLocalTime(time_t value);
String recentScoreboardScheduleUrl();
String upcomingScheduleUrl();
void setStartupLed(bool on);
void handleOta();
void handleWebServer();
void startApSetupMode();
void handleHomePage();
void handleLastGameRequest();
bool addLastGameJson(JsonDocument& response);

void logMessage(const String& message) {
  struct tm timeInfo;
  if (clockSynced && getLocalTime(&timeInfo, 10)) {
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeInfo);
    Serial.print("[");
    Serial.print(timestamp);
    Serial.print("] ");
  }

  Serial.println(message);
}

void feedWatchdog() {
  esp_task_wdt_reset();
}

void loadWifiCredentials() {
  preferences.begin(WIFI_PREFS_NAMESPACE, true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("pass", "");
  preferences.end();
}

void saveWifiCredentials(const String& ssid, const String& password) {
  preferences.begin(WIFI_PREFS_NAMESPACE, false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.end();
  logMessage("Saved Wi-Fi credentials for \"" + ssid + "\". Rebooting.");
}

void clearWifiCredentials() {
  preferences.begin(WIFI_PREFS_NAMESPACE, false);
  preferences.clear();
  preferences.end();
  wifiSsid = "";
  wifiPassword = "";
  logMessage("Cleared stored Wi-Fi credentials.");
}

void setLight(bool on) {
  const bool pinHigh = LIGHT_ACTIVE_HIGH ? on : !on;
  digitalWrite(LIGHT_CONTROL_PIN, pinHigh ? HIGH : LOW);
  lightIsOn = on;
  setStartupLed(on);
}

bool manualLightOverrideExpired() {
  return manualLightOverride &&
         manualLightExpiresAtMs != 0 &&
         static_cast<long>(millis() - manualLightExpiresAtMs) >= 0;
}

void cancelManualLightOverride() {
  manualLightOverride = false;
  manualLightExpiresAtMs = 0;
}

void applyAutomaticLightIfAllowed() {
  if (manualLightOverride && !manualLightOverrideExpired()) {
    return;
  }

  if (manualLightOverrideExpired()) {
    cancelManualLightOverride();
  }

  setLight(automaticLightOn);
}

void setAutomaticLight(bool on) {
  automaticLightOn = on;
  applyAutomaticLightIfAllowed();
}

void setStartupLed(bool on) {
  const bool pinHigh = STARTUP_LED_ACTIVE_LOW ? !on : on;
  digitalWrite(STARTUP_LED_PIN, pinHigh ? HIGH : LOW);
}

void blinkStartupLed() {
  for (int blink = 0; blink < STARTUP_LED_BLINKS; blink++) {
    setStartupLed(true);
    delay(STARTUP_LED_BLINK_MS);
    setStartupLed(false);
    delay(STARTUP_LED_BLINK_MS);
  }
}

void delayWithOta(unsigned long delayMs) {
  const unsigned long endAt = millis() + delayMs;
  while (millis() < endAt) {
    feedWatchdog();
    handleOta();
    handleWebServer();
    delay(10);
  }
}

bool flashLightActive() {
  return flashEndsAtMs != 0 && static_cast<long>(millis() - flashEndsAtMs) < 0;
}

void cancelFlashLight() {
  flashEndsAtMs = 0;
  flashNextToggleAtMs = 0;
}

void startFlashLight(int durationSeconds) {
  if (durationSeconds <= 0) {
    durationSeconds = SCORE_FLASH_SECONDS;
  }

  cancelManualLightOverride();
  flashEndsAtMs = millis() + static_cast<unsigned long>(durationSeconds) * 1000UL;
  flashNextToggleAtMs = 0;
  flashPatternOn = false;
}

void maintainFlashLight() {
  if (!flashLightActive()) {
    if (flashEndsAtMs != 0) {
      flashEndsAtMs = 0;
      flashNextToggleAtMs = 0;
      applyAutomaticLightIfAllowed();
    }
    return;
  }

  const unsigned long now = millis();
  if (flashNextToggleAtMs == 0 || now >= flashNextToggleAtMs) {
    flashPatternOn = !flashPatternOn;
    setLight(flashPatternOn);
    flashNextToggleAtMs = now + FLASH_INTERVAL_MS;
  }
}

void flashLight(int durationSeconds) {
  startFlashLight(durationSeconds);
  while (flashLightActive()) {
    feedWatchdog();
    handleOta();
    handleWebServer();
    maintainFlashLight();
    delay(10);
  }
}

void setBackoffUntil(time_t untilEpoch, const String& reason) {
  backoffUntilEpoch = untilEpoch;
  currentPollIntervalMs = BACKOFF_CHECK_INTERVAL_MS;
  logMessage(reason + " Backing off API polling until " + formatLocalTime(untilEpoch) + ".");
  logMessage("Using 30-minute backoff check interval.");
}

void forceNextPollSoon() {
  lastPollMs = millis() - currentPollIntervalMs;
}

void applyPregameLightOff(int gamePk) {
  if (pregameLightOffGamePk != gamePk) {
    pregameLightOffApplied = false;
    pregameLightOffGamePk = gamePk;
  }

  if (!pregameLightOffApplied) {
    pregameLightOffApplied = true;
    setAutomaticLight(false);
    logMessage("Pregame cutoff reached (1 hour before first pitch). Light off.");
  }
}

void maintainPregameLight() {
  if (apSetupMode) {
    return;
  }

  time_t targetStartEpoch = 0;
  int targetGamePk = -1;

  if (latestGame.hasGame && latestGame.startEpoch > 0) {
    if (latestGame.final && finalHandledForCurrentGame) {
      if (!latestNextGame.hasGame || latestNextGame.startEpoch <= 0) {
        return;
      }
      targetStartEpoch = latestNextGame.startEpoch;
      targetGamePk = latestNextGame.gamePk;
    } else {
      targetStartEpoch = latestGame.startEpoch;
      targetGamePk = latestGame.gamePk;
    }
  } else {
    return;
  }

  const time_t now = time(nullptr);
  const time_t pregameCutoff = targetStartEpoch - PREGAME_LIGHT_OFF_LEAD_SECONDS;
  if (now >= pregameCutoff) {
    applyPregameLightOff(targetGamePk);
  }
}

void maintainTimedStates() {
  const time_t now = time(nullptr);

  if (manualLightOverrideExpired()) {
    cancelManualLightOverride();
    applyAutomaticLightIfAllowed();
    logMessage("Manual light override expired. Resuming automatic light control.");
  }

  maintainPregameLight();

  if (backoffUntilEpoch != 0 && now >= backoffUntilEpoch) {
    backoffUntilEpoch = 0;
    currentPollIntervalMs = CHECK_INTERVAL_MS;
    logMessage("Backoff window ended. Resuming normal API polling.");
    forceNextPollSoon();
  }

  maintainFlashLight();
}

void handleOta() {
  if (otaStarted && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
  }
}

void startOtaIfNeeded() {
  if (otaStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    otaUpdateInProgress = true;
    setLight(false);
    logMessage("OTA update starting.");
  });

  ArduinoOTA.onEnd([]() {
    logMessage("OTA update finished. Rebooting.");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPercent = 101;
    const unsigned int percent = (progress * 100U) / total;
    if (percent != lastPercent && percent % 10U == 0) {
      lastPercent = percent;
      logMessage("OTA progress: " + String(percent) + "%");
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    logMessage("OTA error: " + String(static_cast<int>(error)));
  });

  ArduinoOTA.begin();
  otaStarted = true;
  logMessage("OTA ready as " + String(OTA_HOSTNAME) + ".local");
}

void addCorsHeaders() {
  webServer.sendHeader("Access-Control-Allow-Origin", "*");
  webServer.sendHeader("Access-Control-Allow-Methods", "GET, PUT, OPTIONS");
  webServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void sendJson(JsonDocument& document, int statusCode = 200) {
  String response;
  serializeJson(document, response);
  addCorsHeaders();
  webServer.send(statusCode, "application/json", response);
}

void addStatusJson(JsonDocument& document) {
  document["hostname"] = OTA_HOSTNAME;
  document["ip"] = WiFi.localIP().toString();

  JsonObject wifi = document.createNestedObject("wifi");
  wifi["mode"] = apSetupMode ? "setup-ap" : "station";
  if (apSetupMode) {
    wifi["ssid"] = SETUP_AP_SSID;
    wifi["apIp"] = WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    wifi["ssid"] = WiFi.SSID();
  } else {
    wifi["ssid"] = wifiSsid;
  }

  JsonObject light = document.createNestedObject("light");
  light["on"] = lightIsOn;
  light["automaticOn"] = automaticLightOn;
  light["controlPin"] = "D4";
  light["manualOverride"] = manualLightOverride && !manualLightOverrideExpired();
  light["flashing"] = flashLightActive();

  if (manualLightOverride && manualLightExpiresAtMs != 0 && !manualLightOverrideExpired()) {
    const unsigned long remainingMs = manualLightExpiresAtMs - millis();
    light["manualRemainingSeconds"] = (remainingMs + 999UL) / 1000UL;
  } else {
    light["manualRemainingSeconds"] = nullptr;
  }

  if (flashLightActive()) {
    const unsigned long remainingMs = flashEndsAtMs - millis();
    light["flashRemainingSeconds"] = (remainingMs + 999UL) / 1000UL;
  } else {
    light["flashRemainingSeconds"] = nullptr;
  }

  JsonObject rays = document.createNestedObject("rays");
  rays["hasGame"] = latestGame.hasGame;

  if (!latestGame.hasGame) {
    return;
  }

  rays["gamePk"] = latestGame.gamePk;
  rays["status"] = latestGame.detailedState;
  rays["live"] = latestGame.live;
  rays["final"] = latestGame.final;
  rays["raysWon"] = latestGame.raysWon;
  rays["raysHome"] = latestGame.raysHome;

  if (latestGame.raysScore >= 0 && latestGame.opponentScore >= 0) {
    rays["raysScore"] = latestGame.raysScore;
    rays["opponentScore"] = latestGame.opponentScore;
  } else {
    rays["raysScore"] = nullptr;
    rays["opponentScore"] = nullptr;
  }

  if (latestGame.startEpoch > 0) {
    rays["startTime"] = formatLocalTime(latestGame.startEpoch);
    const time_t pregameCutoff = latestGame.startEpoch - PREGAME_LIGHT_OFF_LEAD_SECONDS;
    rays["pregameCutoff"] = formatLocalTime(pregameCutoff);
    const time_t now = time(nullptr);
    rays["secondsUntilPregameCutoff"] = pregameCutoff > now ? static_cast<long>(pregameCutoff - now) : 0;
  } else {
    rays["startTime"] = nullptr;
    rays["pregameCutoff"] = nullptr;
    rays["secondsUntilPregameCutoff"] = nullptr;
  }

  rays["pregameLightOffApplied"] = pregameLightOffApplied;
}

void handleStatusRequest() {
  DynamicJsonDocument document(768);
  addStatusJson(document);
  sendJson(document);
}

void handleLightPutRequest() {
  DynamicJsonDocument request(256);
  const DeserializationError error = deserializeJson(request, webServer.arg("plain"));

  if (error) {
    DynamicJsonDocument response(256);
    response["error"] = "Invalid JSON body.";
    sendJson(response, 400);
    return;
  }

  const bool wantsFlash = request["flash"].is<bool>() && request["flash"].as<bool>();

  if (wantsFlash) {
    long flashSeconds = request["seconds"] | SCORE_FLASH_SECONDS;
    if (flashSeconds <= 0) {
      flashSeconds = SCORE_FLASH_SECONDS;
    } else if (flashSeconds > MAX_API_FLASH_SECONDS) {
      flashSeconds = MAX_API_FLASH_SECONDS;
    }

    startFlashLight(static_cast<int>(flashSeconds));
    logMessage("Manual API flash requested for " + String(flashSeconds) + " seconds.");

    DynamicJsonDocument response(768);
    addStatusJson(response);
    sendJson(response);
    return;
  }

  if (!request["light"].is<bool>()) {
    DynamicJsonDocument response(256);
    response["error"] =
      "Expected JSON body like {\"light\": true}, {\"light\": true, \"duration\": 5}, or {\"flash\": true, \"seconds\": 15}.";
    sendJson(response, 400);
    return;
  }

  cancelFlashLight();

  const bool requestedLightOn = request["light"].as<bool>();
  const bool hasDuration = !request["duration"].isNull();

  if (hasDuration) {
    const long durationMinutes = request["duration"] | 0;
    if (durationMinutes < 0) {
      DynamicJsonDocument response(256);
      response["error"] = "duration must be zero or greater.";
      sendJson(response, 400);
      return;
    }

    cancelManualLightOverride();

    if (!requestedLightOn) {
      setLight(false);
      logMessage("Manual API request turned light off.");
    } else {
      setLight(true);
      if (durationMinutes > 0) {
        manualLightOverride = true;
        manualLightExpiresAtMs = millis() + static_cast<unsigned long>(durationMinutes) * 60UL * 1000UL;
        logMessage("Manual light override on for " + String(durationMinutes) + " minutes.");
      } else {
        logMessage("Manual toggle turned light on; automatic control still active.");
      }
    }
  } else if (!requestedLightOn) {
    setLight(false);
    logMessage("Manual toggle turned light off; automatic control still active.");
  } else {
    setLight(true);
    logMessage("Manual toggle turned light on; automatic control still active.");
  }

  DynamicJsonDocument response(768);
  addStatusJson(response);
  sendJson(response);
}

void handleOptionsRequest() {
  addCorsHeaders();
  webServer.send(204);
}

void handleSetupPage();

void handleNotFound() {
  if (apSetupMode) {
    handleSetupPage();
    return;
  }

  DynamicJsonDocument response(256);
  response["error"] = "Not found. Use GET /api/status or PUT /api/light (on/off/flash).";
  sendJson(response, 404);
}

void handleHomePage() {
  addCorsHeaders();
  webServer.send_P(200, "text/html", HOME_PAGE_HTML);
}

void handleLastGameRequest() {
  DynamicJsonDocument response(12288);
  const bool ok = addLastGameJson(response);
  sendJson(response, ok ? 200 : 503);
}

const char SETUP_PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Rays Stadium Wi-Fi Setup</title>
<style>
  :root { color-scheme: light dark; }
  body { font-family: -apple-system, system-ui, sans-serif; margin: 0; padding: 1.5rem;
         max-width: 30rem; margin-inline: auto; }
  h1 { font-size: 1.4rem; }
  label { display: block; margin: 1rem 0 0.25rem; font-weight: 600; }
  select, input, button { width: 100%; padding: 0.6rem; font-size: 1rem; box-sizing: border-box;
         border-radius: 0.5rem; border: 1px solid #8888; }
  button { margin-top: 1.25rem; background: #092c5c; color: #fff; border: none; font-weight: 600; }
  button:disabled { opacity: 0.6; }
  .row { display: flex; gap: 0.5rem; align-items: end; }
  .row button { width: auto; margin-top: 0; }
  #status { margin-top: 1rem; min-height: 1.2rem; }
</style>
</head>
<body>
<h1>Rays Stadium Setup</h1>
<p>Select your Wi-Fi network and enter the password.</p>
<div class="row">
  <div style="flex:1">
    <label for="ssid">Network</label>
    <select id="ssid"><option>Scanning...</option></select>
  </div>
  <button id="rescan" type="button">Rescan</button>
</div>
<label for="pass">Password</label>
<input id="pass" type="password" autocomplete="off" placeholder="Wi-Fi password">
<button id="save" type="button">Save &amp; Connect</button>
<p id="status"></p>
<script>
const ssidSel = document.getElementById('ssid');
const statusEl = document.getElementById('status');
async function scan() {
  ssidSel.innerHTML = '<option>Scanning...</option>';
  try {
    const res = await fetch('/api/wifi/scan');
    const nets = await res.json();
    ssidSel.innerHTML = '';
    if (!nets.length) { ssidSel.innerHTML = '<option value="">No networks found</option>'; return; }
    nets.sort((a,b) => b.rssi - a.rssi);
    for (const n of nets) {
      const o = document.createElement('option');
      o.value = n.ssid;
      o.textContent = n.ssid + (n.secure ? ' \uD83D\uDD12' : '');
      ssidSel.appendChild(o);
    }
  } catch (e) { ssidSel.innerHTML = '<option value="">Scan failed</option>'; }
}
async function save() {
  const ssid = ssidSel.value;
  const password = document.getElementById('pass').value;
  if (!ssid) { statusEl.textContent = 'Pick a network first.'; return; }
  statusEl.textContent = 'Saving...';
  try {
    const res = await fetch('/api/wifi', {
      method: 'PUT', headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ ssid, password })
    });
    if (res.ok) { statusEl.textContent = 'Saved. Device is rebooting and joining your network.'; }
    else { const j = await res.json().catch(() => ({})); statusEl.textContent = j.error || 'Save failed.'; }
  } catch (e) { statusEl.textContent = 'Saved. Device is rebooting.'; }
}
document.getElementById('rescan').addEventListener('click', scan);
document.getElementById('save').addEventListener('click', save);
scan();
</script>
</body>
</html>)HTML";

void handleSetupPage() {
  addCorsHeaders();
  webServer.send_P(200, "text/html", SETUP_PAGE_HTML);
}

void handleWifiScanRequest() {
  const int count = WiFi.scanNetworks();
  DynamicJsonDocument document(4096);
  JsonArray networks = document.to<JsonArray>();

  for (int i = 0; i < count; i++) {
    JsonObject network = networks.createNestedObject();
    network["ssid"] = WiFi.SSID(i);
    network["rssi"] = WiFi.RSSI(i);
    network["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }

  WiFi.scanDelete();
  sendJson(document);
}

void handleWifiPutRequest() {
  DynamicJsonDocument request(512);
  const DeserializationError error = deserializeJson(request, webServer.arg("plain"));

  const String ssid = request["ssid"] | "";
  if (error || ssid.length() == 0) {
    DynamicJsonDocument response(256);
    response["error"] = "Expected JSON body like {\"ssid\":\"name\",\"password\":\"secret\"}.";
    sendJson(response, 400);
    return;
  }

  const String password = request["password"] | "";

  DynamicJsonDocument response(256);
  response["ok"] = true;
  response["message"] = "Credentials saved. Rebooting to connect.";
  sendJson(response);

  saveWifiCredentials(ssid, password);
  delay(500);
  ESP.restart();
}

void handleWifiResetRequest() {
  DynamicJsonDocument response(256);
  response["ok"] = true;
  response["message"] = "Wi-Fi credentials cleared. Rebooting into setup mode.";
  sendJson(response);

  clearWifiCredentials();
  delay(500);
  ESP.restart();
}

void startWebServerIfNeeded() {
  if (webServerStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  webServer.on("/", HTTP_GET, handleHomePage);
  webServer.on("/api/status", HTTP_GET, handleStatusRequest);
  webServer.on("/api/last-game", HTTP_GET, handleLastGameRequest);
  webServer.on("/api/light", HTTP_PUT, handleLightPutRequest);
  webServer.on("/api/wifi/reset", HTTP_POST, handleWifiResetRequest);
  webServer.on("/api/status", HTTP_OPTIONS, handleOptionsRequest);
  webServer.on("/api/last-game", HTTP_OPTIONS, handleOptionsRequest);
  webServer.on("/api/light", HTTP_OPTIONS, handleOptionsRequest);
  webServer.on("/api/wifi/reset", HTTP_OPTIONS, handleOptionsRequest);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  webServerStarted = true;
  logMessage("Web API ready at http://" + WiFi.localIP().toString() + "/api/status");
}

void startApSetupMode() {
  if (apSetupMode) {
    return;
  }

  apSetupMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_SSID);
  const IPAddress apIp = WiFi.softAPIP();

  dnsServer.start(DNS_PORT, "*", apIp);

  webServer.on("/", HTTP_GET, handleSetupPage);
  webServer.on("/api/wifi/scan", HTTP_GET, handleWifiScanRequest);
  webServer.on("/api/wifi", HTTP_PUT, handleWifiPutRequest);
  webServer.on("/api/wifi", HTTP_OPTIONS, handleOptionsRequest);
  // Captive-portal detection endpoints used by phones/laptops.
  webServer.on("/generate_204", HTTP_GET, handleSetupPage);
  webServer.on("/hotspot-detect.html", HTTP_GET, handleSetupPage);
  webServer.on("/redirect", HTTP_GET, handleSetupPage);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  webServerStarted = true;

  setLight(true);
  setupLightToggledAtMs = millis();

  logMessage("Setup AP \"" + String(SETUP_AP_SSID) + "\" started at http://" + apIp.toString());
}

void maintainSetupLight() {
  const unsigned long now = millis();
  if (now - setupLightToggledAtMs >= SETUP_LIGHT_INTERVAL_MS) {
    setupLightToggledAtMs = now;
    setLight(!lightIsOn);
  }
}

void handleWebServer() {
  if (otaUpdateInProgress) {
    return;
  }

  if (apSetupMode) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    return;
  }

  if (webServerStarted && WiFi.status() == WL_CONNECTED) {
    webServer.handleClient();
  }
}

void connectWiFi() {
  if (apSetupMode) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    startOtaIfNeeded();
    startWebServerIfNeeded();
    return;
  }

  const unsigned long now = millis();
  if (lastWifiAttemptMs != 0 && now - lastWifiAttemptMs < WIFI_RETRY_MS) {
    return;
  }

  lastWifiAttemptMs = now;
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  logMessage("Connecting to Wi-Fi \"" + wifiSsid + "\"...");

  const unsigned long timeoutAt = millis() + 15000UL;
  while (WiFi.status() != WL_CONNECTED && millis() < timeoutAt) {
    feedWatchdog();
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    logMessage("Wi-Fi connected: " + WiFi.localIP().toString());
    startOtaIfNeeded();
    startWebServerIfNeeded();
  } else {
    logMessage("Wi-Fi connection timed out; will retry.");
  }
}

bool syncClock() {
  configTzTime(EASTERN_TIMEZONE, "pool.ntp.org", "time.nist.gov");

  struct tm timeInfo;
  for (int attempt = 0; attempt < 20; attempt++) {
    feedWatchdog();
    if (getLocalTime(&timeInfo, 500)) {
      logMessage("Clock synchronized.");
      return true;
    }
  }

  logMessage("Clock sync failed; MLB date queries may be wrong.");
  return false;
}

String todayEasternDate() {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 10)) {
    return "";
  }

  char dateBuffer[11];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", &timeInfo);
  return String(dateBuffer);
}

String easternDateWithDayOffset(int dayOffset) {
  time_t value = time(nullptr) + static_cast<time_t>(dayOffset) * 24 * 60 * 60;
  struct tm timeInfo;
  if (!localtime_r(&value, &timeInfo)) {
    return "";
  }

  char dateBuffer[11];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y-%m-%d", &timeInfo);
  return String(dateBuffer);
}

int daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<int>(dayOfEra) - 719468;
}

time_t parseMlbUtcEpoch(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return 0;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  // Prefer sscanf without a trailing literal: some C libraries reject
  // fractional seconds (".000Z") when the format ends with "Z".
  if (sscanf(value, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return 0;
  }

  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
    return 0;
  }

  const int days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  return static_cast<time_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
}

int parseInningOrdinal(JsonVariantConst value) {
  if (value.is<int>()) {
    return value.as<int>();
  }

  const char* text = value.as<const char*>();
  if (text == nullptr || text[0] == '\0') {
    return 0;
  }

  char* end = nullptr;
  const long parsed = strtol(text, &end, 10);
  if (end != text) {
    return static_cast<int>(parsed);
  }

  return 0;
}

String formatLocalTime(time_t value) {
  struct tm timeInfo;
  if (!localtime_r(&value, &timeInfo)) {
    return "unknown time";
  }

  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &timeInfo);
  return String(timestamp);
}

time_t tomorrowNoonEpoch() {
  time_t now = time(nullptr);
  struct tm timeInfo;
  if (!localtime_r(&now, &timeInfo)) {
    return now + 24 * 60 * 60;
  }

  timeInfo.tm_sec = 0;
  timeInfo.tm_min = 0;
  timeInfo.tm_hour = 12;
  timeInfo.tm_mday += 1;
  return mktime(&timeInfo);
}

bool fetchJson(const String& url, JsonDocument& document) {
  if (WiFi.status() != WL_CONNECTED) {
    logMessage("Skipping request; Wi-Fi is disconnected.");
    return false;
  }

  feedWatchdog();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(HTTP_TIMEOUT_MS);

  if (!https.begin(client, url)) {
    logMessage("Could not start HTTPS request.");
    return false;
  }

  feedWatchdog();
  const int statusCode = https.GET();
  feedWatchdog();

  if (statusCode != HTTP_CODE_OK) {
    logMessage("HTTP request failed with status " + String(statusCode));
    https.end();
    return false;
  }

  const DeserializationError error = deserializeJson(document, https.getStream());
  https.end();
  feedWatchdog();

  if (error) {
    logMessage("JSON parse failed: " + String(error.c_str()));
    return false;
  }

  return true;
}

String scheduleUrlForToday() {
  const String today = todayEasternDate();
  if (today.length() == 0) {
    return "";
  }

  String url = String(MLB_SCHEDULE_URL);
  url += "?sportId=1&teamId=" + String(RAYS_TEAM_ID);
  url += "&date=" + today;
  // Request the full game object so gameDate is always present on-device.
  return url;
}

GameInfo gameInfoFromJson(JsonObject game) {
  GameInfo info;

  if (game.isNull()) {
    return info;
  }

  const int homeTeamId = game["teams"]["home"]["team"]["id"] | -1;
  const int awayTeamId = game["teams"]["away"]["team"]["id"] | -1;
  const bool raysHome = homeTeamId == RAYS_TEAM_ID;
  const bool raysAway = awayTeamId == RAYS_TEAM_ID;

  if (!raysHome && !raysAway) {
    return info;
  }

  const char* abstractState = game["status"]["abstractGameState"] | "";
  const char* detailedState = game["status"]["detailedState"] | "";
  const String gameDate = game["gameDate"] | "";

  info.hasGame = true;
  info.gamePk = game["gamePk"] | -1;
  info.raysHome = raysHome;
  info.final = strcmp(abstractState, "Final") == 0 || strcmp(detailedState, "Final") == 0;
  info.live = strcmp(abstractState, "Live") == 0;
  if (!info.live && !info.final) {
    info.live = strstr(detailedState, "In Progress") != nullptr;
  }
  info.startEpoch = parseMlbUtcEpoch(gameDate.c_str());
  if (info.startEpoch <= 0 && gameDate.length() > 0) {
    logMessage("Could not parse gameDate \"" + gameDate + "\" for game " + String(info.gamePk) + ".");
  }
  info.detailedState = String(detailedState);

  const char* raysSide = raysHome ? "home" : "away";
  const char* opponentSide = raysHome ? "away" : "home";
  info.raysWon = game["teams"][raysSide]["isWinner"] | false;
  info.raysScore = game["teams"][raysSide]["score"] | -1;
  info.opponentScore = game["teams"][opponentSide]["score"] | -1;

  return info;
}

ScheduleInfo getTodaySchedule() {
  ScheduleInfo schedule;
  const String url = scheduleUrlForToday();
  if (url.length() == 0) {
    logMessage("No local date available for schedule request.");
    return schedule;
  }

  DynamicJsonDocument document(32768);
  if (!fetchJson(url, document)) {
    return schedule;
  }
  schedule.fetchOk = true;

  JsonArray games = document["dates"][0]["games"].as<JsonArray>();
  if (games.isNull() || games.size() == 0) {
    logMessage("No Rays game scheduled today.");
    return schedule;
  }

  const time_t now = time(nullptr);
  bool sawCurrentGame = false;
  bool chooseNextAfterHandledFinal = false;

  for (JsonObject game : games) {
    const GameInfo candidate = gameInfoFromJson(game);
    if (!candidate.hasGame) {
      continue;
    }

    if (!schedule.currentGame.hasGame) {
      schedule.currentGame = candidate;
      if (currentGamePk == -1) {
        sawCurrentGame = true;
        continue;
      }
    }

    if (currentGamePk == candidate.gamePk) {
      schedule.currentGame = candidate;
      sawCurrentGame = true;
      chooseNextAfterHandledFinal = candidate.final && finalHandledForCurrentGame;
      continue;
    }

    bool becameCurrentGame = false;
    if (chooseNextAfterHandledFinal && !candidate.final) {
      schedule.currentGame = candidate;
      chooseNextAfterHandledFinal = false;
      becameCurrentGame = true;
    }

    if (!becameCurrentGame && sawCurrentGame && !schedule.nextGame.hasGame && candidate.startEpoch > now && !candidate.final) {
      schedule.nextGame = candidate;
    }
  }

  if (!schedule.currentGame.hasGame) {
    logMessage("Schedule response did not include the Rays.");
  }

  return schedule;
}

GameInfo getPreviousFinalGame(int beforeGamePk) {
  GameInfo previous;
  const String url = recentScoreboardScheduleUrl();
  if (url.length() == 0) {
    return previous;
  }

  DynamicJsonDocument document(16384);
  if (!fetchJson(url, document)) {
    return previous;
  }

  JsonArray dates = document["dates"].as<JsonArray>();
  if (dates.isNull()) {
    return previous;
  }

  for (JsonObject dateEntry : dates) {
    JsonArray games = dateEntry["games"].as<JsonArray>();
    if (games.isNull()) {
      continue;
    }

    for (JsonObject game : games) {
      const GameInfo candidate = gameInfoFromJson(game);
      if (candidate.hasGame && candidate.final && candidate.gamePk != beforeGamePk &&
          (!previous.hasGame || candidate.startEpoch >= previous.startEpoch)) {
        previous = candidate;
      }
    }
  }

  return previous;
}

void handlePregameLightBeforeCutoff(const GameInfo& game) {
  if (!game.hasGame || game.startEpoch <= 0 || game.final) {
    return;
  }

  const time_t now = time(nullptr);
  const time_t pregameCutoff = game.startEpoch - PREGAME_LIGHT_OFF_LEAD_SECONDS;
  if (now >= pregameCutoff) {
    applyPregameLightOff(game.gamePk);
    return;
  }

  if (previousFinalForGamePk != game.gamePk) {
    previousFinalForGamePk = game.gamePk;
    previousFinalAppliedPk = -1;
    lastPreviousFinalCheckMs = 0;
  }

  // Re-check periodically: a prior game that crossed midnight may only flip to
  // Final after we first started tracking today's game, and it would otherwise
  // be missed (it drops out of "today's" schedule once the date rolls over).
  const unsigned long nowMs = millis();
  if (previousFinalAppliedPk != -1 &&
      lastPreviousFinalCheckMs != 0 &&
      nowMs - lastPreviousFinalCheckMs < PREVIOUS_FINAL_RECHECK_MS) {
    return;
  }
  lastPreviousFinalCheckMs = nowMs;

  const GameInfo previous = getPreviousFinalGame(game.gamePk);
  if (!previous.hasGame) {
    return;
  }

  // Only (re)apply when the most recent final game actually changes, so we
  // don't fight a manual toggle on every poll.
  if (previous.gamePk != previousFinalAppliedPk) {
    previousFinalAppliedPk = previous.gamePk;
    setAutomaticLight(previous.raysWon);
    logMessage(
      previous.raysWon
        ? "Most recent Rays game was a win; light on until pregame cutoff."
        : "Most recent Rays game was a loss; light off until today's result."
    );
  }
}

String liveLineScoreUrl(int gamePk) {
  String url = String(MLB_LIVE_FEED_BASE_URL);
  url += String(gamePk);
  url += "/feed/live?fields=gameData,datetime,dateTime,liveData,linescore,currentInningOrdinal,inningState,balls,strikes,outs,offense,first,second,third,innings,num,away,runs,home,teams,hits,errors";
  return url;
}

String recentScoreboardScheduleUrl() {
  const String startDate = easternDateWithDayOffset(-14);
  const String endDate = easternDateWithDayOffset(0);
  if (startDate.length() == 0 || endDate.length() == 0) {
    return "";
  }

  String url = String(MLB_SCHEDULE_URL);
  url += "?sportId=1&teamId=" + String(RAYS_TEAM_ID);
  url += "&startDate=" + startDate;
  url += "&endDate=" + endDate;
  url += "&fields=dates,games,gamePk,gameDate,status,detailedState,abstractGameState,teams,home,away,team,id,name,teamName,abbreviation,leagueRecord,wins,losses,isWinner,score";
  return url;
}

String upcomingScheduleUrl() {
  const String startDate = easternDateWithDayOffset(0);
  const String endDate = easternDateWithDayOffset(14);
  if (startDate.length() == 0 || endDate.length() == 0) {
    return "";
  }

  String url = String(MLB_SCHEDULE_URL);
  url += "?sportId=1&teamId=" + String(RAYS_TEAM_ID);
  url += "&startDate=" + startDate;
  url += "&endDate=" + endDate;
  url += "&fields=dates,games,gamePk,gameDate,status,detailedState,abstractGameState,teams,home,away,team,id,name,teamName,abbreviation";
  return url;
}

bool isRaysGame(JsonObject game) {
  if (game.isNull()) {
    return false;
  }

  const int homeTeamId = game["teams"]["home"]["team"]["id"] | -1;
  const int awayTeamId = game["teams"]["away"]["team"]["id"] | -1;
  return homeTeamId == RAYS_TEAM_ID || awayTeamId == RAYS_TEAM_ID;
}

bool isLiveRaysGame(JsonObject game) {
  if (!isRaysGame(game)) {
    return false;
  }

  const char* abstractState = game["status"]["abstractGameState"] | "";
  return strcmp(abstractState, "Live") == 0;
}

bool isActiveRaysGame(JsonObject game) {
  if (!isRaysGame(game)) {
    return false;
  }

  const char* abstractState = game["status"]["abstractGameState"] | "";
  if (strcmp(abstractState, "Final") == 0) {
    return false;
  }

  if (strcmp(abstractState, "Live") == 0) {
    return true;
  }

  const char* detailedState = game["status"]["detailedState"] | "";
  return strcmp(detailedState, "Warmup") == 0 ||
         strcmp(detailedState, "Pre-Game") == 0 ||
         strstr(detailedState, "In Progress") != nullptr ||
         strcmp(detailedState, "Delayed Start") == 0;
}

bool isFinalRaysGame(JsonObject game) {
  if (!isRaysGame(game)) {
    return false;
  }

  const char* abstractState = game["status"]["abstractGameState"] | "";
  const char* detailedState = game["status"]["detailedState"] | "";
  return strcmp(abstractState, "Final") == 0 || strstr(detailedState, "Final") != nullptr;
}

void addTeamJson(JsonObject target, JsonObject source) {
  JsonObject team = source["team"];
  JsonObject record = source["leagueRecord"];

  target["id"] = team["id"] | -1;
  target["name"] = team["name"] | "";
  target["teamName"] = team["teamName"] | "";
  target["abbreviation"] = team["abbreviation"] | "";
  target["wins"] = record["wins"] | 0;
  target["losses"] = record["losses"] | 0;
  target["score"] = source["score"] | 0;
  target["isWinner"] = source["isWinner"] | false;
}

bool addLastGameJson(JsonDocument& response) {
  if (WiFi.status() != WL_CONNECTED) {
    response["error"] = "Wi-Fi is disconnected.";
    return false;
  }

  if (!clockSynced) {
    clockSynced = syncClock();
    if (!clockSynced) {
      response["error"] = "Clock is not synchronized.";
      return false;
    }
  }

  int gamePk = -1;

  {
    const String scheduleUrl = recentScoreboardScheduleUrl();
    if (scheduleUrl.length() == 0) {
      response["error"] = "No local date available for schedule request.";
      return false;
    }

    DynamicJsonDocument scheduleDoc(24576);
    if (!fetchJson(scheduleUrl, scheduleDoc)) {
      response["error"] = "Could not fetch recent Rays games.";
      return false;
    }

    JsonObject liveGame;
    JsonObject activeGame;
    JsonObject finalGame;
    JsonArray dates = scheduleDoc["dates"].as<JsonArray>();
    for (JsonObject dateEntry : dates) {
      JsonArray games = dateEntry["games"].as<JsonArray>();
      for (JsonObject game : games) {
        if (isLiveRaysGame(game)) {
          liveGame = game;
        }

        if (isActiveRaysGame(game)) {
          activeGame = game;
        }

        if (isFinalRaysGame(game)) {
          finalGame = game;
        }
      }
    }

    JsonObject selectedGame = liveGame;
    if (selectedGame.isNull()) {
      selectedGame = activeGame;
    }
    if (selectedGame.isNull()) {
      selectedGame = finalGame;
    }
    if (selectedGame.isNull()) {
      response["error"] = "No live or final Rays game found in the schedule window.";
      return false;
    }

    gamePk = selectedGame["gamePk"] | -1;
    response["gamePk"] = gamePk;
    response["gameDate"] = selectedGame["gameDate"] | "";
    response["status"] = selectedGame["status"]["detailedState"] | "Final";
    response["live"] = isLiveRaysGame(selectedGame) ||
                       (strstr(selectedGame["status"]["detailedState"] | "", "In Progress") != nullptr);

    // If we somehow still ended up with a future scheduled game (no recent final
    // in the window), prefer any finalGame we did find so we show a completed
    // box score instead of a 0-0 placeholder.
    if (!finalGame.isNull() && selectedGame == activeGame) {
      const char* selDetailed = selectedGame["status"]["detailedState"] | "";
      if (strcmp(selDetailed, "Scheduled") == 0) {
        selectedGame = finalGame;
        gamePk = selectedGame["gamePk"] | -1;
        response["gamePk"] = gamePk;
        response["gameDate"] = selectedGame["gameDate"] | "";
        response["status"] = selectedGame["status"]["detailedState"] | "Final";
        response["live"] = false;
        response["raysHome"] = (selectedGame["teams"]["home"]["team"]["id"] | -1) == RAYS_TEAM_ID;

        JsonObject teams2 = response.createNestedObject("teams");
        addTeamJson(teams2.createNestedObject("away"), selectedGame["teams"]["away"].as<JsonObject>());
        addTeamJson(teams2.createNestedObject("home"), selectedGame["teams"]["home"].as<JsonObject>());
      }
    }
    response["raysHome"] = (selectedGame["teams"]["home"]["team"]["id"] | -1) == RAYS_TEAM_ID;

    JsonObject teams = response.createNestedObject("teams");
    addTeamJson(teams.createNestedObject("away"), selectedGame["teams"]["away"].as<JsonObject>());
    addTeamJson(teams.createNestedObject("home"), selectedGame["teams"]["home"].as<JsonObject>());
  }

  handleWebServer();
  feedWatchdog();

  {
    const String upcomingUrl = upcomingScheduleUrl();
    if (upcomingUrl.length() > 0) {
      DynamicJsonDocument upcomingDoc(12288);
      if (fetchJson(upcomingUrl, upcomingDoc)) {
        const time_t now = time(nullptr);
        time_t nearestStart = 0;
        JsonObject nearestGame;
        JsonArray dates = upcomingDoc["dates"].as<JsonArray>();
        for (JsonObject dateEntry : dates) {
          JsonArray upcomingGames = dateEntry["games"].as<JsonArray>();
          if (upcomingGames.isNull()) {
            continue;
          }

          for (JsonObject game : upcomingGames) {
            if (!isRaysGame(game)) {
              continue;
            }

            const int pk = game["gamePk"] | -1;
            if (pk == gamePk) {
              continue;
            }

            const char* abstractState = game["status"]["abstractGameState"] | "";
            if (strcmp(abstractState, "Final") == 0) {
              continue;
            }

            const char* upcomingGameDate = game["gameDate"] | "";
            const time_t startEpoch = parseMlbUtcEpoch(upcomingGameDate);
            if (startEpoch == 0) {
              continue;
            }

            if (startEpoch <= now && strcmp(abstractState, "Live") != 0) {
              continue;
            }

            if (nearestStart == 0 || startEpoch < nearestStart) {
              nearestStart = startEpoch;
              nearestGame = game;
            }
          }
        }

        if (!nearestGame.isNull()) {
          const bool raysHome = (nearestGame["teams"]["home"]["team"]["id"] | -1) == RAYS_TEAM_ID;
          const char* opponentSide = raysHome ? "away" : "home";
          JsonObject opponentTeam = nearestGame["teams"][opponentSide];

          JsonObject nextOut = response.createNestedObject("nextGame");
          nextOut["gamePk"] = nearestGame["gamePk"] | -1;
          nextOut["gameDate"] = nearestGame["gameDate"] | "";
          nextOut["startTime"] = formatLocalTime(nearestStart);
          nextOut["raysHome"] = raysHome;
          nextOut["status"] = nearestGame["status"]["detailedState"] | "";

          JsonObject opponent = nextOut.createNestedObject("opponent");
          JsonObject team = opponentTeam["team"];
          opponent["id"] = team["id"] | -1;
          opponent["name"] = team["name"] | "";
          opponent["abbreviation"] = team["abbreviation"] | "";
        }
      }
    }
  }

  handleWebServer();
  feedWatchdog();

  DynamicJsonDocument lineDoc(20480);
  if (!fetchJson(liveLineScoreUrl(gamePk), lineDoc)) {
    response["lineScoreError"] = "Could not fetch line score.";
    return true;
  }

  JsonObject sourceLinescore = lineDoc["liveData"]["linescore"];
  if (sourceLinescore.isNull()) {
    response["lineScoreError"] = "Line score unavailable.";
    return true;
  }

  // Prefer detailed linescore data for final scores (fixes cases where the
  // schedule summary does not include scores for previous-night final games).
  {
    JsonObject responseTeams = response["teams"];
    if (!responseTeams.isNull()) {
      const int lsAwayRuns = sourceLinescore["teams"]["away"]["runs"] | (responseTeams["away"]["score"] | 0);
      const int lsHomeRuns = sourceLinescore["teams"]["home"]["runs"] | (responseTeams["home"]["score"] | 0);
      responseTeams["away"]["score"] = lsAwayRuns;
      responseTeams["home"]["score"] = lsHomeRuns;
    }
  }

  JsonObject lineScore = response.createNestedObject("lineScore");
  JsonArray inningsOut = lineScore.createNestedArray("innings");
  JsonArray innings = sourceLinescore["innings"].as<JsonArray>();
  const bool isLive = response["live"] | false;

  if (isLive) {
    const int currentInning = parseInningOrdinal(sourceLinescore["currentInningOrdinal"]);
    const char* inningState = sourceLinescore["inningState"] | "";
    const bool inTopHalf = strcmp(inningState, "Top") == 0;
    const int columnCount = currentInning > 9 ? currentInning : 9;

    for (int inningNum = 1; inningNum <= columnCount; inningNum++) {
      JsonObject apiInning;
      if (!innings.isNull()) {
        for (JsonObject inning : innings) {
          if ((inning["num"] | 0) == inningNum) {
            apiInning = inning;
            break;
          }
        }
      }

      const bool pastInning = inningNum < currentInning;
      const bool currentInningActive = inningNum == currentInning;
      const bool awayComplete = pastInning || (currentInningActive && !inTopHalf);
      const bool homeComplete =
        pastInning ||
        (currentInningActive && !apiInning.isNull() && !apiInning["home"]["runs"].isNull());

      JsonObject inningOut = inningsOut.createNestedObject();
      inningOut["num"] = inningNum;

      if (awayComplete && !apiInning.isNull()) {
        inningOut["away"] = apiInning["away"]["runs"] | 0;
      } else {
        inningOut["away"] = nullptr;
      }

      if (homeComplete && !apiInning.isNull()) {
        inningOut["home"] = apiInning["home"]["runs"] | 0;
      } else {
        inningOut["home"] = nullptr;
      }
    }
  } else {
    for (JsonObject inning : innings) {
      JsonObject inningOut = inningsOut.createNestedObject();
      inningOut["num"] = inning["num"] | 0;
      inningOut["away"] = inning["away"]["runs"] | 0;
      if (inning["home"]["runs"].isNull()) {
        inningOut["home"] = nullptr;
      } else {
        inningOut["home"] = inning["home"]["runs"] | 0;
      }
    }
  }

  JsonObject totals = lineScore.createNestedObject("totals");

  // Use the linescore summary for authoritative final runs/hits/errors.
  // (The main team scores were already synced above from this same source.)
  JsonObject awayLs = sourceLinescore["teams"]["away"];
  JsonObject homeLs = sourceLinescore["teams"]["home"];

  JsonObject awayTotals = totals.createNestedObject("away");
  awayTotals["runs"] = awayLs["runs"] | 0;
  awayTotals["hits"] = awayLs["hits"] | 0;
  awayTotals["errors"] = awayLs["errors"] | 0;

  JsonObject homeTotals = totals.createNestedObject("home");
  homeTotals["runs"] = homeLs["runs"] | 0;
  homeTotals["hits"] = homeLs["hits"] | 0;
  homeTotals["errors"] = homeLs["errors"] | 0;

  if (response["live"] | false) {
    JsonObject inningState = response.createNestedObject("inningState");
    const String inning = String(sourceLinescore["inningState"] | "") + " " + String(sourceLinescore["currentInningOrdinal"] | "");
    inningState["inning"] = inning;
    inningState["balls"] = sourceLinescore["balls"] | 0;
    inningState["strikes"] = sourceLinescore["strikes"] | 0;
    inningState["outs"] = sourceLinescore["outs"] | 0;

    JsonObject runners = inningState.createNestedObject("runners");
    JsonObject offense = sourceLinescore["offense"];
    runners["first"] = !offense["first"].isNull();
    runners["second"] = !offense["second"].isNull();
    runners["third"] = !offense["third"].isNull();
  }

  return true;
}

void resetForGameIfNeeded(const GameInfo& game) {
  if (!game.hasGame || game.gamePk == currentGamePk) {
    return;
  }

  currentGamePk = game.gamePk;
  finalHandledForCurrentGame = false;
  lastRaysRunsSeen = -1;
  pregameLightOffApplied = false;
  previousFinalForGamePk = -1;
  previousFinalAppliedPk = -1;
  lastPreviousFinalCheckMs = 0;

  // If the device starts mid-game, ignore runs already on the board so it only
  // flashes for scoring discovered after this sketch begins watching the game.
  if (game.live && game.raysScore >= 0) {
    lastRaysRunsSeen = game.raysScore;
  }

  logMessage("Watching Rays game " + String(currentGamePk) + " (" + game.detailedState + ").");
}

void handleRaysScoringDetection(const GameInfo& game) {
  if (!game.live || game.raysScore < 0) {
    return;
  }

  const int currentRuns = game.raysScore;

  if (lastRaysRunsSeen < 0) {
    lastRaysRunsSeen = currentRuns;
    return;
  }

  if (currentRuns > lastRaysRunsSeen) {
    const int runsScored = currentRuns - lastRaysRunsSeen;
    logMessage(
      runsScored == 1
        ? "Rays run scored. Flashing light for 15 seconds."
        : "Rays scored " + String(runsScored) + " runs. Flashing light for 15 seconds."
    );
    flashLight(SCORE_FLASH_SECONDS);
  }

  lastRaysRunsSeen = currentRuns;
}

void handleFinalResult(const GameInfo& game, const GameInfo& nextGame) {
  if (!game.final || finalHandledForCurrentGame) {
    return;
  }

  finalHandledForCurrentGame = true;

  if (game.raysWon) {
    logMessage("Rays win detected. Flashing light for 20 seconds.");
    flashLight(WIN_FLASH_SECONDS);
  } else {
    cancelManualLightOverride();
    logMessage("Rays final detected; no win celebration.");
    setAutomaticLight(false);
  }

  const time_t now = time(nullptr);
  const time_t cutoff = nextGame.hasGame ? nextGame.startEpoch - PREGAME_LIGHT_OFF_LEAD_SECONDS : 0;
  if (nextGame.hasGame && cutoff > now) {
    if (game.raysWon) {
      setAutomaticLight(true);
      setBackoffUntil(cutoff, "Doubleheader detected after Rays win; keeping light on until pregame cutoff.");
    } else {
      setAutomaticLight(false);
      setBackoffUntil(cutoff, "Doubleheader detected after Rays loss; keeping light off.");
    }

    logMessage("Next Rays game starts at " + formatLocalTime(nextGame.startEpoch) + ".");
  } else if (nextGame.hasGame) {
    backoffUntilEpoch = 0;
    currentPollIntervalMs = CHECK_INTERVAL_MS;
    setAutomaticLight(game.raysWon);
    logMessage("Doubleheader detected, but the pregame cutoff has already passed. Resuming live polling.");
    forceNextPollSoon();
  } else {
    setAutomaticLight(game.raysWon);
    setBackoffUntil(
      tomorrowNoonEpoch(),
      game.raysWon ? "No doubleheader found after Rays win; keeping light on until pregame cutoff." : "No doubleheader found after Rays final."
    );
  }
}

void pollMlbApi() {
  if (otaUpdateInProgress) {
    return;
  }

  feedWatchdog();
  maintainTimedStates();

  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!clockSynced) {
    clockSynced = syncClock();
    if (!clockSynced) {
      return;
    }
  }

  const ScheduleInfo schedule = getTodaySchedule();
  if (schedule.fetchOk && schedule.currentGame.hasGame) {
    latestGame = schedule.currentGame;
    latestNextGame = schedule.nextGame;
  }
  const GameInfo game = schedule.currentGame;
  if (!game.hasGame) {
    return;
  }

  resetForGameIfNeeded(game);
  logMessage("Rays game status: " + game.detailedState);

  handlePregameLightBeforeCutoff(game);
  handleRaysScoringDetection(game);
  handleFinalResult(game, schedule.nextGame);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // ESP-IDF v5 (Arduino core 3.x) watchdog API. The core usually starts the
  // WDT already, so reconfigure it; fall back to init if it wasn't running.
  const esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&wdtConfig) != ESP_OK) {
    esp_task_wdt_init(&wdtConfig);
  }
  esp_task_wdt_add(nullptr);

  pinMode(STARTUP_LED_PIN, OUTPUT);
  setStartupLed(false);
  blinkStartupLed();

  pinMode(LIGHT_CONTROL_PIN, OUTPUT);
  setLight(false);
  flashLight(3);

  loadWifiCredentials();
  if (wifiSsid.length() == 0) {
    logMessage("No stored Wi-Fi credentials. Starting setup AP.");
    startApSetupMode();
    return;
  }

  for (int attempt = 0; attempt < 3 && WiFi.status() != WL_CONNECTED; attempt++) {
    lastWifiAttemptMs = 0;
    connectWiFi();
  }

  if (WiFi.status() != WL_CONNECTED) {
    logMessage("Could not join \"" + wifiSsid + "\". Starting setup AP.");
    startApSetupMode();
    return;
  }

  clockSynced = syncClock();
  lastPollMs = 0;
}

void loop() {
  feedWatchdog();

  if (apSetupMode) {
    handleWebServer();
    maintainSetupLight();
    return;
  }

  handleOta();
  if (otaUpdateInProgress) {
    return;
  }

  handleWebServer();
  maintainTimedStates();

  if (millis() - lastPollMs >= currentPollIntervalMs) {
    lastPollMs = millis();
    pollMlbApi();
  }
}
