# ESP-IDF Migration Plan

## Decision

VS Code is an editor. It does not replace Arduino or ESP-IDF.

Use VS Code with the Espressif extension for an ESP-IDF port. This combination gives direct access to ESP-IDF power management.

Keep the Arduino version as the hardware reference. Port one service at a time.

## Reasons for an ESP-IDF port

ESP-IDF provides direct control of FreeRTOS tasks, power domains, Wi-Fi events, and sleep locks.

It also provides component tests and controlled partition builds. These features help a long-life battery product.

Arduino already supports the requested light sleep and GPIO wake behavior. An immediate port is not required for these functions.

## Proposed component layout

```text
main/
  app_main.c
components/
  board_waveshare_rlcd/
  display_st7305/
  sen66_service/
  battery_service/
  weather_service/
  time_service/
  network_service/
  web_service/
  storage_service/
  alarm_service/
  ui_service/
```

Each component must own one hardware or application function.

`board_waveshare_rlcd` must define all pins and power controls. Other components must not use numeric GPIO values.

`sen66_service` must own the measurement state machine. It must also own VOC state save and restore.

`battery_service` must own ADC calibration, state-of-charge estimation, and power-mode hysteresis.

`weather_service` must own WeatherAPI and OpenWeather requests. It must also own the daily call limit.

`network_service` must own station mode, access-point mode, and the five-minute Wi-Fi window.

`ui_service` must receive data through immutable snapshots. It must not call a network or sensor driver.

## Task model

Use one low-frequency task for the UI. Give the task a two-second period.

Use one sensor task for the SEN66 schedule. Block the task on a timer or event group.

Use one network task for scheduled data requests. Stop Wi-Fi after each low-power update.

Use the ESP-IDF HTTP server for the local interface. Do not run the server while Wi-Fi is off.

Use an event group for these states:

```text
WIFI_REQUIRED
WIFI_CONNECTED
SENSOR_REQUIRED
DISPLAY_REQUIRED
ALARM_ACTIVE
EXTERNAL_POWER
```

The power manager can enter light sleep when no state blocks sleep.

## Storage

Use NVS for settings, credentials, and the eight-byte VOC state.

Use FATFS for long sensor histories. Buffer records in RAM and write one batch per hour.

Use a version and CRC in each binary record. Reject a record with an unknown version or invalid CRC.

Do not store the OpenWeather key in source control. Use a local configuration header during development.

For production, use an encrypted NVS partition when the threat model requires key protection.

## Migration sequence

1. Create an ESP-IDF project with the 16 MB partition table.
2. Port the board pin definitions.
3. Port the ST7305 driver.
4. Verify one static display page.
5. Port the PCF85063 RTC service.
6. Port the SEN66 driver and serial output.
7. Port VOC state save and restore.
8. Port battery ADC calibration.
9. Port GPIO 0 and GPIO 18 wake.
10. Port WeatherAPI and OpenWeather requests.
11. Port the Wi-Fi window and setup access point.
12. Port the web interface.
13. Port alarms and audio.
14. Port each LCD page.
15. Compare Arduino and ESP-IDF sensor results.
16. Measure current in each power state.

Do not port all pages before the sensor and power services pass their tests.

## Acceptance tests

The ESP-IDF version must pass these tests:

1. Read every SEN66 output at address `0x6B`.
2. Restore the VOC state after a reset.
3. Keep NOx behavior within the documented sensor limits.
4. Read battery voltage within the calibrated tolerance.
5. Wake from GPIO 0.
6. Wake from GPIO 18.
7. Update the display at 0.5 Hz or less.
8. Stop Wi-Fi between low-power update windows.
9. Keep OpenWeather calls below the configured daily limit.
10. Start the setup access point after a failed Wi-Fi connection.
11. Preserve timers and alarms across the migration.
12. Match Fahrenheit and mph output on all user pages.

## Measurement requirement

Measure current at the battery terminals. USB current does not show the complete charger and battery path.

Record these states:

| State | Required measurement |
|---|---|
| External power, Wi-Fi active | Average and peak current |
| Battery, Wi-Fi active | Average and peak current |
| Battery, SEN66 active | Average current |
| Battery, SEN66 idle | Average current |
| Battery, ESP32-S3 light sleep | Average current |
| Display scheduled sleep | Average current |

Use the results to set the final duty cycle. Do not use component data-sheet values as the finished product result.
