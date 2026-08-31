#pragma once

// Trimmed down from sensor-node's SensorNodeConfig.h (~/Arduino/libraries/sensor-node) -- same
// idea (up to kMaxNetworks known Wi-Fi networks, persisted to NVS, most-recently-added first),
// minus everything specific to sensor-node's write path (device name/ID, API key, log
// interval). Adds stationPrefix, which sensor-node has no equivalent of.

#include <Arduino.h>

struct CydConfig {
  static const uint8_t kMaxNetworks = 3;
  String ssids[kMaxNetworks];
  String passwords[kMaxNetworks];

  // Sent as the API's `prefix` query param -- see https://larsi.org/weather/ for the station
  // list (ICAO codes, e.g. "KABQ").
  String stationPrefix;

  // Base URL API calls are built against -- json/sensors.php and csv/current.php are appended
  // onto it directly (see parseServerUrl()). The scheme is accepted but ignored: this always
  // connects over TLS on port 443 regardless of what's typed here.
  String baseUrl;

  bool isComplete() const { return ssids[0].length() > 0 && stationPrefix.length() > 0; }
};

// Reads saved settings from NVS. Returns config.isComplete().
bool loadCydConfig(CydConfig &config);

void saveCydConfig(const CydConfig &config);

// Erases saved settings so the next loadCydConfig() call reports incomplete, sending setup()
// back to the portal.
void clearCydConfig();

// Splits a URL like "https://larsi.org/weather/" into a bare hostname ("larsi.org") and a base
// path guaranteed to start and end with "/" ("/weather/"). Returns false (leaving host/basePath
// untouched) if no host is present at all, e.g. an empty or scheme-only URL.
bool parseServerUrl(const String &url, String &host, String &basePath);

// Last 6 hex digits of WiFi.macAddress() (the per-device-unique tail, not the shared vendor
// OUI) -- used to build a unique setup-AP name.
String macSuffix();
