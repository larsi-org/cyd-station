// CydPortal.h
// MIT License
// https://opensource.org/licenses/MIT
// Copyright (c) 2026, Lars Schumann, larsi.org@gmail.com
//
#pragma once

#include <Arduino.h>

// Blocks forever: brings up an open Wi-Fi access point plus a captive config portal (network
// picker + password only -- station/server setup happens separately, see below), saves
// whatever's submitted, then restarts the device. Trimmed down from sensor-node's
// SensorNodePortal.h/.cpp -- same network-picker/NVS/DNS-hijack mechanics, minus everything
// specific to sensor-node's write path.
void runCydSetupPortal();

// The station config page (server root, section, station prefix -- picked from a live-fetched
// list, not typed in), reachable at the device's normal LAN IP once it's on the real network --
// no DNS hijack, no blocking. Call once after a successful connect; call
// handleCydConfigServer() every loop() tick afterward to actually service requests, and check
// configServerSaved() to know when to restart the device.
void startCydConfigServer();
void handleCydConfigServer();
bool configServerSaved();
