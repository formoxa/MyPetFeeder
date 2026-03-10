#include <WiFi.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <time.h>
#include <ArduinoJson.h>

// ===== Pin Configuration =====
#define LED_PIN       2
#define FEEDER_PIN    14

// ===== WiFi Configuration =====
#define AP_SSID       "My_Pet_Feeder_Setup"
#define AP_PASSWORD   "12345678"
#define WIFI_TIMEOUT  10000

// ===== File Paths =====
#define SSID_PATH     "/wifi_ssid.txt"
#define PASS_PATH     "/wifi_pass.txt"
#define FEED_DUR_PATH "/feed_duration.txt"
#define SCHEDULE_PATH "/schedule.json"

// ===== MQTT Configuration =====
#define MQTT_BROKER   "mqttgo.io"
#define MQTT_PORT     1883

// Topics - Subscribe (receive from broker)
#define TOPIC_FEED_NOW      "MyPetFeeder/feedNow"
#define TOPIC_SET_DURATION  "MyPetFeeder/setDuration"
#define TOPIC_SET_SCHEDULE  "MyPetFeeder/setSchedule"
#define TOPIC_DEL_SCHEDULE  "MyPetFeeder/delSchedule"

// Topics - Publish (send to broker)
#define TOPIC_STATUS        "MyPetFeeder/status"
#define TOPIC_DURATION      "MyPetFeeder/duration"
#define TOPIC_SCHEDULES     "MyPetFeeder/schedules"

// ===== NTP Configuration =====
#define NTP_SERVER    "time.stdtime.gov.tw"
#define TZ_OFFSET     8 * 3600  // UTC+8

// ===== Schedule =====
#define MAX_SCHEDULES 10

struct FeedSchedule {
  uint8_t dayOfWeek;  // 0=Sun, 1=Mon, ..., 6=Sat
  uint8_t hour;
  uint8_t minute;
  bool active;
};

FeedSchedule schedules[MAX_SCHEDULES];
int scheduleCount = 0;

// ===== Global Objects =====
WebServer server(80);
WiFiClient espClient;
PubSubClient mqtt(espClient);

String savedSSID = "2D";
String savedPass = "0980974995";

int feedDuration = 5;          // Default feed duration in seconds
bool isFeeding = false;
unsigned long feedStartTime = 0;
unsigned long lastStatusSend = 0;
unsigned long lastScheduleSend = 0;
unsigned long lastScheduleCheck = 0;

// ===== Day name helper =====
const char* dayName(uint8_t d) {
  const char* names[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  return (d < 7) ? names[d] : "???";
}

// ===== LittleFS Helpers =====
String readFile(const char *path) {
  if (!LittleFS.exists(path)) return "";
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  String content = f.readString();
  f.close();
  content.trim();
  return content;
}

bool writeFile(const char *path, const String &data) {
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.printf("[FS] Failed to write: %s\n", path);
    return false;
  }
  f.print(data);
  f.close();
  return true;
}

// ===== WiFi Credentials =====
bool loadCredentials() {
  savedSSID = readFile(SSID_PATH);
  savedPass = readFile(PASS_PATH);
  if (savedSSID.isEmpty()) {
    Serial.println("[FS] No saved SSID found");
    return false;
  }
  Serial.printf("[FS] Loaded SSID: %s\n", savedSSID.c_str());
  return true;
}

bool saveCredentials(const String &ssid, const String &pass) {
  if (!writeFile(SSID_PATH, ssid) || !writeFile(PASS_PATH, pass)) {
    Serial.println("[FS] Failed to save credentials");
    return false;
  }
  Serial.printf("[FS] Saved SSID: %s\n", ssid.c_str());
  return true;
}

// ===== Feed Duration =====
void loadFeedDuration() {
  String val = readFile(FEED_DUR_PATH);
  if (!val.isEmpty()) {
    feedDuration = val.toInt();
    if (feedDuration < 1) feedDuration = 1;
    if (feedDuration > 60) feedDuration = 60;
  }
  Serial.printf("[FS] Feed duration: %d sec\n", feedDuration);
}

void saveFeedDuration() {
  writeFile(FEED_DUR_PATH, String(feedDuration));
}

// ===== Schedule Persistence =====
void loadSchedules() {
  String json = readFile(SCHEDULE_PATH);
  if (json.isEmpty()) {
    scheduleCount = 0;
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    Serial.println("[FS] Failed to parse schedule JSON");
    scheduleCount = 0;
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  scheduleCount = 0;
  for (JsonObject obj : arr) {
    if (scheduleCount >= MAX_SCHEDULES) break;
    schedules[scheduleCount].dayOfWeek = obj["day"];
    schedules[scheduleCount].hour = obj["hour"];
    schedules[scheduleCount].minute = obj["min"];
    schedules[scheduleCount].active = true;
    scheduleCount++;
  }
  Serial.printf("[FS] Loaded %d schedules\n", scheduleCount);
}

void saveSchedules() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].active) continue;
    JsonObject obj = arr.add<JsonObject>();
    obj["day"] = schedules[i].dayOfWeek;
    obj["hour"] = schedules[i].hour;
    obj["min"] = schedules[i].minute;
  }
  String json;
  serializeJson(doc, json);
  writeFile(SCHEDULE_PATH, json);
}

// ===== Feeder Control =====
void startFeeding() {
  if (isFeeding) return;
  isFeeding = true;
  feedStartTime = millis();
  digitalWrite(FEEDER_PIN, LOW);
  Serial.printf("[Feed] Started, duration: %d sec\n", feedDuration);
}

void stopFeeding() {
  isFeeding = false;
  digitalWrite(FEEDER_PIN, HIGH);
  Serial.println("[Feed] Stopped");
}

void checkFeedingTimeout() {
  if (isFeeding && millis() - feedStartTime >= (unsigned long)feedDuration * 1000) {
    stopFeeding();
  }
}

// ===== Time Helper =====
String getCurrentTime() {
  struct tm t;
  if (!getLocalTime(&t)) return "No time";
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

// ===== MQTT =====
String buildSchedulesJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].active) continue;
    JsonObject obj = arr.add<JsonObject>();
    obj["index"] = i;
    obj["day"] = schedules[i].dayOfWeek;
    obj["dayName"] = dayName(schedules[i].dayOfWeek);
    obj["hour"] = schedules[i].hour;
    obj["min"] = schedules[i].minute;
  }
  String json;
  serializeJson(doc, json);
  return json;
}

void publishStatus() {
  JsonDocument doc;
  doc["online"] = true;
  doc["time"] = getCurrentTime();
  doc["feeding"] = isFeeding;
  doc["ip"] = WiFi.localIP().toString();
  String json;
  serializeJson(doc, json);
  mqtt.publish(TOPIC_STATUS, json.c_str());
}

void publishDuration() {
  mqtt.publish(TOPIC_DURATION, String(feedDuration).c_str(), true);
}

void publishSchedules() {
  String json = buildSchedulesJson();
  mqtt.publish(TOPIC_SCHEDULES, json.c_str(), true);
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] %s: %s\n", topic, msg.c_str());

  String t = String(topic);

  // 1. Feed now button
  if (t == TOPIC_FEED_NOW) {
    if (msg == "1" || msg == "true") {
      Serial.println("[MQTT] Feed now triggered");
      startFeeding();
    }
  }

  // 2. Set feed duration (seconds)
  else if (t == TOPIC_SET_DURATION) {
    int d = msg.toInt();
    if (d >= 1 && d <= 60) {
      feedDuration = d;
      saveFeedDuration();
      publishDuration();
      Serial.printf("[MQTT] Feed duration set to %d sec\n", feedDuration);
    }
  }

  // 3. Add schedule: {"day":1,"hour":8,"min":30}
  else if (t == TOPIC_SET_SCHEDULE) {
    JsonDocument doc;
    if (deserializeJson(doc, msg)) return;
    if (scheduleCount >= MAX_SCHEDULES) {
      Serial.println("[MQTT] Schedule full");
      return;
    }
    uint8_t day = doc["day"];
    uint8_t hour = doc["hour"];
    uint8_t minute = doc["min"];
    if (day > 6 || hour > 23 || minute > 59) return;

    schedules[scheduleCount].dayOfWeek = day;
    schedules[scheduleCount].hour = hour;
    schedules[scheduleCount].minute = minute;
    schedules[scheduleCount].active = true;
    scheduleCount++;
    saveSchedules();
    publishSchedules();
    Serial.printf("[MQTT] Schedule added: %s %02d:%02d\n", dayName(day), hour, minute);
  }

  // 4. Delete schedule by index: "0", "1", or "all"
  else if (t == TOPIC_DEL_SCHEDULE) {
    if (msg == "all") {
      scheduleCount = 0;
      Serial.println("[MQTT] All schedules cleared");
    } else {
      int idx = msg.toInt();
      if (idx >= 0 && idx < scheduleCount) {
        for (int i = idx; i < scheduleCount - 1; i++) {
          schedules[i] = schedules[i + 1];
        }
        scheduleCount--;
        Serial.printf("[MQTT] Schedule %d deleted\n", idx);
      }
    }
    saveSchedules();
    publishSchedules();
  }
}

void connectMQTT() {
  if (mqtt.connected()) return;

  String clientId = "PetFeeder_" + String(random(0xFFFF), HEX);
  Serial.printf("[MQTT] Connecting to %s...\n", MQTT_BROKER);

  if (mqtt.connect(clientId.c_str())) {
    Serial.println("[MQTT] Connected");
    mqtt.subscribe(TOPIC_FEED_NOW);
    mqtt.subscribe(TOPIC_SET_DURATION);
    mqtt.subscribe(TOPIC_SET_SCHEDULE);
    mqtt.subscribe(TOPIC_DEL_SCHEDULE);
    publishDuration();
    publishSchedules();
    publishStatus();
  } else {
    Serial.printf("[MQTT] Failed, rc=%d\n", mqtt.state());
  }
}

// ===== Schedule Check =====
void checkSchedules() {
  struct tm t;
  if (!getLocalTime(&t)) return;

  for (int i = 0; i < scheduleCount; i++) {
    if (!schedules[i].active) continue;
    if (schedules[i].dayOfWeek == t.tm_wday &&
        schedules[i].hour == t.tm_hour &&
        schedules[i].minute == t.tm_min) {
      Serial.printf("[Schedule] Triggered: %s %02d:%02d\n",
                    dayName(schedules[i].dayOfWeek),
                    schedules[i].hour, schedules[i].minute);
      startFeeding();
    }
  }
}

// ===== WiFi Connection =====
bool connectWiFi(const String &ssid, const String &pass) {
  Serial.printf("[WiFi] Attempting to connect: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("[WiFi] Connection failed");
  WiFi.disconnect();
  return false;
}

// ===== AP Mode Web Page =====
const char htmlPage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Pet Feeder WiFi Setup</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, sans-serif;
      background: #0f172a; color: #e2e8f0;
      display: flex; justify-content: center; align-items: center;
      min-height: 100vh; padding: 1rem;
    }
    .card {
      background: #1e293b; border-radius: 12px;
      padding: 2rem; width: 100%; max-width: 380px;
      box-shadow: 0 4px 24px rgba(0,0,0,0.4);
    }
    h2 { text-align: center; margin-bottom: 1.5rem; color: #38bdf8; }
    label { display: block; margin-bottom: .3rem; font-size: .9rem; color: #94a3b8; }
    input, select {
      width: 100%; padding: .7rem; margin-bottom: 1rem;
      border: 1px solid #334155; border-radius: 8px;
      background: #0f172a; color: #e2e8f0; font-size: 1rem;
    }
    input:focus, select:focus { outline: none; border-color: #38bdf8; }
    button {
      width: 100%; padding: .75rem; border: none; border-radius: 8px;
      background: #0ea5e9; color: #fff; font-size: 1rem;
      cursor: pointer; font-weight: 600;
    }
    button:hover { background: #0284c7; }
    .scan-btn { background: #334155; margin-bottom: 1rem; font-size: .85rem; }
    .scan-btn:hover { background: #475569; }
    .msg { text-align: center; margin-top: 1rem; font-size: .9rem; }
    .msg.ok { color: #4ade80; }
    .msg.err { color: #f87171; }
  </style>
</head>
<body>
  <div class="card">
    <h2>Pet Feeder WiFi Setup</h2>
    <button class="scan-btn" onclick="scan()">Scan Networks</button>
    <div>
      <label for="ssid">SSID</label>
      <select id="ssidList" onchange="document.getElementById('ssid').value=this.value" style="display:none"></select>
      <input type="text" id="ssid" placeholder="Enter or select from above">
    </div>
    <div>
      <label for="pass">Password</label>
      <input type="password" id="pass" placeholder="WiFi password">
    </div>
    <button onclick="save()">Save & Connect</button>
    <div id="msg" class="msg"></div>
  </div>
  <script>
    function scan() {
      fetch('/scan').then(r=>r.json()).then(d=>{
        const sel = document.getElementById('ssidList');
        sel.innerHTML = '<option value="">-- Select a network --</option>';
        d.forEach(n => {
          const o = document.createElement('option');
          o.value = n.ssid; o.textContent = n.ssid + ' (' + n.rssi + ' dBm)';
          sel.appendChild(o);
        });
        sel.style.display = 'block';
      }).catch(()=> showMsg('Scan failed', false));
    }
    function save() {
      const ssid = document.getElementById('ssid').value;
      const pass = document.getElementById('pass').value;
      if (!ssid) { showMsg('Please enter an SSID', false); return; }
      fetch('/save', {
        method: 'POST',
        headers: {'Content-Type':'application/x-www-form-urlencoded'},
        body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)
      }).then(r=>r.json()).then(d=>{
        if (d.ok) {
          showMsg('Connecting... ESP will restart in 5 seconds.', true);
        } else {
          showMsg('Save failed: ' + d.msg, false);
        }
      }).catch(()=> showMsg('Request failed', false));
    }
    function showMsg(t, ok) {
      const m = document.getElementById('msg');
      m.textContent = t; m.className = 'msg ' + (ok ? 'ok' : 'err');
    }
  </script>
</body>
</html>
)rawliteral";

void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("[AP] Started AP: %s\n", AP_SSID);
  Serial.printf("[AP] Config page: http://%s\n", WiFi.softAPIP().toString().c_str());

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", htmlPage);
  });

  server.on("/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", json);
  });

  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    if (ssid.isEmpty()) {
      server.send(200, "application/json", "{\"ok\":false,\"msg\":\"SSID cannot be empty\"}");
      return;
    }
    if (writeFile(SSID_PATH, ssid) && writeFile(PASS_PATH, pass)) {
      server.send(200, "application/json", "{\"ok\":true}");
      Serial.printf("[FS] Saved SSID: %s\n", ssid.c_str());
      Serial.println("WiFi credentials updated, ESP will restart in 5 seconds");
      delay(5000);
      ESP.restart();
    } else {
      server.send(200, "application/json", "{\"ok\":false,\"msg\":\"Save failed\"}");
    }
  });

  server.begin();
  Serial.println("[AP] Web server started");
}

// ===== Main =====
void setup() {
  Serial.begin(115200);
  Serial.println("\n========== My Pet Feeder ==========");

  pinMode(LED_PIN, OUTPUT);
  pinMode(FEEDER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(FEEDER_PIN, HIGH);

  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS initialization failed!");
    return;
  }
  Serial.println("[FS] LittleFS ready");

  // Load settings
  loadFeedDuration();
  loadSchedules();

  // Try WiFi connection
  if (connectWiFi(savedSSID, savedPass)) {
    Serial.println("[System] WiFi connected, entering normal mode");

    // Sync time via NTP
    configTime(TZ_OFFSET, 0, NTP_SERVER);
    Serial.println("[NTP] Time sync started");

    // Setup MQTT (connect later in loop)
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
  } else {
    Serial.println("[System] Entering AP setup mode");
    startAP();
  }
}

void loop() {
  // === AP Mode ===
  if (WiFi.getMode() == WIFI_AP) {
    server.handleClient();
    return;
  }

  // === Normal Mode (WiFi connected) ===

  // Skip MQTT and other tasks if WiFi is disconnected
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_PIN, LOW);
    return;
  }

  // LED blink every 2 seconds
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink >= 2000) {
    lastBlink = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  // MQTT keep alive & reconnect
  if (!mqtt.connected()) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect >= 5000) {
      lastReconnect = millis();
      connectMQTT();
    }
  }
  mqtt.loop();

  // Publish status every 5 seconds
  if (millis() - lastStatusSend >= 5000) {
    lastStatusSend = millis();
    publishStatus();
  }

  // Publish schedules every 3 seconds
  if (millis() - lastScheduleSend >= 5000) {
    lastScheduleSend = millis();
    publishSchedules();
  }

  // Check schedules every 30 seconds
  if (millis() - lastScheduleCheck >= 30000) {
    lastScheduleCheck = millis();
    checkSchedules();
  }

  // Check feeding timeout
  checkFeedingTimeout();
}