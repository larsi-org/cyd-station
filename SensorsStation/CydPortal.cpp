#include "CydPortal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include <algorithm>
#include <utility>
#include <vector>

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

// Adds a new network at the front of config's known-network list (most-recently-added first),
// shifting the rest down and dropping the oldest if full. If ssid already matches an existing
// slot, just updates that slot's password in place. A blank incoming password on an
// already-known SSID leaves the saved one alone (the password field is never pre-filled), so
// resubmitting the form just to change the station prefix doesn't silently wipe a working
// Wi-Fi password.
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

String buildFormPage() {
  // Pre-fill station prefix/base URL from the existing config -- this portal only runs
  // because none of the known networks worked (moved to a new location, most likely), so the
  // common case is just adding one new network without retyping everything else.
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
  page += "<title>CYD Sensors Station Setup</title>";
  page += "<style>body{font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em}";
  page += "label{display:block;margin-top:1em;font-weight:bold}";
  page += "input,select{width:100%;padding:.4em;box-sizing:border-box;font-size:1em}";
  page += "button{margin-top:1.5em;padding:.6em 1.2em;font-size:1em}</style></head><body>";
  page += "<h1>CYD Sensors Station Setup</h1>";
  if (existing.ssids[0].length() > 0) {
    page += "<p>Station and server are pre-filled from the existing setup -- pick a new Wi-Fi "
            "network below. Up to " +
            String(CydConfig::kMaxNetworks) +
            " networks are remembered (oldest is replaced), so moving back later should "
            "reconnect automatically.</p>";
  }
  page += "<form method=\"POST\" action=\"/save\">";
  page += "<label>Wi-Fi Network</label><select name=\"ssid\">" + options + "</select>";
  page += "<label>Wi-Fi Password</label><input type=\"password\" name=\"password\" "
          "placeholder=\"Leave blank to keep the saved password for a known network\">";
  page += "<label>Station Prefix</label><input type=\"text\" name=\"stationPrefix\" "
          "maxlength=\"32\" required value=\"" +
          htmlEscape(existing.stationPrefix) +
          "\" placeholder=\"e.g. batcave -- see larsi.org/sensors/\">";
  page += "<label>Server URL</label><input type=\"url\" name=\"baseUrl\" maxlength=\"96\" "
          "required value=\"" +
          htmlEscape(existing.baseUrl.length() > 0 ? existing.baseUrl
                                                    : "https://larsi.org/sensors/") +
          "\">";
  page += "<button type=\"submit\">Save &amp; Reboot</button>";
  page += "</form></body></html>";
  return page;
}

void handleRoot() { server.send(200, "text/html", buildFormPage()); }

void handleSave() {
  // Start from the existing config, not a blank one, so the other known-network slots survive.
  CydConfig config;
  loadCydConfig(config);

  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");
  config.stationPrefix = server.arg("stationPrefix");
  config.stationPrefix.trim();
  config.baseUrl = server.arg("baseUrl");
  config.baseUrl.trim();

  if (newSsid.length() == 0) {
    server.send(400, "text/html", "<p>Wi-Fi network is required. <a href=\"/\">Back</a></p>");
    return;
  }
  if (config.stationPrefix.length() == 0) {
    server.send(400, "text/html", "<p>Station Prefix is required. <a href=\"/\">Back</a></p>");
    return;
  }
  String unusedHost, unusedBasePath;  // just validating shape here -- CydConfig does the real parse
  if (!parseServerUrl(config.baseUrl, unusedHost, unusedBasePath)) {
    server.send(400, "text/html", "<p>Server URL must include a hostname. <a href=\"/\">Back</a></p>");
    return;
  }

  addOrUpdateNetwork(config, newSsid, newPassword);

  saveCydConfig(config);
  server.send(200, "text/html", "<p>Saved. Rebooting...</p>");
  saved = true;
}

// Any unrecognized path bounces back to the form -- this is what makes phones/laptops auto-pop
// the captive portal page on connect.
void handleNotFound() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

}  // namespace

void runCydSetupPortal() {
  WiFi.mode(WIFI_AP_STA);
  delay(100);  // the MAC isn't reliably readable immediately after WiFi.mode() -- hit as all
               // zeros on a true first-ever boot (no prior STA activity to have warmed up the
               // radio already, unlike the "known networks all failed" path into this portal)

  String apName = "CYD-Sensors-Setup-" + macSuffix();
  WiFi.softAP(apName.c_str());
  IPAddress apIP = WiFi.softAPIP();

  Serial.printf("[CydPortal] Setup portal: join Wi-Fi \"%s\", then visit http://%s/\n",
                apName.c_str(), apIP.toString().c_str());

  dnsServer.start(kDnsPort, "*", apIP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
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
