/*
  PIR Notifier for LoLin NodeMCU v3 (ESP8266)
  --------------------------------------------
  - On boot, tries to connect to a saved WiFi network.
  - If it can't connect (no saved network, or connection fails), it opens
    an OPEN (no password) access point so you can configure it.
  - Serves a single web page that lets you:
      * Scan for WiFi networks and pick one, enter a password, save it
        (device then reboots and tries to connect).
      * Choose which pin the PIR sensor is wired to (default D0).
      * Set a URL that gets called whenever the PIR triggers, which HTTP
        method to use (GET/POST/PUT/PATCH), and the cooldown (in ms)
        between triggers.
  - When the PIR triggers: calls the configured URL with the configured
    method and flashes the onboard LED (D4 / GPIO2).

  Libraries required (install via Arduino Library Manager):
      - ArduinoJson (by Benoit Blanchon), version 6.x

  Board core required:
      - ESP8266 board package (see README.md)

  See README.md for full flashing + setup instructions.
*/

#include <memory>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

// ---------- fixed hardware ----------
#define LED_PIN 2        // D4 onboard LED, active LOW
#define BUTTON_PIN 0     // D3 - onboard FLASH button, used to reset config on boot
#define CONFIG_FILE "/config.json"
#define AP_SSID "PIR-Setup"          // open network name shown during setup
#define DEFAULT_COOLDOWN_MS 5000     // default minimum gap between two triggers

// ---------- pin name <-> GPIO map (NodeMCU silkscreen labels) ----------
struct PinMap { const char *name; uint8_t gpio; };
PinMap pinTable[] = {
  {"D0", 16}, {"D1", 5}, {"D2", 4}, {"D3", 0},
  {"D4", 2},  {"D5", 14}, {"D6", 12}, {"D7", 13}, {"D8", 15}
};
const int pinTableSize = sizeof(pinTable) / sizeof(pinTable[0]);

ESP8266WebServer server(80);

// ---------- config, loaded from / saved to LittleFS ----------
String cfgSsid = "";
String cfgPass = "";
String cfgPirPin = "D0";
String cfgTriggerUrl = "";
String cfgTriggerMethod = "GET";           // GET, POST, PUT, or PATCH
unsigned long cfgCooldownMs = DEFAULT_COOLDOWN_MS;

const char *ALLOWED_METHODS[] = {"GET", "POST", "PUT", "PATCH"};
const int ALLOWED_METHODS_SIZE = sizeof(ALLOWED_METHODS) / sizeof(ALLOWED_METHODS[0]);

bool isAllowedMethod(const String &m) {
  for (int i = 0; i < ALLOWED_METHODS_SIZE; i++) {
    if (m == ALLOWED_METHODS[i]) return true;
  }
  return false;
}

bool staConnected = false;
uint8_t pirGpio = 16;
unsigned long lastTrigger = 0;
int lastPirState = LOW;

// ---------------- config load/save ----------------
void loadConfig() {
  cfgSsid = ""; cfgPass = ""; cfgPirPin = "D0"; cfgTriggerUrl = "";
  cfgTriggerMethod = "GET"; cfgCooldownMs = DEFAULT_COOLDOWN_MS;
  if (!LittleFS.exists(CONFIG_FILE)) return;
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return;
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  cfgSsid          = doc["ssid"]           | "";
  cfgPass          = doc["pass"]           | "";
  cfgPirPin        = doc["pir_pin"]        | "D0";
  cfgTriggerUrl    = doc["trigger_url"]    | "";
  cfgTriggerMethod = doc["trigger_method"] | "GET";
  cfgCooldownMs    = doc["cooldown_ms"]    | DEFAULT_COOLDOWN_MS;
  if (!isAllowedMethod(cfgTriggerMethod)) cfgTriggerMethod = "GET";
}

void saveConfig() {
  StaticJsonDocument<512> doc;
  doc["ssid"] = cfgSsid;
  doc["pass"] = cfgPass;
  doc["pir_pin"] = cfgPirPin;
  doc["trigger_url"] = cfgTriggerUrl;
  doc["trigger_method"] = cfgTriggerMethod;
  doc["cooldown_ms"] = cfgCooldownMs;
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

uint8_t gpioForName(const String &name) {
  for (int i = 0; i < pinTableSize; i++) {
    if (name == pinTable[i].name) return pinTable[i].gpio;
  }
  return 16; // fall back to D0
}

// ---------------- web page ----------------
String htmlPage() {
  String html = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PIR Node Setup</title><style>"
    "body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 10px;}"
    "h2{color:#333} label{display:block;margin-top:12px;font-weight:bold}"
    "select,input{width:100%;padding:8px;margin-top:4px;box-sizing:border-box}"
    "button{margin-top:16px;padding:10px 16px;background:#2a7;color:#fff;border:0;border-radius:4px;cursor:pointer}"
    "#networks div{padding:6px;border-bottom:1px solid #eee;cursor:pointer}"
    "#networks div:hover{background:#f0f0f0}"
    ".status{padding:8px;background:#eef;border-radius:4px;margin-bottom:10px}"
    "</style></head><body>");

  html += F("<h2>PIR Node Setup</h2>");
  html += "<div class='status'>Status: ";
  if (staConnected) {
    html += "Connected to WiFi (" + WiFi.SSID() + "), IP: " + WiFi.localIP().toString();
  } else {
    html += "Access Point mode &mdash; not connected to a WiFi network";
  }
  html += "</div>";

  html += F(
    "<h3>WiFi</h3>"
    "<button onclick='scanNetworks()'>Scan networks</button>"
    "<div id='networks'></div>"
    "<form id='wifiForm' action='/savewifi' method='POST'>"
    "<label>SSID</label><input type='text' id='ssid' name='ssid' value='");
  html += cfgSsid;
  html += F(
    "' required>"
    "<label>Password</label><input type='password' name='password' placeholder='leave blank for an open network'>"
    "<button type='submit'>Save WiFi &amp; Reboot</button>"
    "</form>");

  html += F(
    "<h3>PIR / Trigger settings</h3>"
    "<form action='/saveconfig' method='POST'>"
    "<label>PIR connected to</label><select name='pir_pin'>");
  for (int i = 0; i < pinTableSize; i++) {
    html += "<option value='" + String(pinTable[i].name) + "'";
    if (cfgPirPin == pinTable[i].name) html += " selected";
    html += ">" + String(pinTable[i].name) + " (GPIO" + String(pinTable[i].gpio) + ")</option>";
  }
  html += F(
    "</select>"
    "<label>Trigger URL (called on motion, e.g. http://host/trigger)</label>"
    "<input type='text' name='trigger_url' value='");
  html += cfgTriggerUrl;
  html += F(
    "' placeholder='http://...'>"
    "<label>HTTP method</label><select name='trigger_method'>");
  for (int i = 0; i < ALLOWED_METHODS_SIZE; i++) {
    html += "<option value='" + String(ALLOWED_METHODS[i]) + "'";
    if (cfgTriggerMethod == ALLOWED_METHODS[i]) html += " selected";
    html += ">" + String(ALLOWED_METHODS[i]) + "</option>";
  }
  html += F(
    "</select>"
    "<label>Cooldown between triggers (ms)</label>"
    "<input type='number' name='cooldown_ms' min='0' step='100' value='");
  html += String(cfgCooldownMs);
  html += F(
    "'>"
    "<button type='submit'>Save Settings</button>"
    "</form>");

  html += F(
    "<script>"
    "function scanNetworks(){"
    "document.getElementById('networks').innerHTML='Scanning...';"
    "fetch('/scan').then(r=>r.json()).then(list=>{"
    "let el=document.getElementById('networks');el.innerHTML='';"
    "list.forEach(n=>{"
    "let d=document.createElement('div');"
    "d.textContent=n.ssid+' ('+n.rssi+' dBm)'+(n.secure?' \\uD83D\\uDD12':'');"
    "d.onclick=()=>{document.getElementById('ssid').value=n.ssid;};"
    "el.appendChild(d);"
    "});"
    "});"
    "}"
    "</script>");

  html += F("</body></html>");
  return html;
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleScan() {
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"secure\":" + String(WiFi.encryptionType(i) != ENC_TYPE_NONE ? "true" : "false") + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
  WiFi.scanDelete();
}

void handleSaveWifi() {
  if (server.hasArg("ssid")) cfgSsid = server.arg("ssid");
  if (server.hasArg("password")) cfgPass = server.arg("password");
  saveConfig();
  server.send(200, "text/html", "<html><body><h3>Saved. Rebooting and connecting...</h3></body></html>");
  delay(1000);
  ESP.restart();
}

void handleSaveConfig() {
  if (server.hasArg("pir_pin")) cfgPirPin = server.arg("pir_pin");
  if (server.hasArg("trigger_url")) cfgTriggerUrl = server.arg("trigger_url");
  if (server.hasArg("trigger_method")) {
    String m = server.arg("trigger_method");
    m.toUpperCase();
    if (isAllowedMethod(m)) cfgTriggerMethod = m;
  }
  if (server.hasArg("cooldown_ms")) {
    long v = server.arg("cooldown_ms").toInt();
    if (v < 0) v = 0;
    cfgCooldownMs = (unsigned long)v;
  }
  saveConfig();
  pirGpio = gpioForName(cfgPirPin);
  pinMode(pirGpio, INPUT);
  server.sendHeader("Location", "/");
  server.send(303);
}

// ---------------- PIR handling ----------------
void flashLed() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(LED_PIN, LOW);  delay(100);
    digitalWrite(LED_PIN, HIGH); delay(100);
  }
}

void callTriggerUrl() {
  if (cfgTriggerUrl.length() == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  WiFiClient client;
  std::unique_ptr<BearSSL::WiFiClientSecure> clientSecure;
  bool ok = false;

  if (cfgTriggerUrl.startsWith("https://")) {
    clientSecure.reset(new BearSSL::WiFiClientSecure());
    clientSecure->setInsecure(); // no certificate validation - keep it simple
    ok = http.begin(*clientSecure, cfgTriggerUrl);
  } else {
    ok = http.begin(client, cfgTriggerUrl);
  }

  if (ok) {
    http.sendRequest(cfgTriggerMethod.c_str());
    http.end();
  }
}

void checkPir() {
  if (WiFi.status() != WL_CONNECTED) return; // need WiFi to call the URL
  int state = digitalRead(pirGpio);
  unsigned long now = millis();
  if (state == HIGH && lastPirState == LOW && (now - lastTrigger > cfgCooldownMs)) {
    lastTrigger = now;
    callTriggerUrl();
    flashLed();
  }
  lastPirState = state;
}

// ---------------- WiFi modes ----------------
void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID); // open network, no password
  staConnected = false;
}

bool tryConnectSTA() {
  if (cfgSsid.length() == 0) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  return WiFi.status() == WL_CONNECTED;
}

// ---------------- setup / loop ----------------
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH); // off (active low)
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  LittleFS.begin();

  // Hold the onboard FLASH button while powering on to wipe saved config
  delay(50);
  if (digitalRead(BUTTON_PIN) == LOW) {
    LittleFS.remove(CONFIG_FILE);
  }

  loadConfig();
  pirGpio = gpioForName(cfgPirPin);
  pinMode(pirGpio, INPUT);

  staConnected = tryConnectSTA();
  if (!staConnected) {
    startAPMode();
  }

  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.on("/saveconfig", HTTP_POST, handleSaveConfig);
  server.begin();
}

void loop() {
  server.handleClient();
  checkPir();
}
