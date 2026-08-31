// Cheap Yellow Display (ESP32-2432S028R) sensor station.
//
// Polls larsi.org's sensors API (github.com/larsi-org/html, sensors/ section) for one
// station's channel list and latest readings, and shows them on the CYD's built-in 2.8"
// ILI9341 screen. No touch, no audio -- just a display client.
//
// This sketch and its sibling WeatherStation are deliberately near-identical: both sections'
// APIs follow the same shape (json/sensors.php?prefix=X for channel metadata,
// csv/current.php?prefix=X for latest values as channel,value,epoch rows), so both sketches
// share the same fetch/parse/render structure -- only the API base path and default station
// differ. See this repo's README for why the wire parameter stayed `prefix` rather than
// `station`.
//
// Wi-Fi, station prefix, and server URL are all set at runtime via a captive setup portal
// (CydPortal.h) rather than compiled in -- on first boot, or whenever none of the up-to-3
// saved networks connect, this opens an access point ("CYD-Sensors-Setup-xxxxxx") with a
// config page. See CydConfig.h for what's persisted (NVS) and how the network list works.
//
// Requires the ArduinoJson library (Library Manager -> "ArduinoJson", tested against 7.x).

#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "CertBundle.h"
#include "CydConfig.h"
#include "CydPortal.h"

// --- CYD pin mapping (ESP32-2432S028R) ------------------------------------------------------
// The display is on the "HSPI-pattern" pins (14/13/12), not the ESP32's default VSPI pins, so
// it needs its own SPIClass instance rather than the implicit default one.
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_MOSI  13
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1  // tied to EN
#define TFT_BL    21  // backlight, driven HIGH for full brightness

#define SCREEN_ROTATION 0  // portrait; use 2 if mounted upside down

SPIClass hspi(HSPI);
Adafruit_ILI9341 tft(&hspi, TFT_DC, TFT_CS, TFT_RST);

CydConfig config;
String apiHost;
String apiBasePath;

// --- Refresh timing --------------------------------------------------------------------------
const unsigned long REFRESH_INTERVAL_MS = 60UL * 1000UL;  // 1 minute
unsigned long lastRefresh = 0;
bool firstRefresh = true;

// A station's channel layout barely ever changes, so metadata (channel list, labels, units)
// is only re-fetched occasionally, on a much longer cycle than live readings.
const unsigned long METADATA_INTERVAL_MS = 60UL * 60UL * 1000UL;  // 1 hour
unsigned long lastMetadataFetch = 0;
bool haveMetadata = false;

const int MAX_SENSORS = 64;  // a station can have up to 256 channels, though nowhere near that
                              // many in practice today -- generous since pagination (below)
                              // means a station no longer has to fit on one screen.

// Sensors that don't fit on one page cycle automatically -- header stays put, only the rows
// below it change. ROWS_PER_PAGE is derived from the portrait screen: (320 tall - 40 header) /
// 25 per row.
const int ROWS_PER_PAGE = 11;
const unsigned long PAGE_INTERVAL_MS = 5UL * 1000UL;  // 5 seconds per page
unsigned long lastPageFlip = 0;
int currentPage = 0;

struct SensorMeta {
  int channel = -1;
  String property;  // e.g. "Temperature"
  String unit;
};

struct SensorReading {
  bool valid = false;
  float value = 0;
  unsigned long epoch = 0;
};

SensorMeta sensors[MAX_SENSORS];
int sensorCount = 0;
SensorReading readings[MAX_SENSORS];

// Tries each saved network in turn (CydConfig's up-to-3 list), most-recently-added first.
// Returns false if none connect within timeoutMs each -- caller falls back to the setup portal.
bool connectToKnownNetwork(unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  for (uint8_t i = 0; i < CydConfig::kMaxNetworks; i++) {
    if (config.ssids[i].length() == 0) continue;
    drawStatus("Connecting to " + config.ssids[i] + "...");
    WiFi.begin(config.ssids[i].c_str(), config.passwords[i].c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect();
  }
  return false;
}

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  hspi.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(ILI9341_BLACK);
  drawStatus("Starting...");

  bool haveSettings = loadCydConfig(config);
  if (!haveSettings || !connectToKnownNetwork(15000)) {
    drawStatus("Starting setup portal...");
    runCydSetupPortal();  // never returns -- restarts the device once the form is saved
  }

  if (!parseServerUrl(config.baseUrl, apiHost, apiBasePath)) {
    // Shouldn't happen -- CydConfig's own default is always a well-formed URL, and the
    // portal validates any user-submitted one with this same function before saving. If the
    // saved value is somehow broken anyway, treat it like incomplete config rather than
    // silently guessing a server this device wasn't actually told to use.
    drawStatus("Invalid server URL, opening setup portal...");
    delay(2000);
    runCydSetupPortal();  // never returns -- restarts the device once the form is saved
  }

  syncTime();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    drawStatus("WiFi disconnected, reconnecting...");
    WiFi.reconnect();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
      delay(2000);
      return;
    }
    syncTime();
  }

  if (time(nullptr) < 1700000000) {  // not yet NTP-synced
    drawStatus("Waiting for time sync...");
    syncTime();
    delay(1000);
    return;
  }

  unsigned long now = millis();

  if (!haveMetadata || now - lastMetadataFetch >= METADATA_INTERVAL_MS) {
    if (!haveMetadata) drawStatus("Fetching " + config.stationPrefix + " info...");
    if (fetchStationMetadata()) {
      haveMetadata = true;
      lastMetadataFetch = now;
    }
  }

  if (haveMetadata && (firstRefresh || now - lastRefresh >= REFRESH_INTERVAL_MS)) {
    firstRefresh = false;
    lastRefresh = now;
    fetchLatestReadings();
    currentPage = 0;
    lastPageFlip = now;
    drawSensors();
  } else if (haveMetadata && totalPages() > 1 && now - lastPageFlip >= PAGE_INTERVAL_MS) {
    currentPage = (currentPage + 1) % totalPages();
    lastPageFlip = now;
    drawSensors();
  }

  delay(1000);
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

// --- Networking --------------------------------------------------------------------------

WiFiClientSecure connectApi() {
  WiFiClientSecure client;
  client.setCACertBundle(kServerCertBundle, kServerCertBundleLen);
  client.connect(apiHost.c_str(), 443);
  return client;
}

// Reads and discards HTTP response headers, leaving the stream positioned at the body.
void skipHttpHeaders(WiFiClientSecure &client) {
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
}

bool httpsGetLines(const String &path, void (*onLine)(const String &line)) {
  WiFiClientSecure client = connectApi();
  if (!client.connected()) {
    Serial.println("Connect failed");
    return false;
  }
  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + apiHost + "\r\n" +
               "User-Agent: cyd-larsi-org-sensors-station\r\n" +
               "Connection: close\r\n\r\n");
  skipHttpHeaders(client);
  while (client.connected() || client.available()) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) onLine(line);
    }
  }
  client.stop();
  return true;
}

bool fetchStationMetadata() {
  WiFiClientSecure client = connectApi();
  if (!client.connected()) {
    Serial.println("Connect failed");
    return false;
  }
  String path = apiBasePath + "json/sensors.php?prefix=" + config.stationPrefix;
  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: " + apiHost + "\r\n" +
               "User-Agent: cyd-larsi-org-sensors-station\r\n" +
               "Connection: close\r\n\r\n");
  skipHttpHeaders(client);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, client);
  client.stop();

  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  sensorCount = 0;
  JsonArray arr = doc["sensors"];
  for (JsonObject s : arr) {
    if (sensorCount >= MAX_SENSORS) break;
    sensors[sensorCount].channel = s["value"] | -1;
    sensors[sensorCount].property = String((const char *)(s["property"] | ""));
    sensors[sensorCount].unit = String((const char *)(s["unit"] | ""));
    sensorCount++;
  }
  return sensorCount > 0;
}

int findSensorIndex(int channel) {
  for (int i = 0; i < sensorCount; i++) {
    if (sensors[i].channel == channel) return i;
  }
  return -1;
}

void onCurrentLine(const String &line) {
  // Columns: channel,value,epoch -- always plain numbers, no CSV quoting to worry about.
  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  if (c1 < 0 || c2 < 0) return;

  int channel = line.substring(0, c1).toInt();
  int idx = findSensorIndex(channel);
  if (idx < 0) return;

  readings[idx].value = line.substring(c1 + 1, c2).toFloat();
  readings[idx].epoch = strtoul(line.substring(c2 + 1).c_str(), nullptr, 10);
  readings[idx].valid = true;
}

void fetchLatestReadings() {
  for (int i = 0; i < sensorCount; i++) readings[i] = SensorReading();
  String path = apiBasePath + "csv/current.php?prefix=" + config.stationPrefix;
  httpsGetLines(path, onCurrentLine);
}

int totalPages() {
  if (sensorCount == 0) return 1;
  return (sensorCount + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
}

// --- Display -----------------------------------------------------------------------------

void drawStatus(const String &message) {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 100);
  tft.println(message);
}

String formatAge(unsigned long epoch) {
  if (epoch == 0) return "never";
  long ageSeconds = (long)time(nullptr) - (long)epoch;
  if (ageSeconds < 60) return String(ageSeconds) + "s ago";
  if (ageSeconds < 3600) return String(ageSeconds / 60) + "m ago";
  return String(ageSeconds / 3600) + "h ago";
}

void drawSensors() {
  tft.fillScreen(ILI9341_BLACK);

  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(10, 6);
  tft.println(config.stationPrefix);

  if (totalPages() > 1) {
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_DARKGREY);
    tft.setCursor(180, 12);
    tft.print(String(currentPage + 1) + "/" + String(totalPages()));
  }

  int y = 34;
  const int rowHeight = 25;

  int first = currentPage * ROWS_PER_PAGE;
  int last = min(sensorCount, first + ROWS_PER_PAGE);

  for (int i = first; i < last; i++) {
    tft.setTextSize(1);
    tft.setCursor(10, y);
    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.print(sensors[i].property);

    tft.setCursor(165, y);
    if (readings[i].valid) {
      tft.setTextColor(ILI9341_DARKGREY);
      tft.print(formatAge(readings[i].epoch));
    }

    tft.setTextSize(2);
    tft.setCursor(10, y + 9);
    if (readings[i].valid) {
      tft.setTextColor(ILI9341_YELLOW);
      tft.print(String(readings[i].value, 1) + " " + sensors[i].unit);
    } else {
      tft.setTextColor(ILI9341_RED);
      tft.print("no data");
    }

    y += rowHeight;
  }
}
