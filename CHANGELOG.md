# Change Log

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

