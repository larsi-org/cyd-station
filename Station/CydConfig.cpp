#include "CydConfig.h"

#include <Preferences.h>
#include <WiFi.h>

namespace {
const char *kNamespace = "cydstation";
}

bool loadCydConfig(CydConfig &config) {
  Preferences prefs;
  prefs.begin(kNamespace, true);
  for (uint8_t i = 0; i < CydConfig::kMaxNetworks; i++) {
    config.ssids[i] = prefs.getString(("ssid" + String(i)).c_str(), "");
    config.passwords[i] = prefs.getString(("password" + String(i)).c_str(), "");
  }
  config.stationPrefix = prefs.getString("stationPrefix", "");
  config.serverRoot = prefs.getString("serverRoot", "");
  config.section = prefs.getString("section", "");
  config.inverseDisplay = prefs.getBool("inverseDisplay", false);
  prefs.end();
  return config.hasWifi();
}

void saveCydConfig(const CydConfig &config) {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  for (uint8_t i = 0; i < CydConfig::kMaxNetworks; i++) {
    prefs.putString(("ssid" + String(i)).c_str(), config.ssids[i]);
    prefs.putString(("password" + String(i)).c_str(), config.passwords[i]);
  }
  prefs.putString("stationPrefix", config.stationPrefix);
  prefs.putString("serverRoot", config.serverRoot);
  prefs.putString("section", config.section);
  prefs.putBool("inverseDisplay", config.inverseDisplay);
  prefs.end();
}

void clearCydConfig() {
  Preferences prefs;
  prefs.begin(kNamespace, false);
  prefs.clear();
  prefs.end();
}

bool parseServerUrl(const String &url, String &host, String &basePath) {
  String rest = url;
  rest.trim();
  int schemeEnd = rest.indexOf("://");
  if (schemeEnd >= 0) rest = rest.substring(schemeEnd + 3);

  int pathStart = rest.indexOf('/');
  String h = pathStart >= 0 ? rest.substring(0, pathStart) : rest;
  String p = pathStart >= 0 ? rest.substring(pathStart) : "/";
  if (h.length() == 0) return false;
  if (!p.endsWith("/")) p += "/";

  host = h;
  basePath = p;
  return true;
}

String macSuffix() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return mac.substring(mac.length() - 6);
}
