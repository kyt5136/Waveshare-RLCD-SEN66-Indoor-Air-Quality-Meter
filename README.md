# Waveshare-RLCD SEN66 Indoor Air Quality Meter

Firmware and operating documentation for a desktop indoor-air-quality station built on the Waveshare ESP32-S3-RLCD-4.2 and a Sensirion SEN66 connected to the board's external I2C bus.

The SEN66 is the authoritative source for indoor temperature, relative humidity, carbon dioxide, VOC Index, NOx Index, and particulate-matter measurements. WeatherAPI supplies outdoor conditions, forecasts, sunrise and sunset, and outdoor PM2.5 data. The 400 x 300 reflective LCD presents thirteen local pages; an embedded HTTP interface provides configuration, page selection, timer/alarm control, and live data inspection.

## Engineering status

- Target hardware: Waveshare ESP32-S3-RLCD-4.2
- Application framework: Arduino-ESP32
- Reference build date: 2026-07-24
- Reference core: Espressif Arduino-ESP32 3.3.11
- Reference binary size: 1,288,475 bytes
- Reference dynamic allocation: 50,088 bytes of global data
- Display units: degrees Fahrenheit and miles per hour
- Primary indoor sensor: Sensirion SEN66 at I2C address `0x6B`

The reference installation uses the board's 16 MB flash configuration with a 3 MB application partition and approximately 9.9 MB FATFS partition. This is flash allocation, not RAM allocation. Waveshare specifies 8 MB PSRAM and 16 MB flash for the board.

## Functional scope

### Indoor measurements

The firmware reads the complete processed SEN66 measurement frame:

- PM1.0
- PM2.5
- PM4.0
- PM10
- compensated relative humidity
- compensated temperature
- VOC Index
- NOx Index
- CO2

Indoor particle AQI is calculated locally from the current PM2.5 and PM10 readings. It is a short-term instrument indication, not an official regulatory AQI report. See [Data processing and AQI](docs/DATA_PROCESSING.md).

### LCD pages

| Page | Display |
|---:|---|
| 0 | Indoor Air: temperature, humidity, CO2, VOC, indoor AQI, outdoor AQI, sunrise, sunset, SEN66 state, battery, and Wi-Fi |
| 1 | Analog Clock |
| 2 | North American Time Zones |
| 3 | Outdoor Conditions |
| 4 | Next Six Forecast Hours |
| 5 | Three-Day Forecast |
| 6 | Northern Hemisphere Seasons |
| 7 | Season Orbit |
| 8 | Temperature History |
| 9 | Humidity History |
| 10 | System Information |
| 11 | Complete SEN66 Output |
| 12 | Timers, Stopwatch, and Alarms |

The web interface stores a page-inclusion mask in NVS. Unchecked pages are skipped by both hardware-button navigation and automatic cycling. A page can still be selected directly from the web interface for inspection.

### Web interface

The ESP32 serves an unauthenticated HTTP interface on its LAN address. Available controls include:

- immediate WeatherAPI refresh
- immediate NTP synchronization
- weather location replacement using decimal `latitude,longitude`
- automatic LCD page cycling and dwell time
- per-page navigation inclusion
- page-change audio enable/disable
- alarm and timer audio enable/disable
- direct LCD page selection
- current-display screenshot as a one-bit BMP
- countdown timer, stopwatch, and three persistent alarms

The interface is intended for a trusted local network. It does not implement TLS, user authentication, or authorization. See [Security](SECURITY.md).

## Hardware connection

The SEN66 is connected to the board's external I2C header:

| Signal | ESP32-S3 pin | SEN66 connection |
|---|---:|---|
| SDA | GPIO 13 | SDA |
| SCL | GPIO 14 | SCL |
| Supply | Per SEN66 module requirements | VDD |
| Ground | GND | GND |

Do not infer supply voltage from the I2C logic level. Confirm the exact SEN66 carrier/module power-input requirements before energizing the assembly. The firmware assumes the physical connection has already been verified.

Additional board assignments used by the firmware are documented in [Hardware integration](docs/HARDWARE.md).

## Software prerequisites

Install the following through Arduino IDE Board Manager and Library Manager:

| Component | Reference version | Purpose |
|---|---:|---|
| Espressif ESP32 Arduino core | 3.3.11 | ESP32-S3 runtime, Wi-Fi, HTTP, NVS, I2S, and ESP-IDF display interfaces |
| Adafruit GFX Library | 1.12.6 | One-bit canvas and graphics primitives |
| Sensirion I2C SEN66 | 1.3.1 | SEN66 command and measurement driver |
| Sensirion Core | 0.7.3 | Sensirion I2C framing, CRC, and error support |
| Soldered PCF85063A RTC Arduino Library | 1.0.0 | PCF85063A RTC access |

The audio implementation uses the Arduino-ESP32 3.x `ESP_I2S` API and will not compile unchanged against older 2.x cores.

## Configuration

1. Copy `firmware/weather_dash/secrets.example.h` to `firmware/weather_dash/secrets.h`.
2. Replace every placeholder.
3. Do not commit `secrets.h`.

```cpp
#pragma once

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* weatherApiKey = "YOUR_WEATHERAPI_KEY";
const char* weatherLocation = "YOUR_CITY_OR_LAT_LON";
```

`weatherLocation` is the first-boot default. A valid location entered through the web interface is stored in the ESP32 `Preferences` namespace and takes precedence on subsequent boots. The credentials file is never rewritten by the firmware.

## Build and upload

1. Open `firmware/weather_dash/weather_dash.ino` in Arduino IDE.
2. Select an ESP32-S3 target compatible with the Waveshare board.
3. Select 16 MB flash.
4. Select the partition layout with a 3 MB application partition and approximately 9.9 MB FATFS.
5. Enable the board's PSRAM option appropriate to the installed ESP32-S3 module.
6. Compile before connecting the battery-powered installation.
7. Upload over USB and open Serial Monitor at 115200 baud.

The display driver allocates its frame buffer and lookup tables in PSRAM. A build can compile successfully yet fail at runtime if PSRAM is unavailable or configured incorrectly.

## Runtime intervals

| Operation | Interval |
|---|---:|
| SEN66 measurement read | 1 second |
| Battery ADC read | 10 seconds |
| Temperature/humidity history sample | 15 minutes |
| History capacity | 24 samples / 6 hours |
| WeatherAPI refresh | 30 minutes |
| NTP synchronization | 24 hours |
| LCD auto-cycle dwell | 3 to 300 seconds, configurable |

## Documentation index

- [System architecture](docs/ARCHITECTURE.md)
- [Hardware integration](docs/HARDWARE.md)
- [Data processing and AQI](docs/DATA_PROCESSING.md)
- [Web interface and persistence](docs/WEB_INTERFACE.md)
- [Build, commissioning, and maintenance](docs/OPERATIONS.md)
- [Source provenance and third-party attribution](ATTRIBUTION.md)
- [Security model](SECURITY.md)

## Provenance and licensing

This repository is a renamed GitHub fork of [JohnWillieGee/Waveshare-RLCD-ESP32S3-Weather-dashboard](https://github.com/JohnWillieGee/Waveshare-RLCD-ESP32S3-Weather-dashboard), which supplied the immediate dashboard and display-driver baseline. That repository, at audited commit `01dfda32398427422586dd58fd0441e5796cb510`, does not contain the `LICENSE` file referenced by its README and GitHub reports no detected license.

An earlier acknowledged source, [juanjocastillo/Waveshare-RLCD-ESP32S3-Dashboard-v12_1](https://github.com/juanjocastillo/Waveshare-RLCD-ESP32S3-Dashboard-v12_1), is MIT-licensed and is the apparent source of the low-level RLCD driver and several font assets.

No project-wide open-source license is asserted here because the immediate upstream contribution has no explicit license grant. Third-party components retain their own licenses. Refer to [ATTRIBUTION.md](ATTRIBUTION.md) before copying, redistributing, or relicensing any portion of this repository.

## Safety and interpretation

- This is not a calibrated regulatory monitor.
- Current PM-derived AQI is not equivalent to the prescribed 24-hour AQI reporting process.
- VOC Index and NOx Index are dimensionless processed indicators, not gas concentrations.
- CO2, VOC, and NOx outputs require sensor startup and conditioning time.
- Battery voltage thresholds are approximate and are not a fuel-gauge algorithm.
- The web interface must not be exposed directly to the public internet.
