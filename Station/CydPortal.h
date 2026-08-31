#pragma once

#include <Arduino.h>

// Blocks forever: brings up an open Wi-Fi access point plus a captive config portal (network
// picker, password, station prefix, base URL), saves whatever the user submits, then restarts
// the device. Trimmed down from sensor-node's SensorNodePortal.h/.cpp -- same network-picker/
// NVS/DNS-hijack mechanics, minus everything specific to sensor-node's write path.
void runCydSetupPortal();
