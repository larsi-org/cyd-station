// Cheap Yellow Display (ESP32-2432S028R) sensor station.
//
// Polls larsi.org's sensors API (github.com/larsi-org/html, sensors/ section) for one
// station's channel list and latest readings, and shows them on the CYD's built-in 2.8"
// ILI9341 screen. No touch, no audio -- just a display client.
//
// Config lives in secrets.h (gitignored) -- copy secrets.h.example to secrets.h and fill in
// your WiFi credentials and the station prefix to display (see https://larsi.org/sensors/ for
// the station list).
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
#include "secrets.h"

// --- CYD pin mapping (ESP32-2432S028R) ------------------------------------------------------
// Same wiring as this repo's WeatherStation sketch -- see its header comment for why.
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_MOSI  13
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1  // tied to EN
#define TFT_BL    21  // backlight, driven HIGH for full brightness

#define SCREEN_ROTATION 1  // landscape, USB ports to the right; use 3 if mounted upside down

SPIClass hspi(HSPI);
Adafruit_ILI9341 tft(&hspi, TFT_DC, TFT_CS, TFT_RST);

// --- Refresh timing --------------------------------------------------------------------------
const unsigned long REFRESH_INTERVAL_MS = 60UL * 1000UL;  // 1 minute
unsigned long lastRefresh = 0;
bool firstRefresh = true;

// A station's device/sensor layout barely ever changes, so metadata (channel list, labels,
// units) is only re-fetched occasionally, on a much longer cycle than live readings.
const unsigned long METADATA_INTERVAL_MS = 60UL * 60UL * 1000UL;  // 1 hour
unsigned long lastMetadataFetch = 0;
bool haveMetadata = false;

// How far back to look for each channel's latest reading. Generous on purpose: this is
// processed as a stream (one CSV row at a time, always overwriting a channel's "latest so
// far"), so a wide window costs parse time, not RAM, and protects against a sensor that only
// logs sparsely still showing up.
const unsigned long READING_WINDOW_SECONDS = 6UL * 60UL * 60UL;  // 6 hours

const int MAX_SENSORS = 8;  // a station can have up to 256 channels; only the first
                             // MAX_SENSORS (in channel order) fit this screen

struct SensorMeta {
  int channel = -1;
  String label;     // owning device's name
  String property;  // e.g. "Temperature"
  String unit;
};

struct SensorReading {
  bool valid = false;
  float value = 0;
  unsigned long epoch = 0;
};

String stationLabel;
SensorMeta sensors[MAX_SENSORS];
int sensorCount = 0;
SensorReading readings[MAX_SENSORS];

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  hspi.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(SCREEN_ROTATION);
  tft.fillScreen(ILI9341_BLACK);
  drawStatus("Connecting to WiFi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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
    if (!haveMetadata) drawStatus("Fetching " SENSOR_PREFIX " info...");
    if (fetchSensorMetadata()) {
      haveMetadata = true;
      lastMetadataFetch = now;
    }
  }

  if (haveMetadata && (firstRefresh || now - lastRefresh >= REFRESH_INTERVAL_MS)) {
    firstRefresh = false;
    lastRefresh = now;
    fetchLatestReadings();
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
  client.connect("larsi.org", 443);
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
               "Host: larsi.org\r\n" +
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

// A minimal quote-aware CSV splitter -- see WeatherStation's sketch for the same helper and
// why plain String.split() on ',' isn't safe here.
int splitCsvLine(const String &line, String out[], int maxFields) {
  int field = 0;
  int i = 0;
  int len = line.length();
  while (i < len && field < maxFields) {
    String value = "";
    if (line[i] == '"') {
      i++;
      while (i < len) {
        if (line[i] == '"') {
          if (i + 1 < len && line[i + 1] == '"') {
            value += '"';
            i += 2;
          } else {
            i++;
            break;
          }
        } else {
          value += line[i++];
        }
      }
    } else {
      while (i < len && line[i] != ',') value += line[i++];
    }
    out[field++] = value;
    if (i < len && line[i] == ',') i++;
  }
  return field;
}

bool fetchSensorMetadata() {
  WiFiClientSecure client = connectApi();
  if (!client.connected()) {
    Serial.println("Connect failed");
    return false;
  }
  String path = String("/sensors/json/sensors.php?prefix=") + SENSOR_PREFIX;
  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: larsi.org\r\n" +
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

  stationLabel = doc["label"] | SENSOR_PREFIX;

  sensorCount = 0;
  JsonArray arr = doc["sensors"];
  for (JsonObject s : arr) {
    if (sensorCount >= MAX_SENSORS) break;
    sensors[sensorCount].channel = s["value"] | -1;
    sensors[sensorCount].label = String((const char *)(s["label"] | ""));
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

void onDataLine(const String &line) {
  // Columns: t,s,v  (epoch, channel, value) -- ordered by epoch, so later rows always
  // overwrite earlier ones for the same channel, leaving the true latest after the stream.
  String fields[3];
  int count = splitCsvLine(line, fields, 3);
  if (count < 3) return;

  int channel = fields[1].toInt();
  int idx = findSensorIndex(channel);
  if (idx < 0) return;

  readings[idx].epoch = strtoul(fields[0].c_str(), nullptr, 10);
  readings[idx].value = fields[2].toFloat();
  readings[idx].valid = true;
}

void fetchLatestReadings() {
  for (int i = 0; i < sensorCount; i++) readings[i] = SensorReading();

  unsigned long nowEpoch = time(nullptr);
  unsigned long tMin = nowEpoch > READING_WINDOW_SECONDS ? nowEpoch - READING_WINDOW_SECONDS : 0;

  String path = String("/sensors/csv/data.php?prefix=") + SENSOR_PREFIX +
                "&t_min=" + String(tMin) + "&t_max=" + String(nowEpoch);
  httpsGetLines(path, onDataLine);
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
  tft.println(stationLabel);

  int y = 34;
  const int rowHeight = 25;

  for (int i = 0; i < sensorCount; i++) {
    tft.setTextSize(1);
    tft.setCursor(10, y);
    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.print(sensors[i].property.length() ? sensors[i].property : sensors[i].label);

    tft.setCursor(220, y);
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
