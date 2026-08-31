// Cheap Yellow Display (ESP32-2432S028R) weather station.
//
// Polls larsi.org's weather API (github.com/larsi-org/html, weather/ section) for one
// station's latest observed conditions and shows them on the CYD's built-in 2.8" ILI9341
// screen. No touch, no audio -- just a display client.
//
// Config lives in secrets.h (gitignored) -- copy secrets.h.example to secrets.h and fill in
// your WiFi credentials and the ICAO station prefix to display (see
// https://larsi.org/weather/ for the station list).

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "CertBundle.h"
#include "secrets.h"

// --- CYD pin mapping (ESP32-2432S028R) ------------------------------------------------------
// Same wiring this repo's sibling projects landed on (see the "make/cyd/" page's yoRadio
// section): the display is on the "HSPI-pattern" pins (14/13/12), not the ESP32's default
// VSPI pins, so it needs its own SPIClass instance rather than the implicit default one.
#define TFT_SCLK  14
#define TFT_MISO  12
#define TFT_MOSI  13
#define TFT_CS    15
#define TFT_DC    2
#define TFT_RST   -1  // tied to EN
#define TFT_BL    21  // backlight, driven HIGH for full brightness

// Landscape, USB ports to the right. Flip to 3 if your board is mounted upside down.
#define SCREEN_ROTATION 1

SPIClass hspi(HSPI);
Adafruit_ILI9341 tft(&hspi, TFT_DC, TFT_CS, TFT_RST);

// --- Refresh timing --------------------------------------------------------------------------
const unsigned long REFRESH_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 minutes
unsigned long lastRefresh = 0;
bool firstRefresh = true;

// --- Station metadata (fetched once, rarely changes) ----------------------------------------
String stationLabel = "";
float stationLat = 0, stationLng = 0, stationEle = 0;
bool haveMetadata = false;

// --- Live reading, parsed out of weather/csv/current.php -------------------------------------
struct WeatherReading {
  bool valid = false;
  float temperatureC = 0;
  float dewPointC = 0;
  float humidityPct = 0;
  float pressureHpa = 0;
  float windDirDeg = 0;
  float windSpeedMs = 0;
  float clouds10th = 0;
};

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
  }

  if (!haveMetadata) {
    haveMetadata = fetchStationMetadata();
  }

  unsigned long now = millis();
  if (firstRefresh || now - lastRefresh >= REFRESH_INTERVAL_MS) {
    firstRefresh = false;
    lastRefresh = now;
    if (!haveMetadata) drawStatus("Fetching " WEATHER_PREFIX " info...");
    WeatherReading reading = fetchCurrentReading();
    if (reading.valid) {
      drawWeather(reading);
    } else {
      drawStatus("No data for station " WEATHER_PREFIX);
    }
  }

  delay(1000);
}

// --- Networking --------------------------------------------------------------------------

bool httpsGetLines(const String &path, void (*onLine)(const String &line)) {
  WiFiClientSecure client;
  client.setCACertBundle(kServerCertBundle, kServerCertBundleLen);

  if (!client.connect("larsi.org", 443)) {
    Serial.println("Connect failed");
    return false;
  }

  client.print(String("GET ") + path + " HTTP/1.1\r\n" +
               "Host: larsi.org\r\n" +
               "User-Agent: cyd-larsi-org-weather-station\r\n" +
               "Connection: close\r\n\r\n");

  // Skip HTTP headers.
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }

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

// A minimal quote-aware CSV splitter -- station descriptions can contain commas, and
// fputcsv() (the server side) quotes those fields, so a plain String.split on ',' would
// misalign every column after one.
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

bool g_metadataOk = false;
String g_metadataLabel;
float g_metadataLat = 0, g_metadataLng = 0, g_metadataEle = 0;

void onMetadataLine(const String &line) {
  // json/sensors.php returns one JSON object, not one-per-line -- but ArduinoJson isn't a
  // dependency here, so this project just does a cheap targeted extraction instead of a full
  // parse. Good enough for the handful of scalar fields this needs.
  int labelIdx = line.indexOf("\"label\":\"");
  if (labelIdx >= 0) {
    int start = labelIdx + 9;
    int end = line.indexOf('"', start);
    if (end > start) g_metadataLabel = line.substring(start, end);
  }
  auto extractFloat = [&](const char *key) -> float {
    String k = String("\"") + key + "\":";
    int idx = line.indexOf(k);
    if (idx < 0) return 0;
    int start = idx + k.length();
    int end = start;
    while (end < (int)line.length() &&
           (isDigit(line[end]) || line[end] == '-' || line[end] == '.')) {
      end++;
    }
    return line.substring(start, end).toFloat();
  };
  if (line.indexOf("\"lat\":") >= 0) g_metadataLat = extractFloat("lat");
  if (line.indexOf("\"lng\":") >= 0) g_metadataLng = extractFloat("lng");
  if (line.indexOf("\"ele\":") >= 0) g_metadataEle = extractFloat("ele");
  g_metadataOk = g_metadataLabel.length() > 0;
}

bool fetchStationMetadata() {
  g_metadataOk = false;
  g_metadataLabel = "";
  httpsGetLines(String("/weather/json/sensors.php?prefix=") + WEATHER_PREFIX, onMetadataLine);
  if (g_metadataOk) {
    stationLabel = g_metadataLabel;
    stationLat = g_metadataLat;
    stationLng = g_metadataLng;
    stationEle = g_metadataEle;
  }
  return g_metadataOk;
}

WeatherReading g_reading;

void onCurrentLine(const String &line) {
  // Columns: stationId,name,lat,lng,0,1,2,3,4,5,6
  //          (temp, dewpoint, humidity, pressure, wind dir, wind speed, clouds)
  String fields[11];
  int count = splitCsvLine(line, fields, 11);
  if (count < 11) return;
  if (fields[0] != WEATHER_PREFIX) return;

  g_reading.temperatureC = fields[4].toFloat();
  g_reading.dewPointC = fields[5].toFloat();
  g_reading.humidityPct = fields[6].toFloat();
  g_reading.pressureHpa = fields[7].toFloat();
  g_reading.windDirDeg = fields[8].toFloat();
  g_reading.windSpeedMs = fields[9].toFloat();
  g_reading.clouds10th = fields[10].toFloat();
  g_reading.valid = fields[4].length() > 0;  // empty string = sensor missing/stale
}

WeatherReading fetchCurrentReading() {
  g_reading = WeatherReading();
  httpsGetLines("/weather/csv/current.php", onCurrentLine);
  return g_reading;
}

// --- Display -----------------------------------------------------------------------------

void drawStatus(const String &message) {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 100);
  tft.println(message);
}

void drawRow(int y, const String &label, const String &value, uint16_t color) {
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_LIGHTGREY);
  tft.setCursor(10, y);
  tft.print(label);

  tft.setTextSize(2);
  tft.setTextColor(color);
  tft.setCursor(10, y + 12);
  tft.println(value);
}

void drawWeather(const WeatherReading &r) {
  tft.fillScreen(ILI9341_BLACK);

  tft.setTextColor(ILI9341_CYAN);
  tft.setTextSize(2);
  tft.setCursor(10, 6);
  tft.println(stationLabel.length() ? stationLabel : String(WEATHER_PREFIX));

  int y = 40;
  const int rowHeight = 42;

  drawRow(y, "TEMPERATURE", String(r.temperatureC, 1) + " C", ILI9341_YELLOW);
  y += rowHeight;
  drawRow(y, "DEW POINT", String(r.dewPointC, 1) + " C", ILI9341_WHITE);
  y += rowHeight;
  drawRow(y, "HUMIDITY", String(r.humidityPct, 0) + " %", ILI9341_WHITE);
  y += rowHeight;
  drawRow(y, "PRESSURE", String(r.pressureHpa, 0) + " hPa", ILI9341_WHITE);
  y += rowHeight;
  drawRow(y, "WIND", String(r.windDirDeg, 0) + (char)247 + " @ " +
                          String(r.windSpeedMs, 1) + " m/s",
          ILI9341_WHITE);
}
