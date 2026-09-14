// CydConfig.h
// MIT License
// https://opensource.org/licenses/MIT
// Copyright (c) 2026, Lars Schumann, larsi.org@gmail.com
//
#pragma once

// Trimmed down from sensor-node's SensorNodeConfig.h (~/Arduino/libraries/sensor-node) -- same
// idea (up to kMaxNetworks known Wi-Fi networks, persisted to NVS, most-recently-added first),
// minus everything specific to sensor-node's write path (device name/ID, API key, log
// interval). Adds stationPrefix/serverRoot/section, which sensor-node has no equivalent of.

#include <Arduino.h>

struct CydConfig {
  static const uint8_t kMaxNetworks = 3;
  String ssids[kMaxNetworks];
  String passwords[kMaxNetworks];

  // Sent as the API's `prefix` query param -- picked from a live-fetched list (see
  // CydPortal.cpp's fetchStationList()) rather than typed in, so it's always a real station.
  String stationPrefix;

  // e.g. "https://larsi.org/" -- always ends with "/". Combined with section below to form the
  // URL API calls are built against (see baseUrl()). Kept separate from section rather than
  // storing one combined URL so the config page can offer section as a plain dropdown without
  // needing to parse it back out of a stored string.
  String serverRoot;

  // "weather" or "sensors" -- which of larsi.org's two same-shaped APIs (channel list via
  // json/sensors.php, latest values via csv/current.php) this device polls.
  String section;

  // Display-only preference, unrelated to Wi-Fi/station completeness -- white background,
  // darker text, instead of the default dark background. Doesn't gate hasStation()/hasWifi().
  bool inverseDisplay = false;

  // Display-only preference: convert any channel reporting unit "C" (both sections' convention
  // for temperature/dew point) to Fahrenheit for display. Off by default -- the server always
  // reports Celsius regardless of this setting, so it's a pure render-time choice.
  bool fahrenheit = false;

  // "text", "graphs", or "both" -- which pages loop() cycles through: just the paginated
  // value-list pages, just the one graphs page, or both in sequence. Defaults to "text" for a
  // brand-new device (matches the sketch's original, graphs-less behavior); an already-set
  // value is always preserved on reload, same as section below.
  String displayMode = "text";

  // Which channels the graphs page shows, one per row -- see CydPortal.cpp's Graph 1-3
  // dropdowns for how each is picked (defaults to 0/1/2, but any channel the station reports can
  // go in any slot). Populated regardless of displayMode, so switching into "graphs"/"both"
  // later doesn't need re-picking channels.
  int graphChannels[3] = {0, 1, 2};

  bool hasWifi() const { return ssids[0].length() > 0; }

  bool hasStation() const {
    return stationPrefix.length() > 0 && serverRoot.length() > 0 && section.length() > 0;
  }

  // The full section URL API calls are built against, e.g. "https://larsi.org/weather/". The
  // scheme is accepted but ignored by parseServerUrl() -- calls always go out over TLS on port
  // 443 regardless of what's stored here.
  String baseUrl() const { return serverRoot + section + "/"; }
};

// Reads saved settings from NVS. Returns config.hasWifi() -- callers that also need station
// config to be present check config.hasStation() separately, since the two are now set up
// independently (Wi-Fi via the AP-mode portal, station via the always-on config page).
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
