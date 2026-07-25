# Change Log

## Outdoor CO2 calibration workflow - 2026-07-25

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
