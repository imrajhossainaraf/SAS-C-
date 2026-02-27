#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <RTClib.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "esp_task_wdt.h"

// ================= PIN SETUP =================

#define SS_PIN 5
#define RST_PIN 4
#define BUZZER_PIN 2
#define HOTSPOT_BTN 32

// ================= OBJECTS =================

MFRC522 rfid(SS_PIN, RST_PIN);
RTC_DS3231 rtc;
WebServer server(80);
Preferences prefs;

// ================= CONFIG =================

String cfg_ssid = "";
String cfg_password = "";
String cfg_serverURL = "";
String cfg_deviceName = "ESP32";
unsigned long cfg_interval = 60000;

const char* AP_SSID = "RFID-Setup";
const char* AP_PASS = "rfid1234";

const char* storageFile = "/records.txt";

// ================= STATE =================

bool hotspotRunning = false;
bool wifiConnected = false;
bool lastBtnState = false;
bool rtcPresent = false;

// FIX #7: Flag to trigger restart from loop() instead of inside web handler
bool pendingRestart = false;
unsigned long restartTime = 0;

unsigned long lastWifiAttempt = 0;
unsigned long lastSendAttempt = 0;

bool buzzerActive = false;
unsigned long buzzerStartTime = 0;
const unsigned long BUZZER_DURATION = 60;

int scanCountInterval = 0;

unsigned long lastSerialStatusTime = 0;
const unsigned long SERIAL_STATUS_INTERVAL = 10000;

// ================= SERIAL LOG =================

String getTimestamp()
{
  if (rtcPresent)
  {
    DateTime now = rtc.now();
    char ts[20];
    sprintf(ts, "%04d-%02d-%02d %02d:%02d:%02d",
      now.year(), now.month(), now.day(),
      now.hour(), now.minute(), now.second());
    return String(ts);
  }

  unsigned long ms = millis() / 1000;
  char ts[20];
  sprintf(ts, "[%06lu]", ms);
  return String(ts);
}

void log(String msg)
{
  Serial.println("[" + getTimestamp() + "] " + msg);
  Serial.flush();
}

void logAction(String action, String detail = "")
{
  String msg = "ACTION: " + action;
  if (detail.length() > 0) msg += " | " + detail;
  log(msg);
}

// ================= STORAGE =================

void storeRecord(String uid, String ts)
{
  File f = SPIFFS.open(storageFile, FILE_APPEND);

  if (!f)
  {
    log("ERROR: Failed to open storage file for writing");
    return;
  }

  f.print(uid);
  f.print(",");
  f.println(ts);
  f.close();

  log("Record stored: " + uid + " at " + ts);
}

// ================= HOTSPOT =================

void handleHotspot()
{
  bool btn = digitalRead(HOTSPOT_BTN);

  if (btn == lastBtnState) return;
  lastBtnState = btn;

  if (btn && !hotspotRunning)
  {
    WiFi.softAP(AP_SSID, AP_PASS);
    hotspotRunning = true;

    log("HOTSPOT ENABLED - SSID: " + String(AP_SSID) + " | IP: " + WiFi.softAPIP().toString());
    log("Web config: http://" + WiFi.softAPIP().toString() + "/");
  }

  if (!btn && hotspotRunning)
  {
    WiFi.softAPdisconnect(true);
    hotspotRunning = false;
    log("HOTSPOT DISABLED");
  }
}

// ================= WIFI =================

void handleWiFi()
{
  if (cfg_ssid == "")
  {
    static bool warned = false;
    if (!warned)
    {
      log("WARNING: No WiFi credentials configured. Hold button to start hotspot.");
      warned = true;
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    if (!wifiConnected)
    {
      wifiConnected = true;
      logAction("WIFI_CONNECTED", "IP: " + WiFi.localIP().toString() + " | RSSI: " + String(WiFi.RSSI()) + " dBm");
    }
    return;
  }

  if (wifiConnected)
  {
    wifiConnected = false;
    log("WiFi disconnected - will retry...");
  }

  if (millis() - lastWifiAttempt < 10000) return;

  lastWifiAttempt = millis();

  // FIX #2: Fully non-blocking — just call begin() and return.
  // Status is checked on the next loop iteration.
  log("Connecting to WiFi: " + cfg_ssid);
  WiFi.begin(cfg_ssid.c_str(), cfg_password.c_str());
}

// ================= NTP TIME SYNC =================

void syncTimeWithNTP()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    log("NTP SYNC: Skipped - WiFi not connected");
    return;
  }

  logAction("NTP_SYNC_START");

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  struct tm timeinfo;
  int retries = 0;
  const int maxRetries = 10;

  while (!getLocalTime(&timeinfo) && retries < maxRetries)
  {
    Serial.print(".");
    esp_task_wdt_reset(); // FIX #3/#4: Reset watchdog inside blocking loops
    delay(1000);
    retries++;
  }
  Serial.println();

  if (getLocalTime(&timeinfo))
  {
    if (rtcPresent)
    {
      rtc.adjust(DateTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      ));
      logAction("NTP_SYNC_SUCCESS", "RTC updated");
    }
    else
    {
      log("NTP time obtained but RTC not available");
    }
  }
  else
  {
    log("ERROR: NTP sync failed after " + String(maxRetries) + " attempts");
  }
}

// ================= SEND =================

bool sendRecords()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    log("SEND SKIPPED: No WiFi connection");
    return false;
  }

  if (!SPIFFS.exists(storageFile))
  {
    log("SEND SKIPPED: No records file found");
    return false;
  }

  File checkFile = SPIFFS.open(storageFile);
  if (checkFile)
  {
    size_t fileSize = checkFile.size();
    checkFile.close();

    log("Storage file size: " + String(fileSize) + " bytes");

    if (fileSize == 0)
    {
      SPIFFS.remove(storageFile);
      log("Empty file removed");
      return false;
    }
  }

  logAction("SEND_START");

  const int MAX_RECORDS = 50;

  DynamicJsonDocument doc(4096);
  doc["device"] = cfg_deviceName;
  doc["timestamp"] = getTimestamp();

  JsonArray arr = doc.createNestedArray("records");

  File f = SPIFFS.open(storageFile);
  int count = 0;

  if (f)
  {
    while (f.available() && count < MAX_RECORDS)
    {
      String line = f.readStringUntil('\n');
      line.trim();

      int idx = line.indexOf(',');

      if (idx > 0)
      {
        JsonObject obj = arr.createNestedObject();
        obj["uid"] = line.substring(0, idx);
        obj["time"] = line.substring(idx + 1);
        count++;
      }
    }
    f.close();
    log("Read " + String(count) + " records from file");
  }

  if (arr.size() == 0)
  {
    doc["status"] = "no scans";
    log("No valid records found in file");
  }

  String body;
  serializeJson(doc, body);
  log("Payload size: " + String(body.length()) + " bytes");

  HTTPClient http;
  http.begin(cfg_serverURL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);

  logAction("SEND_HTTP_POST", "URL: " + cfg_serverURL);

  int code = http.POST(body);
  String response = http.getString();
  http.end();

  log("Server response code: " + String(code));
  if (response.length() > 0)
    log("Server response: " + response.substring(0, 100));

  if (code >= 200 && code < 300)
  {
    logAction("SEND_SUCCESS", "Code: " + String(code) + " | Records: " + String(count));

    if (count < MAX_RECORDS)
    {
      SPIFFS.remove(storageFile);
      log("All records sent and cleared (" + String(count) + " records)");
    }
    else
    {
      File src = SPIFFS.open(storageFile);
      File tmp = SPIFFS.open("/records_tmp.txt", FILE_WRITE);

      if (src && tmp)
      {
        int skipped = 0;
        while (src.available())
        {
          String line = src.readStringUntil('\n');
          line.trim();
          if (skipped < count) { skipped++; continue; }
          if (line.length() > 0) tmp.println(line);
        }
        log("Partial send: " + String(count) + " sent, remainder preserved");
      }

      if (src) src.close();
      if (tmp) tmp.close();

      SPIFFS.remove(storageFile);
      SPIFFS.rename("/records_tmp.txt", storageFile);
    }

    return true;
  }
  else
  {
    logAction("SEND_FAILED", "HTTP Code: " + String(code));
    return false;
  }
}

// ================= RFID =================

void handleRFID()
{
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())
  {
    log("ERROR: Failed to read card serial");
    return;
  }

  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++)
  {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }

  uid.toUpperCase();

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  String timestamp;

  if (rtcPresent)
  {
    DateTime now = rtc.now();
    char ts[25];
    sprintf(ts, "%04d-%02d-%02dT%02d:%02d:%02d",
      now.year(), now.month(), now.day(),
      now.hour(), now.minute(), now.second());
    timestamp = String(ts);
  }
  else
  {
    timestamp = "millis:" + String(millis());
  }

  storeRecord(uid, timestamp);

  scanCountInterval++;

  buzzerActive = true;
  buzzerStartTime = millis();
  digitalWrite(BUZZER_PIN, HIGH);

  logAction("RFID_SCAN", "UID: " + uid + " | Scan #" + String(scanCountInterval) + " this interval");
}

// ================= BUZZER =================

void handleBuzzer()
{
  if (buzzerActive && millis() - buzzerStartTime >= BUZZER_DURATION)
  {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
  }
}

// ================= CONFIG =================

void loadConfig()
{
  prefs.begin("cfg", true);

  cfg_ssid = prefs.getString("ssid", "");
  cfg_password = prefs.getString("pass", "");
  cfg_serverURL = prefs.getString("url", "");
  cfg_deviceName = prefs.getString("dev", "ESP32");
  cfg_interval = prefs.getULong("int", 60000);

  prefs.end();

  log("Config loaded:");
  log("  SSID: " + (cfg_ssid.length() > 0 ? cfg_ssid : "[NOT SET]"));
  log("  Server URL: " + (cfg_serverURL.length() > 0 ? cfg_serverURL : "[NOT SET]"));
  log("  Device Name: " + cfg_deviceName);
  log("  Upload Interval: " + String(cfg_interval / 1000) + "s");
}

void saveConfig()
{
  prefs.begin("cfg", false);

  prefs.putString("ssid", cfg_ssid);
  prefs.putString("pass", cfg_password);
  prefs.putString("url", cfg_serverURL);
  prefs.putString("dev", cfg_deviceName);
  prefs.putULong("int", cfg_interval);

  prefs.end();

  log("Config saved");
}

// ================= WEB =================

void handleRoot()
{
  log("Web config page accessed by: " + server.client().remoteIP().toString());

  String html = "<!DOCTYPE html><html><head><title>RFID Scanner Config</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;margin:20px;background:#f0f0f0;}";
  html += ".container{max-width:500px;margin:auto;background:white;padding:20px;border-radius:10px;}";
  html += "input{width:100%;padding:8px;margin:5px 0 15px;box-sizing:border-box;border:1px solid #ddd;border-radius:4px;}";
  html += "button{background:#4CAF50;color:white;padding:10px 15px;border:none;border-radius:4px;cursor:pointer;}";
  html += "button:hover{background:#45a049;}</style></head>";
  html += "<body><div class='container'><h2>RFID Scanner Configuration</h2>";
  html += "<form id='configForm'><label>WiFi SSID:</label>";
  html += "<input type='text' id='ssid' value='" + cfg_ssid + "' required>";
  html += "<label>WiFi Password:</label>";
  // FIX #6: Never embed password in HTML source — blank field, only update if user types a new one
  html += "<input type='password' id='pass' placeholder='Leave blank to keep current'>";
  html += "<label>Server URL:</label>";
  html += "<input type='url' id='url' value='" + cfg_serverURL + "' placeholder='http://yourserver.com/api'>";
  html += "<label>Device Name:</label>";
  html += "<input type='text' id='dev' value='" + cfg_deviceName + "'>";
  html += "<label>Upload Interval (ms, min 5000):</label>";
  html += "<input type='number' id='interval' value='" + String(cfg_interval) + "' min='5000'>";
  html += "<button type='submit'>Save and Restart</button></form>";
  html += "<script>document.getElementById('configForm').onsubmit=async(e)=>{";
  html += "e.preventDefault();";
  html += "const passVal=document.getElementById('pass').value;";
  html += "const data={ssid:document.getElementById('ssid').value,";
  // Only include password in payload if user actually typed something
  html += "pass:passVal.length>0?passVal:null,";
  html += "url:document.getElementById('url').value,";
  html += "dev:document.getElementById('dev').value,";
  html += "interval:parseInt(document.getElementById('interval').value)};";
  html += "const r=await fetch('/save',{method:'POST',body:JSON.stringify(data)});";
  html += "if(r.ok)alert('Saved! Device restarting...');else alert('Error: '+await r.text());";
  html += "}</script></div></body></html>";

  server.send(200, "text/html", html);
}

void handleConfigGet()
{
  DynamicJsonDocument doc(512);

  doc["ssid"] = cfg_ssid;
  // FIX #6: Never send password over the API
  doc["pass"] = "";
  doc["url"] = cfg_serverURL;
  doc["dev"] = cfg_deviceName;
  doc["interval"] = cfg_interval;

  String s;
  serializeJson(doc, s);

  server.send(200, "application/json", s);
}

void handleConfigSave()
{
  log("Config save request from: " + server.client().remoteIP().toString());

  DynamicJsonDocument doc(512);

  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  if (error)
  {
    log("ERROR: Invalid JSON received: " + String(error.c_str()));
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }

  String new_ssid = doc["ssid"].as<String>();
  String new_url = doc["url"].as<String>();
  unsigned long new_interval = doc["interval"].as<unsigned long>();

  if (new_ssid.length() == 0 || new_ssid.length() > 31)
  {
    server.send(400, "text/plain", "Invalid SSID (must be 1-31 chars)");
    return;
  }
  cfg_ssid = new_ssid;

  // FIX #6: Only update password if a new one was actually provided
  if (!doc["pass"].isNull() && doc["pass"].as<String>().length() > 0)
  {
    cfg_password = doc["pass"].as<String>();
    log("Password updated");
  }
  else
  {
    log("Password unchanged");
  }

  if (new_url.length() > 0)
  {
    if (new_url.startsWith("http://") || new_url.startsWith("https://"))
    {
      cfg_serverURL = new_url;
    }
    else
    {
      server.send(400, "text/plain", "Invalid URL (must start with http:// or https://)");
      return;
    }
  }

  cfg_deviceName = doc["dev"].as<String>();

  if (new_interval >= 5000)
  {
    cfg_interval = new_interval;
  }
  else
  {
    server.send(400, "text/plain", "Interval must be at least 5000ms");
    return;
  }

  saveConfig();

  // FIX #7: Send response immediately, then let loop() handle the restart
  server.send(200, "text/plain", "OK");

  pendingRestart = true;
  restartTime = millis() + 500;

  log("Config saved - restart scheduled");
}

void handleStatus()
{
  String status = "=== SYSTEM STATUS ===\n";
  status += "Device: " + cfg_deviceName + "\n";
  status += "Uptime: " + String(millis() / 1000) + " seconds\n";
  status += "Free heap: " + String(ESP.getFreeHeap()) + " bytes\n";
  status += "WiFi: " + String(wifiConnected ? "Connected" : "Disconnected") + "\n";
  if (wifiConnected) status += "IP: " + WiFi.localIP().toString() + "\n";
  status += "Hotspot: " + String(hotspotRunning ? "Active" : "Inactive") + "\n";
  status += "RTC: " + String(rtcPresent ? "Present" : "Not found") + "\n";
  status += "Records pending: " + String(SPIFFS.exists(storageFile) ? "Yes" : "None") + "\n";
  status += "Scans this interval: " + String(scanCountInterval) + "\n";

  server.send(200, "text/plain", status);
}

// ================= STATUS DISPLAY =================

void printStatusToSerial()
{
  Serial.println("\n=== SYSTEM STATUS ===");
  Serial.println("Device: " + cfg_deviceName);
  Serial.println("Uptime: " + String(millis() / 1000) + " seconds");
  Serial.println("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
  Serial.println("WiFi: " + String(wifiConnected ? "Connected" : "Disconnected"));
  if (wifiConnected)
  {
    Serial.println("  IP: " + WiFi.localIP().toString());
    Serial.println("  RSSI: " + String(WiFi.RSSI()) + " dBm");
  }
  Serial.println("Hotspot: " + String(hotspotRunning ? "Active" : "Inactive"));
  if (hotspotRunning) Serial.println("  IP: " + WiFi.softAPIP().toString());
  Serial.println("RTC: " + String(rtcPresent ? "Present" : "Not found"));

  // FIX #5: Single file open for both size and line count
  if (SPIFFS.exists(storageFile))
  {
    File f = SPIFFS.open(storageFile);
    if (f)
    {
      size_t size = f.size();
      int lineCount = 0;
      while (f.available()) { f.readStringUntil('\n'); lineCount++; }
      f.close();
      Serial.println("Storage: " + String(size) + " bytes, " + String(lineCount) + " records");
    }
  }
  else
  {
    Serial.println("Storage: No pending records");
  }

  Serial.println("Scans this interval: " + String(scanCountInterval));
  Serial.println("Upload interval: " + String(cfg_interval / 1000) + "s");
  Serial.println("=====================\n");
}

// ================= SETUP =================

void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n=== RFID SCANNER SYSTEM STARTING ===");
  Serial.println("Firmware version: 2.1");
  Serial.println("Chip: " + String(ESP.getChipModel()));
  Serial.println("Cores: " + String(ESP.getChipCores()));
  Serial.println("Flash: " + String(ESP.getFlashChipSize() / 1024 / 1024) + " MB");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(HOTSPOT_BTN, INPUT_PULLDOWN);

  Wire.begin(21, 22);

  if (!SPIFFS.begin(true))
  {
    Serial.println("ERROR: SPIFFS mount failed");
  }
  else
  {
    Serial.println("SPIFFS: " + String(SPIFFS.usedBytes()) + "/" + String(SPIFFS.totalBytes()) + " bytes used");
  }

  SPI.begin();

  rfid.PCD_Init();
  rfid.PCD_DumpVersionToSerial();
  Serial.println("RFID reader initialized");

  if (!rtc.begin())
  {
    Serial.println("WARNING: RTC not found - using millis() fallback for timestamps");
    rtcPresent = false;
  }
  else
  {
    rtcPresent = true;
    if (rtc.lostPower()) Serial.println("WARNING: RTC lost power - time may be inaccurate");
    DateTime now = rtc.now();
    char buf[25];
    sprintf(buf, "%04d-%02d-%02dT%02d:%02d:%02d",
      now.year(), now.month(), now.day(),
      now.hour(), now.minute(), now.second());
    Serial.println("RTC initialized: " + String(buf));
  }

  loadConfig();

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  server.on("/", handleRoot);
  server.on("/config", handleConfigGet);
  server.on("/save", HTTP_POST, handleConfigSave);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("Web server started");

  // FIX #3: Init watchdog with generous timeout BEFORE any blocking operations,
  // and reset it inside every blocking loop so it never fires spuriously
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true
};
esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
  Serial.println("Watchdog initialized (30s timeout)");

  // Initial NTP sync
  if (cfg_ssid.length() > 0 && rtcPresent)
  {
    Serial.println("Connecting for initial NTP sync...");
    WiFi.begin(cfg_ssid.c_str(), cfg_password.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20)
    {
      delay(500);
      Serial.print(".");
      esp_task_wdt_reset(); // FIX #3/#4: Keep watchdog fed during blocking connect
      attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("Connected - syncing NTP...");
      syncTimeWithNTP();
    }
    else
    {
      Serial.println("Could not connect for NTP sync - will retry when WiFi is available");
    }
  }

  // Startup buzzer confirmation
  digitalWrite(BUZZER_PIN, HIGH);
  delay(100);
  digitalWrite(BUZZER_PIN, LOW);

  logAction("SYSTEM_READY");
  printStatusToSerial();
}

// ================= LOOP =================

void loop()
{
  server.handleClient();

  handleHotspot();

  handleWiFi();

  handleRFID();

  handleBuzzer();

  // FIX #7: Restart handled here after HTTP response has already been sent
  if (pendingRestart && millis() >= restartTime)
  {
    log("Restarting now...");
    ESP.restart();
  }

  if (millis() - lastSendAttempt > cfg_interval)
  {
    lastSendAttempt = millis();

    if (scanCountInterval == 0)
      log("No scans in this interval");

    sendRecords();

    scanCountInterval = 0;
  }

  // NTP sync every 24 hours
  static unsigned long lastNTPSync = 0;
  if (rtcPresent && wifiConnected && millis() - lastNTPSync > 86400000UL)
  {
    syncTimeWithNTP();
    lastNTPSync = millis();
  }

  // Periodic serial status
  if (millis() - lastSerialStatusTime > SERIAL_STATUS_INTERVAL)
  {
    lastSerialStatusTime = millis();
    printStatusToSerial();
  }

  esp_task_wdt_reset();

  delay(10);
}
