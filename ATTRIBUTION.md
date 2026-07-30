# Source Provenance and Third-Party Attribution

## 1. Purpose

This document records the practical source lineage of the repository and the external specifications, libraries, services, and font assets used by the firmware. It is an engineering provenance record, not legal advice.

## 2. Repository lineage

### Immediate upstream

- Project: [JohnWillieGee/Waveshare-RLCD-ESP32S3-Weather-dashboard](https://github.com/JohnWillieGee/Waveshare-RLCD-ESP32S3-Weather-dashboard)
- Audited commit: [`01dfda32398427422586dd58fd0441e5796cb510`](https://github.com/JohnWillieGee/Waveshare-RLCD-ESP32S3-Weather-dashboard/commit/01dfda32398427422586dd58fd0441e5796cb510)
- Author shown by Git: JohnWillieGee
- Relationship: immediate dashboard inspiration and code baseline
- Material inherited: application organization, page-rendering patterns, WeatherAPI integration approach, RTC/NTP behavior, web-control baseline, RLCD driver copy, and embedded font assets
- License state at audit: no `LICENSE` file in the repository root. GitHub API `licenseInfo` is null

This repository was created with GitHub's fork mechanism and renamed. GitHub therefore retains the parent relationship in repository metadata.

### Earlier upstream acknowledged by the immediate source

- Project: [juanjocastillo/Waveshare-RLCD-ESP32S3-Dashboard-v12_1](https://github.com/juanjocastillo/Waveshare-RLCD-ESP32S3-Dashboard-v12_1)
- Audited commit: [`3d6916a5d9280734a9a8b04c3f69976720132a5f`](https://github.com/juanjocastillo/Waveshare-RLCD-ESP32S3-Dashboard-v12_1/commit/3d6916a5d9280734a9a8b04c3f69976720132a5f)
- Author shown by Git: Juanjo Castillo
- License: MIT
- Material traceable through the lineage: Waveshare RLCD `DisplayPort` implementation and embedded display-font assets

The MIT license text from that repository is retained in `licenses/Juanjo-Castillo-MIT.txt`.

## 3. Project-specific engineering changes

Relative to the cited weather-dashboard baseline, this project introduces or materially changes:

- replacement of the onboard SHTC3 as primary indoor source with the external Sensirion SEN66.
- acquisition of every standard processed SEN66 output.
- PM2.5/PM10 sub-index calculations and indoor particle AQI.
- separate indoor and outdoor AQI presentation.
- Fahrenheit and mph localization.
- North American time-zone and Northern Hemisphere season behavior.
- removal of astronomy, lunar, aurora, space-weather, and ATS-radio subsystems.
- complete SEN66 diagnostic page.
- timer, stopwatch, alarm, snooze, and audio behavior.
- ESP32-hosted control interface.
- NVS-backed weather location, display, audio, sleep, cycling, alarms, and page-mask settings.
- SHTC3 comparison, 12-hour offset qualification, and corrected fallback measurements.
- FATFS sensor-pair logging and persistent six-hour history.
- ST7305 high-power interaction windows and 0.5 Hz low-power operation.
- USB host detection through the ESP32-S3 hardware CDC interface.
- complete startup I2C inventory and expected-device classification.
- six-hour epoch-based forecast selection across midnight.
- page-filtered manual and automatic navigation.
- technical, operational, data-quality, and security documentation.

## 4. Hardware and protocol sources

| Source | Use |
|---|---|
| [Waveshare ESP32-S3-RLCD-4.2 documentation](https://docs.waveshare.com/ESP32-S3-RLCD-4.2) | board capabilities, resources, handling precautions, and vendor setup references |
| [Waveshare ESP32-S3-RLCD-4.2 schematic](https://files.waveshare.com/wiki/ESP32-S3-RLCD-4.2/ESP32-S3-RLCD-4.2-schematic.pdf) | GPIO assignments, I2C devices, battery divider values, and board power connections |
| [ST7305 controller datasheet](https://files.waveshare.com/wiki/common/ST_7305_V0_2.pdf) | high-power command, low-power command, frame-rate control, and sleep sequence |
| [Sensirion SHTC3 datasheet](https://sensirion.com/en/media/documents/643F9C8E/63A5A436/Datasheet_SHTC3.pdf) | address, wake, sleep, measurement command, conversion formulas, and CRC |
| [Sensirion SEN6x datasheet](https://sensirion.com/media/documents/FAFC548D/693FBB15/PS_DS_SEN6x.pdf) | SEN66 signals, I2C behavior, electrical/operating requirements, and measurement semantics |
| [WeatherAPI documentation](https://www.weatherapi.com/docs/) | forecast endpoint, response fields, astronomy, hourly data, and air-quality response |
| [OpenWeather One Call 4.0](https://openweathermap.org/api/one-call-4) | one-minute precipitation, 15-minute conditions, alerts, endpoint limits, and update guidance |
| [OpenWeather Air Pollution API](https://openweathermap.org/api/air-pollution) | outdoor PM2.5 and PM10 concentrations |
| [Arduino-ESP32 ADC API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html) | calibrated ADC millivolt conversion |
| [ESP32-S3 sleep modes](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/sleep_modes.html) | light-sleep timer wake and digital GPIO wake |
| [EPA 2024 PM AQI update](https://www.epa.gov/system/files/documents/2024-02/pm-naaqs-air-quality-index-fact-sheet.pdf) | updated PM2.5 AQI breakpoints |
| [EPA AQS AQI breakpoints](https://aqs.epa.gov/aqsweb/documents/codetables/aqi_breakpoints.html) | PM breakpoint reference |
| [NXP PCF85063A datasheet](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf) | RTC register behavior and electrical characteristics |
| [NTP Pool Project](https://www.ntppool.org/) | network time service. firmware default is `us.pool.ntp.org` |

## 5. Software dependencies

Dependencies are installed separately and are not vendored in this repository.

| Dependency | Reference version | License | Project |
|---|---:|---|---|
| Arduino-ESP32 | 3.3.11 | LGPL-2.1-or-later with component-specific terms | [espressif/arduino-esp32](https://github.com/espressif/arduino-esp32) |
| Espressif Audio Development Framework | reference source only | Apache-2.0 | [espressif/esp-adf](https://github.com/espressif/esp-adf) |
| Adafruit GFX | 1.12.6 | BSD | [adafruit/Adafruit-GFX-Library](https://github.com/adafruit/Adafruit-GFX-Library) |
| ArduinoJson | 7.4.3 | MIT | [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson) |
| Sensirion I2C SEN66 | 1.3.1 | BSD-3-Clause | [Sensirion/arduino-i2c-sen66](https://github.com/Sensirion/arduino-i2c-sen66) |
| Sensirion Core | 0.7.3 | BSD-3-Clause | [Sensirion/arduino-core](https://github.com/Sensirion/arduino-core) |
| Soldered PCF85063A RTC | 1.0.0 | GPL-3.0 | [SolderedElectronics/Soldered-PCF85063A-RTC-Module-Arduino-Library](https://github.com/SolderedElectronics/Soldered-PCF85063A-RTC-Module-Arduino-Library) |

Anyone distributing compiled firmware must evaluate and comply with the licenses of the linked libraries, particularly the GPL-3.0 terms of the RTC library.

The ES8311 codec initialization sequence in `weather_dash.ino` was developed
with reference to Espressif's ESP-ADF ES8311 driver. ESP-ADF is not linked into
the Arduino build.

## 6. Battery design reference

[Battery18650Stats](https://github.com/danilopinotti/Battery18650Stats) informed the review of voltage-based charge estimates.

That project uses an MIT license. It also requires a voltmeter-derived conversion factor.

This firmware does not include that library or copy its source. The firmware uses the Waveshare divider and the calibrated ESP32 ADC API.

## 7. Font assets

### DSEG7

`font.h` and `secfont.h` contain generated Adafruit GFX bitmap representations of DSEG7 Classic Bold.

- Designer/project: Keshikan, [DSEG font family](https://www.keshikan.net/fonts-e.html)
- License: SIL Open Font License 1.1
- Conversion tool identified in file headers: [Squix OLED Display Font Generator](https://oleddisplay.squix.ch/)
- Retained license: `licenses/SIL-OFL-1.1.txt`

The font is not original work of this repository.

## 8. License boundary

No root `LICENSE` file is provided because the immediate upstream repository contains no explicit license grant. A public GitHub repository is not automatically open source, and attribution does not create redistribution rights.

The following statements therefore apply:

- the MIT-licensed earlier upstream material remains subject to MIT.
- DSEG font material remains subject to SIL OFL 1.1.
- external libraries remain subject to their respective licenses.
- the immediate upstream author's unlicensed contributions remain under that author's copyright.
- no project-wide relicensing is claimed by this repository.

Before redistributing outside GitHub's fork mechanism or issuing binaries, obtain appropriate permission and perform a dependency-license review.
