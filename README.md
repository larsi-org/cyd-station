# cyd-larsi-org

Arduino sketches turning a [Cheap Yellow Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)
(`ESP32-2432S028R`) into a dedicated status display for [larsi.org](https://larsi.org)'s own
APIs -- no third-party weather service, no cloud dashboard, just the CYD's screen polling the
same site that already tracks this data. Background and hardware notes are on the site itself,
at [larsi.org/make/cyd/](https://larsi.org/make/cyd/).

## Sketches

- **WeatherStation** -- polls [larsi.org/weather/](https://larsi.org/weather/)'s API for one
  station's latest observed conditions (temperature, dew point, humidity, pressure, wind) and
  shows them on the display. Any of the ~300 tracked stations works, not just airports near you.
- **SensorsStation** -- polls [larsi.org/sensors/](https://larsi.org/sensors/)'s API for one
  station's channel list and latest readings (up to 8 channels fit the screen).

Both are read-only API clients -- no credentials required beyond WiFi, since both APIs serve
public data.

## Hardware

`ESP32-2432S028R`, either the single-USB or 2-USB-port variant. 2.8" ILI9341 TFT (no touch used
here), driven on the CYD's actual wiring -- SPI clock/data on GPIO 14/13/12, not the ESP32's
default VSPI pins. No external parts needed.

## Building

1. Arduino IDE (or `arduino-cli`) with the `esp32` board package installed, board set to
   **ESP32 Dev Module**.
2. Install libraries via Library Manager: `Adafruit GFX Library`, `Adafruit ILI9341`,
   `ArduinoJson` (SensorsStation only).
3. In each sketch's folder, copy `secrets.h.example` to `secrets.h` and fill in your WiFi
   credentials and the station prefix you want to display.
4. Compile and flash as usual.

Both sketches verify larsi.org's TLS certificate against a small curated CA bundle
(`CertBundle.h` in each sketch folder) rather than skipping verification -- see that file's
header comment for where it comes from.

## License

MIT -- see [LICENSE](LICENSE).
