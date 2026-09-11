// CydPortal.cpp
// MIT License
// https://opensource.org/licenses/MIT
// Copyright (c) 2026, Lars Schumann, larsi.org@gmail.com
//
#include "CydPortal.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "CertBundle.h"
#include "CydConfig.h"

namespace {

const byte kDnsPort = 53;
DNSServer dnsServer;
WebServer server(80);
bool saved = false;

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

// Any unrecognized path bounces back to the form -- during the AP-mode portal, this is what
// makes phones/laptops auto-pop the captive portal page on connect; on the LAN-mode config
// server it's just a friendly catch-all.
void handleNotFound() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// --- AP-mode Wi-Fi-only portal ---------------------------------------------------------------
// Just network + password -- station/server setup happens separately, on the always-on config
// page below, once the device is actually on the network and can fetch a real station list.

// Adds a new network at the front of config's known-network list (most-recently-added first),
// shifting the rest down and dropping the oldest if full. If ssid already matches an existing
// slot, just updates that slot's password in place. A blank incoming password on an
// already-known SSID leaves the saved one alone (the password field is never pre-filled), so
// resubmitting the form just to change networks doesn't silently wipe another slot's password.
void addOrUpdateNetwork(CydConfig &config, const String &ssid, const String &password) {
  for (uint8_t i = 0; i < CydConfig::kMaxNetworks; i++) {
    if (config.ssids[i] == ssid) {
      if (password.length() > 0) config.passwords[i] = password;
      return;
    }
  }
  for (uint8_t i = CydConfig::kMaxNetworks - 1; i > 0; i--) {
    config.ssids[i] = config.ssids[i - 1];
    config.passwords[i] = config.passwords[i - 1];
  }
  config.ssids[0] = ssid;
  config.passwords[0] = password;
}

// Scanned networks, strongest signal first, de-duplicated by SSID.
std::vector<String> scanNetworkNames() {
  int count = WiFi.scanNetworks();
  std::vector<std::pair<int32_t, String>> found;
  for (int i = 0; i < count; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool duplicate = false;
    for (auto &entry : found) {
      if (entry.second == ssid) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) found.push_back({WiFi.RSSI(i), ssid});
  }
  std::sort(found.begin(), found.end(),
            [](const std::pair<int32_t, String> &a, const std::pair<int32_t, String> &b) {
              return a.first > b.first;
            });

  std::vector<String> names;
  names.reserve(found.size());
  for (auto &entry : found) names.push_back(entry.second);
  return names;
}

String buildWifiFormPage() {
  CydConfig existing;
  loadCydConfig(existing);

  std::vector<String> networks = scanNetworkNames();

  String options;
  if (networks.empty()) {
    options = "<option value=\"\">No networks found -- move closer and reset</option>";
  } else {
    for (auto &ssid : networks) {
      String escaped = htmlEscape(ssid);
      options += "<option value=\"" + escaped + "\">" + escaped + "</option>";
    }
  }

  String page;
  page += "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  page += "<title>CYD Wi-Fi Setup</title>";
  page += "<style>body{font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em}";
  page += "label{display:block;margin-top:1em;font-weight:bold}";
  page += "input,select{width:100%;padding:.4em;box-sizing:border-box;font-size:1em}";
  page += "button{margin-top:1.5em;padding:.6em 1.2em;font-size:1em}</style></head><body>";
  page += "<h1>CYD Wi-Fi Setup</h1>";
  if (existing.ssids[0].length() > 0) {
    page += "<p>Pick a new Wi-Fi network below. Up to " + String(CydConfig::kMaxNetworks) +
            " networks are remembered (oldest is replaced), so moving back later should "
            "reconnect automatically. Station and server settings aren't touched here -- once "
            "connected, visit the device's own IP to change those.</p>";
  }
  page += "<form method=\"POST\" action=\"/save\">";
  page += "<label>Wi-Fi Network</label><select name=\"ssid\">" + options + "</select>";
  page += "<label>Wi-Fi Password</label><input type=\"password\" name=\"password\" "
          "placeholder=\"Leave blank to keep the saved password for a known network\">";
  page += "<button type=\"submit\">Save &amp; Reboot</button>";
  page += "</form></body></html>";
  return page;
}

void handleWifiRoot() { server.send(200, "text/html", buildWifiFormPage()); }

void handleWifiSave() {
  // Start from the existing config, not a blank one, so station/server settings (set up
  // separately, on the always-on config page) survive.
  CydConfig config;
  loadCydConfig(config);

  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");

  if (newSsid.length() == 0) {
    server.send(400, "text/html", "<p>Wi-Fi network is required. <a href=\"/\">Back</a></p>");
    return;
  }

  addOrUpdateNetwork(config, newSsid, newPassword);

  saveCydConfig(config);
  server.send(200, "text/html", "<p>Saved. Rebooting...</p>");
  saved = true;
}

// --- Station config page (always-on, reachable at the device's LAN IP) ----------------------
// Server root, section (weather/sensors), and station -- the station list is fetched live from
// whichever section is currently selected, rather than typed in freehand.

// Minimal quote-aware two-field CSV splitter -- csv/locations.php's station labels (city
// names) can contain commas, which the server quotes; only the first two columns (label,
// prefix) are needed here, so this doesn't bother with the rest of the row.
bool splitLocationLine(const String &line, String &label, String &prefix) {
  int i = 0;
  int len = line.length();
  if (i < len && line[i] == '"') {
    i++;
    while (i < len) {
      if (line[i] == '"') {
        if (i + 1 < len && line[i + 1] == '"') {
          label += '"';
          i += 2;
        } else {
          i++;
          break;
        }
      } else {
        label += line[i++];
      }
    }
  } else {
    while (i < len && line[i] != ',') label += line[i++];
  }
  if (i >= len || line[i] != ',') return false;
  i++;
  while (i < len && line[i] != ',') prefix += line[i++];
  return prefix.length() > 0;
}

// Live {label, prefix} pairs for one section, fetched from that section's csv/locations.php.
// Returns an empty list on any connect/parse failure -- the form just shows "no stations found"
// rather than crashing; nothing here is critical-path for the device's normal operation.
std::vector<std::pair<String, String>> fetchStationList(const String &serverRoot,
                                                          const String &section) {
  std::vector<std::pair<String, String>> stations;

  String host, basePath;
  if (!parseServerUrl(serverRoot + section + "/", host, basePath)) return stations;

  WiFiClientSecure client;
  client.setCACertBundle(kServerCertBundle, kServerCertBundleLen);
  if (!client.connect(host.c_str(), 443)) return stations;

  client.print(String("GET ") + basePath + "csv/locations.php HTTP/1.1\r\n" + "Host: " + host +
               "\r\n" + "User-Agent: cyd-station\r\n" + "Connection: close\r\n\r\n");

  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }

  bool first = true;
  while (client.connected() || client.available()) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;
      if (first) {
        first = false;  // header row
        continue;
      }
      String label, prefix;
      if (splitLocationLine(line, label, prefix)) stations.push_back({label, prefix});
    }
  }
  client.stop();
  return stations;
}

// Live {channel, label} pairs for one station, fetched from that station's json/sensors.php --
// same endpoint and response shape cyd-station.ino's fetchStationMetadata() already parses, just
// duplicated here since the portal's fetch needs to run against whatever station is currently
// selected in the form, before that station is even saved. Returns an empty list on any
// connect/parse failure or if stationPrefix is blank, same "just show nothing found" fallback as
// fetchStationList() above.
std::vector<std::pair<int, String>> fetchChannelList(const String &serverRoot,
                                                       const String &section,
                                                       const String &stationPrefix) {
  std::vector<std::pair<int, String>> channels;
  if (stationPrefix.length() == 0) return channels;

  String host, basePath;
  if (!parseServerUrl(serverRoot + section + "/", host, basePath)) return channels;

  WiFiClientSecure client;
  client.setCACertBundle(kServerCertBundle, kServerCertBundleLen);
  if (!client.connect(host.c_str(), 443)) return channels;

  String path = basePath + "json/sensors.php?prefix=" + stationPrefix;
  client.print(String("GET ") + path + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" +
               "User-Agent: cyd-station\r\n" + "Connection: close\r\n\r\n");

  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, client);
  client.stop();
  if (err) return channels;

  JsonArray arr = doc["sensors"];
  for (JsonObject s : arr) {
    int ch = s["value"] | -1;
    if (ch < 0) continue;
    String property = String((const char *)(s["property"] | ""));
    String unit = String((const char *)(s["unit"] | ""));
    String label = property.length() > 0 ? property : ("Channel " + String(ch));
    if (unit.length() > 0) label += " (" + unit + ")";
    channels.push_back({ch, label});
  }
  return channels;
}

// One <select> per graph slot, all built from the same fetched channel list -- only which
// option is "selected" differs (config.graphChannels[slot]).
String buildChannelSelect(const String &name, const std::vector<std::pair<int, String>> &channels,
                           int selected) {
  String options;
  if (channels.empty()) {
    options = "<option value=\"\">No channels found -- pick a station and Refresh Stations "
              "first</option>";
  } else {
    for (auto &c : channels) {
      bool isSelected = c.first == selected;
      options += "<option value=\"" + String(c.first) + "\"" + (isSelected ? " selected" : "") +
                 ">" + htmlEscape(c.second) + "</option>";
    }
  }
  return "<select name=\"" + name + "\">" + options + "</select>";
}

String buildStationFormPage() {
  CydConfig existing;
  loadCydConfig(existing);

  // GET args (present after the "Refresh Stations" button reloads the page) win over the saved
  // config, so switching sections/server without saving still reflects in the fetched list.
  String serverRoot = server.hasArg("serverRoot") ? server.arg("serverRoot")
                       : existing.serverRoot.length() > 0
                           ? existing.serverRoot
                           : "https://larsi.org/";
  if (!serverRoot.endsWith("/")) serverRoot += "/";
  String section = server.hasArg("section")   ? server.arg("section")
                    : existing.section.length() > 0 ? existing.section
                                                     : "weather";
  String displayMode = server.hasArg("displayMode") ? server.arg("displayMode")
                        : existing.displayMode.length() > 0 ? existing.displayMode
                                                             : "text";

  // Checkbox args only appear at all when checked -- server.hasArg("section") tells apart a
  // "Refresh Stations" GET reload (where an unchecked box should read as false) from the very
  // first page load (no args at all, where an unchecked box should just mean "use the saved
  // value").
  bool cameFromReload = server.hasArg("section");
  bool inverseDisplay = cameFromReload ? server.hasArg("inverseDisplay") : existing.inverseDisplay;

  // The station <select>'s own current value rides along on the "Refresh Stations" GET reload
  // like any other form field, so the channel dropdowns can reflect whichever station is
  // currently chosen in the form -- not just the last-saved one.
  String stationPrefixForChannels =
      server.hasArg("stationPrefix") ? server.arg("stationPrefix") : existing.stationPrefix;
  std::vector<std::pair<int, String>> channels =
      fetchChannelList(serverRoot, section, stationPrefixForChannels);

  int graphChannels[3];
  for (uint8_t i = 0; i < 3; i++) {
    String argName = "graphChannel" + String(i);
    graphChannels[i] = cameFromReload && server.hasArg(argName)
                            ? server.arg(argName).toInt()
                            : existing.graphChannels[i];
  }

  std::vector<std::pair<String, String>> stations = fetchStationList(serverRoot, section);

  String stationOptions;
  if (stations.empty()) {
    stationOptions =
        "<option value=\"\">No stations found -- check Server Root, then Refresh</option>";
  } else {
    for (auto &s : stations) {
      String escapedLabel = htmlEscape(s.first);
      String escapedPrefix = htmlEscape(s.second);
      bool isCurrent = s.second == existing.stationPrefix;
      stationOptions += "<option value=\"" + escapedPrefix + "\"" +
                         (isCurrent ? " selected" : "") + ">" + escapedLabel + " (" +
                         escapedPrefix + ")</option>";
    }
  }

  String page;
  page += "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  page += "<title>CYD Station Setup</title>";
  page += "<style>body{font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em}";
  page += "label{display:block;margin-top:1em;font-weight:bold}";
  page += "input,select{width:100%;padding:.4em;box-sizing:border-box;font-size:1em}";
  page += "label.checkbox{display:flex;align-items:center;gap:.5em;font-weight:normal}";
  page += "label.checkbox input{width:auto}";
  page += "button{margin-top:1.5em;margin-right:.5em;padding:.6em 1.2em;font-size:1em}";
  page += "</style></head><body>";
  page += "<h1>CYD Station Setup</h1>";
  page += "<form>";
  page += "<label>Server Root</label><input type=\"url\" name=\"serverRoot\" maxlength=\"64\" "
          "required value=\"" +
          htmlEscape(serverRoot) + "\">";
  page += "<label>Section</label><select name=\"section\">";
  page += String("<option value=\"weather\"") + (section == "weather" ? " selected" : "") +
          ">Weather</option>";
  page += String("<option value=\"sensors\"") + (section == "sensors" ? " selected" : "") +
          ">Sensors</option>";
  page += "</select>";
  page += "<label>Station</label><select name=\"stationPrefix\">" + stationOptions + "</select>";
  page += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"inverseDisplay\"" +
          String(inverseDisplay ? " checked" : "") + "> Inverse Display (white background)</label>";
  page += "<label>Display</label><select name=\"displayMode\">";
  page += String("<option value=\"text\"") + (displayMode == "text" ? " selected" : "") +
          ">Text Only</option>";
  page += String("<option value=\"graphs\"") + (displayMode == "graphs" ? " selected" : "") +
          ">Graphs Only</option>";
  page += String("<option value=\"both\"") + (displayMode == "both" ? " selected" : "") +
          ">Text &amp; Graphs</option>";
  page += "</select>";
  page += "<label>Graph 1</label>" + buildChannelSelect("graphChannel0", channels, graphChannels[0]);
  page += "<label>Graph 2</label>" + buildChannelSelect("graphChannel1", channels, graphChannels[1]);
  page += "<label>Graph 3</label>" + buildChannelSelect("graphChannel2", channels, graphChannels[2]);
  // "Refresh Stations" (GET, reloads with whatever Server Root/Section are currently chosen)
  // comes first in the DOM so it's what fires on Enter -- pressing Enter while editing Server
  // Root should re-fetch the list, not accidentally save before the station selection even
  // reflects the change.
  page += "<button formmethod=\"get\" formaction=\"/\">Refresh Stations</button>";
  page += "<button formmethod=\"post\" formaction=\"/save\">Save &amp; Reboot</button>";
  page += "</form></body></html>";
  return page;
}

void handleStationRoot() { server.send(200, "text/html", buildStationFormPage()); }

void handleStationSave() {
  // Start from the existing config, not a blank one, so Wi-Fi settings (set up separately, via
  // the AP-mode portal) survive.
  CydConfig config;
  loadCydConfig(config);

  String serverRoot = server.arg("serverRoot");
  serverRoot.trim();
  if (!serverRoot.endsWith("/")) serverRoot += "/";
  String section = server.arg("section");
  section.trim();
  String stationPrefix = server.arg("stationPrefix");
  stationPrefix.trim();
  String displayMode = server.arg("displayMode");
  displayMode.trim();

  if (serverRoot.length() <= 1) {
    server.send(400, "text/html", "<p>Server Root is required. <a href=\"/\">Back</a></p>");
    return;
  }
  if (section != "weather" && section != "sensors") {
    server.send(400, "text/html", "<p>Section must be Weather or Sensors. <a href=\"/\">Back</a></p>");
    return;
  }
  if (displayMode != "text" && displayMode != "graphs" && displayMode != "both") {
    server.send(400, "text/html",
                "<p>Display must be Text Only, Graphs Only, or Text &amp; Graphs. "
                "<a href=\"/\">Back</a></p>");
    return;
  }
  if (stationPrefix.length() == 0) {
    server.send(400, "text/html",
                "<p>Station is required -- fetch the list first. <a href=\"/\">Back</a></p>");
    return;
  }
  String unusedHost, unusedBasePath;  // just validating shape here -- baseUrl() does the real parse
  if (!parseServerUrl(serverRoot + section + "/", unusedHost, unusedBasePath)) {
    server.send(400, "text/html", "<p>Server Root must include a hostname. <a href=\"/\">Back</a></p>");
    return;
  }

  config.serverRoot = serverRoot;
  config.section = section;
  config.stationPrefix = stationPrefix;
  config.displayMode = displayMode;
  config.inverseDisplay = server.hasArg("inverseDisplay");  // absent entirely when unchecked
  for (uint8_t i = 0; i < 3; i++) {
    String argName = "graphChannel" + String(i);
    if (server.hasArg(argName) && server.arg(argName).length() > 0) {
      config.graphChannels[i] = server.arg(argName).toInt();
    }
  }

  saveCydConfig(config);
  server.send(200, "text/html", "<p>Saved. Rebooting...</p>");
  saved = true;
}

}  // namespace

void runCydSetupPortal() {
  WiFi.mode(WIFI_AP_STA);
  delay(100);  // the MAC isn't reliably readable immediately after WiFi.mode() -- hit as all
               // zeros on a true first-ever boot (no prior STA activity to have warmed up the
               // radio already, unlike the "known networks all failed" path into this portal)

  String apName = "CYD-Station-Setup-" + macSuffix();
  WiFi.softAP(apName.c_str());
  IPAddress apIP = WiFi.softAPIP();

  Serial.printf("[CydPortal] Setup portal: join Wi-Fi \"%s\", then visit http://%s/\n",
                apName.c_str(), apIP.toString().c_str());

  dnsServer.start(kDnsPort, "*", apIP);

  server.on("/", HTTP_GET, handleWifiRoot);
  server.on("/save", HTTP_POST, handleWifiSave);
  server.onNotFound(handleNotFound);
  server.begin();

  saved = false;
  unsigned long savedAt = 0;
  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (saved && savedAt == 0) savedAt = millis();
    if (savedAt != 0 && millis() - savedAt > 1000) ESP.restart();
  }
}

void startCydConfigServer() {
  saved = false;
  server.on("/", HTTP_GET, handleStationRoot);
  server.on("/save", HTTP_POST, handleStationSave);
  server.onNotFound(handleNotFound);
  server.begin();
}

void handleCydConfigServer() {
  server.handleClient();
}

bool configServerSaved() {
  return saved;
}
