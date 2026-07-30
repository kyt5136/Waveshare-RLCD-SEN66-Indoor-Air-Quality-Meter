# Change Log

## LCD design standard and USB power detection - 2026-07-29

- Added one reusable black header with a fixed page title and local time.
- Applied the header to all 14 LCD pages.
- Removed location coordinates and location strings from LCD headers.
- Added bold label and value fonts for better one-bit display legibility.
- Standardized the measurement-box labels, baselines, spacing, and units.
- Moved `ppm` into the CO2 labels to permit larger CO2 values.
- Added an LCD design standard for the future ESP-IDF port.
- Removed battery voltage as an automatic external-power signal.
- Added USB host connection and disconnection confirmation periods.
- Set low-power mode within two seconds after USB host removal.
- Reduced the current value size on both indoor history pages.

## Display, sensor comparison, and startup diagnostics - 2026-07-29

- Added a complete startup scan for I2C addresses `0x01` through `0x7E`.
- Added expected, missing, unknown, and bus-error classifications to the serial log.
- Added a 60-second ST7305 high-power interaction window after each button press.
- Set the ST7305 low-power frame rate to 0.5 Hz.
- Added ESP32-S3 hardware USB host detection.
- Added 15-second SHTC3 readings and paired SEN66 comparison logging.
- Added 12-hour offset qualification with one-standard-deviation update gates.
- Added corrected SHTC3 fallback data while the SEN66 is idle.
- Added persistent FATFS storage for sensor comparison data and six-hour history.
- Added three-decimal battery voltage to each serial sensor update.
- Added OpenWeather 15-minute temperature, rain probability, and alert processing.
- Added one-time automatic selection of the 60-minute page for each new weather event.
- Rebuilt the 60-minute page with four measurement boxes, a clock, and battery charge.
- Set the development-unit battery voltage correction factor to `1.020`.
- Prevented unqualified raw SHTC3 data from producing steps in the six-hour history.

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
