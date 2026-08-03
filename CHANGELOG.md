# Change Log

## Maintenance, resilience, and on-device settings - 2026-08-03

- Added startup I2C diagnostics and CRC-checked onboard SHTC3 sampling.
- Added FATFS-backed six-hour history persistence and SEN66/SHTC3 comparison logging.
- Added a 12-hour, 100-pair qualification gate before corrected SHTC3 fallback values are used during a stopped SEN66 duty window.
- Added PM finite-value rejection, a 10-second post-start/fan-clean AQI gate, and PM2.5/PM10 exponential smoothing.
- Added SEN66 fan clean-out control and a page-14 on-device settings menu.
- Added NVS page-mask schema migration for the settings page.
- Added USB-host based external-power behavior, a 60-second button-interaction display window, and 5 Hz RTC caching.
- Added OpenWeather 15-minute data and one-time navigation to the minute page for new rain or severe-weather events.
- Standardized display page headers and updated multiple page layouts.
- Added full engineering and operating documentation for this revision.

## Power and weather revision - 2026-07-29

- Replaced the battery ADC equation with calibrated millivolt sampling and the board divider ratio.
- Added two-decimal voltage output and three-decimal System Information output.
- Added a voltage-based charge estimate and a discharge trend to 20 percent.
- Added low-power SEN66 duty control and hourly VOC state storage.
- Added 30-minute OpenWeather updates and rain-based ten-minute updates.
- Added a 60-minute precipitation page.
- Added previous-page control on GPIO 0 and next-page control on GPIO 18.
- Added a five-minute Wi-Fi window after a one-second GPIO 0 hold.
- Added light sleep with wake control from both buttons.
- Added a ten-minute network setup access point after a failed startup connection.
- Added an Arduino-to-ESP-IDF migration plan.
- Applied the STE-flavored writing standard to project documentation.

## Dynamic location time and outdoor CO2 calibration - 2026-07-25

- Replaced the compiled Eastern POSIX timezone with coordinate-derived timezone
  and local-time resolution from WeatherAPI.
- Added automatic RTC, current UTC-offset, and IANA timezone updates after
  first-boot default coordinates or web location changes.
- Added NVS retention of the last resolved timezone for offline restarts.
- Added a guarded forced-CO2 recalibration control to the main web page.
- Added a mandatory fresh WeatherAPI pressure download and SEN66 ambient-pressure compensation.
- Added a five-minute continuous 350-450 ppm outdoor qualification window.
- Added automatic stop, 1500 ms idle wait, 400 ppm FRC, result validation, and measurement restart.
- Added live calibration status, cancellation before the write, Serial diagnostics, and operating documentation.

## SEN66 engineering revision - 2026-07-24

- Replaced the onboard SHTC3 data path with complete Sensirion SEN66 acquisition.
- Added indoor PM2.5 and PM10 AQI sub-index calculation using the 2024 EPA PM2.5 breakpoints.
- Separated indoor and outdoor particle AQI on the primary display.
- Added the complete SEN66 output page.
- Converted all user-facing temperature output to Fahrenheit.
- Converted all user-facing wind output to miles per hour.
- Corrected hourly forecast selection to use the next six full hours across day boundaries.
- Added runtime WeatherAPI latitude/longitude control with NVS persistence.
- Added web-based page inclusion, direct page selection, and adjustable automatic cycling.
- Added independent navigation-audio and alarm-audio controls.
- Added timer, stopwatch, three alarms, snooze behavior, and display-sleep infrastructure.
- Localized seasons and time-zone presentation for North America and the Northern Hemisphere.
- Removed space-weather, lunar, aurora, and ATS-radio subsystems from the inherited dashboard.
- Added architecture, hardware, data-processing, web-interface, operations, security, and attribution documentation.
