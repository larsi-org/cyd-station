// Cheap Yellow Display (ESP32-2432S028R) status station.
//
// Polls one of larsi.org's read-only APIs (github.com/larsi-org/html) for a station's channel
// list and latest readings, and shows them on the CYD's built-in 2.8" ILI9341 screen. No touch,
// no audio -- just a display client.
//
// Works against either the weather or sensors section -- both speak the same API shape
// (json/sensors.php?prefix=X for channel metadata, csv/current.php?prefix=X for latest values
// as channel,value,epoch rows), so which one this device shows is purely a matter of which
// server URL it's configured with at setup time, not a compile-time choice. (This started as
// two separate near-identical sketches, WeatherStation and SensorsStation -- merged once it was
// clear every change had to be made twice, identically, for no functional reason.) See this
// repo's README for why the wire parameter stayed `prefix` rather than `station`.
//
// Wi-Fi, station prefix, and server URL are all set at runtime via a captive setup portal
// (CydPortal.h) rather than compiled in -- on first boot, or whenever none of the up-to-3
// saved networks connect, this opens an access point ("CYD-Station-Setup-xxxxxx") with a
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
const unsigned long REFRESH_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 minutes
unsigned long lastRefresh = 0;
bool firstRefresh = true;

// A station's channel layout barely ever changes, so metadata (channel list, labels, units)
// is only re-fetched occasionally, on a much longer cycle than live readings.
const unsigned long METADATA_INTERVAL_MS = 60UL * 60UL * 1000UL;  // 1 hour
unsigned long lastMetadataFetch = 0;
bool haveMetadata = false;

const int MAX_SENSORS = 64;  // weather stations report exactly 7 (channels 0-6); sensors
                              // stations can have up to 256, though nowhere near that many in
                              // practice today -- generous since pagination (below) means a
                              // station no longer has to fit on one screen.

// Sensors that don't fit on one page cycle automatically -- header stays put, only the rows
// below it change. ROWS_PER_PAGE is derived from the portrait screen: (320 tall - 34 header -
// 16 footer) / 25 per row.
const int ROWS_PER_PAGE = 10;
const unsigned long PAGE_INTERVAL_MS = 10UL * 1000UL;  // 10 seconds per page
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

// --- Graphs --------------------------------------------------------------------------------
// One extra page, after the paginated value-list pages above, showing up to 3 mini history
// graphs (config.graphChannels), stacked one per row. Sensors and weather stations log at very
// different rates (~12/hr vs ~1-2/hr -- see csv/data.php's shared t_min/t_max/sensors
// protocol), so the lookback window is picked per section to land a similar number of samples
// across GRAPH_COLUMNS either way.
const int NUM_GRAPHS = 3;
const int GRAPH_COLUMNS = 48;
const unsigned long SENSORS_GRAPH_WINDOW_S = 6UL * 3600UL;    // ~72 samples at 12/hr
const unsigned long WEATHER_GRAPH_WINDOW_S = 48UL * 3600UL;   // ~48-96 samples at 1-2/hr

// One time-bucketed column of a mini graph. sum/count give the column's average; vmin/vmax (only
// meaningful once count > 0) drive the min-max ribbon for columns dense enough to have more than
// one sample -- see drawGraphs().
struct GraphColumn {
  bool has = false;
  int count = 0;
  float sum = 0;
  float vmin = 0;
  float vmax = 0;
};

GraphColumn graphColumns[NUM_GRAPHS][GRAPH_COLUMNS];
bool graphHasData[NUM_GRAPHS];
float graphGlobalMin[NUM_GRAPHS];
float graphGlobalMax[NUM_GRAPHS];
unsigned long graphTMin = 0;
unsigned long graphWindowSeconds = 0;
bool graphSkippedHeader = false;  // onGraphDataLine() state -- csv/data.php's first line is a
                                   // header ("t,s,v"), unlike csv/current.php's onCurrentLine.

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

  bool haveWifi = loadCydConfig(config);
  if (!haveWifi || !connectToKnownNetwork(15000)) {
    drawStatus("Starting setup portal...");
    runCydSetupPortal();  // never returns -- restarts the device once the form is saved
  }

  // Station/server aren't set up here at all -- that happens on the always-on config page
  // below, once the device is actually on the network and can fetch a real station list. A
  // brand-new device reaches this point with Wi-Fi working but no station chosen yet; loop()
  // shows a "go configure" screen instead of trying to fetch with an empty apiHost.
  if (config.hasStation() && !parseServerUrl(config.baseUrl(), apiHost, apiBasePath)) {
    // Shouldn't happen -- the config page validates this same URL shape before saving. Leave
    // apiHost/apiBasePath empty rather than guessing a server this device wasn't told to use;
    // loop() treats that the same as "no station configured yet".
    Serial.println("Saved server URL is invalid -- treating as unconfigured");
  }

  // Config page stays reachable at this device's normal LAN IP for as long as it's running --
  // not just during the AP-mode portal above -- so the station/server can be set (or changed)
  // without ever having to get the device back into AP mode.
  startCydConfigServer();

  syncTime();
}

void loop() {
  handleCydConfigServer();
  if (configServerSaved()) {
    drawStatus("Saved. Rebooting...");
    delay(1000);
    ESP.restart();
  }

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

  if (!config.hasStation() || apiHost.length() == 0) {
    drawStatus("Visit http://" + WiFi.localIP().toString() + "/ to pick a station");
    delay(2000);
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
    fetchGraphData();
    currentPage = 0;
    lastPageFlip = now;
    drawCurrentPage();
  } else if (haveMetadata && totalPages() > 1 && now - lastPageFlip >= PAGE_INTERVAL_MS) {
    currentPage = (currentPage + 1) % totalPages();
    lastPageFlip = now;
    drawCurrentPage();
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
               "User-Agent: cyd-larsi-org-station\r\n" +
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
               "User-Agent: cyd-larsi-org-station\r\n" +
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

// Bins one csv/data.php row (epoch,channel,value) into whichever graph slot(s) are configured
// for that channel -- same channel can appear in more than one slot, in which case it's binned
// into each. Rows outside [graphTMin, graphTMin + graphWindowSeconds) shouldn't happen (that's
// exactly the t_min/t_max fetchGraphData() requested) but are skipped rather than trusted, since
// a slightly stale device clock could shift the window after the request was already sent.
void onGraphDataLine(const String &line) {
  if (!graphSkippedHeader) {  // csv/data.php's first line is always the "t,s,v" header row
    graphSkippedHeader = true;
    return;
  }

  int c1 = line.indexOf(',');
  int c2 = line.indexOf(',', c1 + 1);
  if (c1 < 0 || c2 < 0) return;

  unsigned long epoch = strtoul(line.substring(0, c1).c_str(), nullptr, 10);
  int channel = line.substring(c1 + 1, c2).toInt();
  float value = line.substring(c2 + 1).toFloat();
  if (epoch < graphTMin || epoch - graphTMin >= graphWindowSeconds) return;

  int col = (int)((epoch - graphTMin) * GRAPH_COLUMNS / graphWindowSeconds);
  col = constrain(col, 0, GRAPH_COLUMNS - 1);

  for (int i = 0; i < NUM_GRAPHS; i++) {
    if (config.graphChannels[i] != channel) continue;
    GraphColumn &gc = graphColumns[i][col];
    if (!gc.has) {
      gc.has = true;
      gc.vmin = gc.vmax = value;
    } else {
      gc.vmin = min(gc.vmin, value);
      gc.vmax = max(gc.vmax, value);
    }
    gc.sum += value;
    gc.count++;
  }
}

void fetchGraphData() {
  for (int i = 0; i < NUM_GRAPHS; i++)
    for (int c = 0; c < GRAPH_COLUMNS; c++) graphColumns[i][c] = GraphColumn();

  graphWindowSeconds = (config.section == "sensors") ? SENSORS_GRAPH_WINDOW_S : WEATHER_GRAPH_WINDOW_S;
  unsigned long tMax = (unsigned long)time(nullptr);
  graphTMin = tMax > graphWindowSeconds ? tMax - graphWindowSeconds : 0;

  String sensorsParam = String(config.graphChannels[0]) + "," + String(config.graphChannels[1]) +
                         "," + String(config.graphChannels[2]);
  String path = apiBasePath + "csv/data.php?prefix=" + config.stationPrefix +
                "&sensors=" + sensorsParam + "&t_min=" + String(graphTMin) + "&t_max=" + String(tMax);
  graphSkippedHeader = false;
  httpsGetLines(path, onGraphDataLine);

  for (int i = 0; i < NUM_GRAPHS; i++) {
    bool any = false;
    float mn = 0, mx = 0;
    for (int c = 0; c < GRAPH_COLUMNS; c++) {
      if (!graphColumns[i][c].has) continue;
      if (!any) {
        mn = graphColumns[i][c].vmin;
        mx = graphColumns[i][c].vmax;
        any = true;
      } else {
        mn = min(mn, graphColumns[i][c].vmin);
        mx = max(mx, graphColumns[i][c].vmax);
      }
    }
    graphHasData[i] = any;
    graphGlobalMin[i] = mn;
    graphGlobalMax[i] = mx;
  }
}

int valuePages() {
  if (sensorCount == 0) return 1;
  return (sensorCount + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
}

// Value-list pages, plus one extra graph page at the end.
int totalPages() {
  return valuePages() + 1;
}

// --- Display -----------------------------------------------------------------------------

// Theme colors -- config.inverseDisplay swaps the default dark background for white, with a
// separate set of darker text colors chosen for contrast/legibility against white rather than
// just reusing the dark-mode palette (the default YELLOW/LIGHTGREY read fine on black but are
// nearly invisible on white).
uint16_t colorBg() { return config.inverseDisplay ? ILI9341_WHITE : ILI9341_BLACK; }
uint16_t colorHeader() { return config.inverseDisplay ? tft.color565(0, 70, 140) : ILI9341_CYAN; }
uint16_t colorMuted() {
  return config.inverseDisplay ? tft.color565(140, 140, 140) : ILI9341_DARKGREY;
}
uint16_t colorLabel() {
  return config.inverseDisplay ? tft.color565(60, 60, 60) : ILI9341_LIGHTGREY;
}
uint16_t colorValue() {
  return config.inverseDisplay ? tft.color565(180, 120, 0) : ILI9341_YELLOW;
}
uint16_t colorError() { return ILI9341_RED; }  // reads fine on both backgrounds as-is
uint16_t colorStatusText() {
  return config.inverseDisplay ? tft.color565(40, 40, 40) : ILI9341_WHITE;
}

void drawStatus(const String &message) {
  tft.fillScreen(colorBg());
  tft.setTextColor(colorStatusText());
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
  tft.fillScreen(colorBg());

  tft.setTextColor(colorHeader());
  tft.setTextSize(2);
  tft.setCursor(10, 6);
  tft.println(config.stationPrefix);

  if (totalPages() > 1) {
    tft.setTextSize(1);
    tft.setTextColor(colorMuted());
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
    tft.setTextColor(colorLabel());
    tft.print(sensors[i].property);

    tft.setCursor(165, y);
    if (readings[i].valid) {
      tft.setTextColor(colorMuted());
      tft.print(formatAge(readings[i].epoch));
    }

    tft.setTextSize(2);
    tft.setCursor(10, y + 9);
    if (readings[i].valid) {
      tft.setTextColor(colorValue());
      tft.print(String(readings[i].value, 1) + " " + sensors[i].unit);
    } else {
      tft.setTextColor(colorError());
      tft.print("no data");
    }

    y += rowHeight;
  }

  tft.setTextSize(1);
  tft.setTextColor(colorMuted());
  tft.setCursor(10, 305);
  tft.print("Config: " + WiFi.localIP().toString());
}

// Stack of mini history graphs (config.graphChannels), one full-width row per channel, one extra
// page after the value-list pages above -- see drawCurrentPage(). Each row scales its own y-axis
// from that channel's own min/max over the fetched window (fetchGraphData()) rather than any
// fixed range, since a given slot could hold anything from a temperature to a wind speed.
void drawGraphs() {
  tft.fillScreen(colorBg());

  tft.setTextColor(colorHeader());
  tft.setTextSize(2);
  tft.setCursor(10, 6);
  tft.println(config.stationPrefix);

  if (totalPages() > 1) {
    tft.setTextSize(1);
    tft.setTextColor(colorMuted());
    tft.setCursor(180, 12);
    tft.print(String(currentPage + 1) + "/" + String(totalPages()));
  }

  const int areaTop = 34;
  const int areaBottom = 302;
  const int areaLeft = 6;
  const int areaRight = 234;
  const int gap = 6;
  const int cellWidth = areaRight - areaLeft;
  const int cellHeight = (areaBottom - areaTop - (NUM_GRAPHS - 1) * gap) / NUM_GRAPHS;
  const int labelHeight = 20;

  for (int i = 0; i < NUM_GRAPHS; i++) {
    int cellX = areaLeft;
    int cellY = areaTop + i * (cellHeight + gap);

    int channel = config.graphChannels[i];
    int idx = findSensorIndex(channel);
    String property = idx >= 0 ? sensors[idx].property : ("Ch " + String(channel));
    String unit = idx >= 0 ? sensors[idx].unit : "";

    tft.setTextSize(1);
    tft.setTextColor(colorLabel());
    tft.setCursor(cellX, cellY);
    tft.print(property);

    tft.setCursor(cellX, cellY + 10);
    if (graphHasData[i]) {
      tft.setTextColor(colorMuted());
      tft.print(String(graphGlobalMin[i], 1) + "-" + String(graphGlobalMax[i], 1) + " " + unit);
    } else {
      tft.setTextColor(colorError());
      tft.print("no data");
      continue;
    }

    int plotX = cellX;
    int plotY = cellY + labelHeight;
    int plotW = cellWidth;
    int plotH = cellHeight - labelHeight;

    float vmin = graphGlobalMin[i];
    float vmax = graphGlobalMax[i];
    if (vmax <= vmin) vmax = vmin + 1;  // flat data -- avoid a divide-by-zero scale

    bool haveLast = false;
    float lastLo = 0, lastHi = 0;

    for (int c = 0; c < GRAPH_COLUMNS; c++) {
      GraphColumn &gc = graphColumns[i][c];
      float lo, hi;
      if (gc.has) {
        lo = gc.vmin;
        hi = gc.vmax;
        haveLast = true;
        lastLo = lo;
        lastHi = hi;
      } else if (haveLast) {
        // No sample landed in this column -- expected for weather's ~1-2/hr rate against a
        // window sized for it. Carry the last known value forward rather than leaving a gap,
        // same as any sparse-telemetry line chart would.
        lo = lastLo;
        hi = lastHi;
      } else {
        continue;  // no data yet at all this far into the window
      }

      int x = plotX + c * plotW / GRAPH_COLUMNS;
      int yLo = plotY + plotH - 1 - (int)((lo - vmin) / (vmax - vmin) * (plotH - 1));
      int yHi = plotY + plotH - 1 - (int)((hi - vmin) / (vmax - vmin) * (plotH - 1));
      tft.drawFastVLine(x, yHi, yLo - yHi + 1, colorValue());
    }
  }
}

// Dispatches to whichever page currentPage actually refers to -- the value-list pages (0 ..
// valuePages()-1) or the one graph page always appended after them.
void drawCurrentPage() {
  if (currentPage >= valuePages()) {
    drawGraphs();
  } else {
    drawSensors();
  }
}
