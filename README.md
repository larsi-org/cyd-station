# cyd-larsi-org

Arduino sketches turning a [Cheap Yellow Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)
(`ESP32-2432S028R`) into a dedicated status display for [larsi.org](https://larsi.org)'s own
APIs -- no third-party weather service, no cloud dashboard, just the CYD's screen polling the
same site that already tracks this data. Background and hardware notes are on the site itself,
at [larsi.org/make/cyd/](https://larsi.org/make/cyd/).

## Sketch

**Station** -- polls one of [larsi.org/weather/](https://larsi.org/weather/) or
[larsi.org/sensors/](https://larsi.org/sensors/)'s API for one station's channel list and latest
readings, and shows them on the display, auto-cycling through pages if there are more than fit
on screen at once. A read-only API client -- no credentials required beyond WiFi, since both
APIs serve public data.

Both sections speak the same API shape (channel list + latest values), so this one sketch works
against either -- which one a given device shows is just a matter of which server URL it's
configured with, not a compile-time choice. (Used to be two separate, near-identical sketches,
WeatherStation and SensorsStation -- merged once every change had to be made twice, identically,
for no functional reason.)

## Setup

No secrets to configure at build time -- setup happens in two stages, both over a plain web
form, no serial monitor needed:

1. **Wi-Fi.** On first boot -- or whenever none of the up to 3 saved networks connect -- the
   device opens its own access point (`CYD-Station-Setup-xxxxxx`) with a captive portal: pick a
   network, enter its password, save. Nothing else is asked here.
2. **Station.** Once connected, the same config form stays reachable at the device's normal LAN
   IP (shown on screen as `Config: <ip>`) for as long as it's running -- no need to get the
   device back into AP mode to change anything later. Set a Server Root (defaults to
   `https://larsi.org/`) and a Section (Weather or Sensors), hit Refresh Stations to fetch that
   section's real station list from the server, then pick one -- no free-text station entry.

Both stages save independently and restart the device; settings persist in NVS across reboots
and firmware updates.

## Hardware

`ESP32-2432S028R`, either the single-USB or 2-USB-port variant. 2.8" ILI9341 TFT (no touch used
here), driven on the CYD's actual wiring -- SPI clock/data on GPIO 14/13/12, not the ESP32's
default VSPI pins. No external parts needed.

## Building

1. Arduino IDE (or `arduino-cli`) with the `esp32` board package installed, board set to
   **ESP32 Dev Module**.
2. Install libraries via Library Manager: `Adafruit GFX Library`, `Adafruit ILI9341`,
   `ArduinoJson`.
3. Compile and flash as usual, then configure Wi-Fi/station/server through the captive portal --
   see Setup above.

Verifies larsi.org's TLS certificate against a small curated CA bundle (`CertBundle.h`) rather
than skipping verification -- see that file's header comment for where it comes from.

## License

MIT -- see [LICENSE](LICENSE).
