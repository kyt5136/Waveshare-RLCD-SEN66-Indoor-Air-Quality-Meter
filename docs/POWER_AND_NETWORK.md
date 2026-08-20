# Power, Battery, and Network Control

## Operating modes

The firmware has an external-power mode, a low-power mode, and a temporary
Recovery Tool high-power override.

The board does not route USB VBUS to an ESP32 input. Software cannot detect all USB power sources directly.

When the selected Arduino USB configuration exposes `Serial.isPlugged()`, the
firmware uses that debounced host-connection state as the external-power
signal. Battery voltage is not used to infer USB power because a full cell is
ambiguous. Boards/configurations that do not expose this signal remain in
low-power mode.

Recovery Tool does not claim that external power is present. While its one-hour
sequence is active, it explicitly suppresses the low-power sensor schedule,
display sleep, Wi-Fi shutdown, and ESP32 light sleep. Normal automatic power
behavior resumes after completion, cancellation, failure, or restart.

## Battery measurement

The board schematic shows a 200 kΩ upper resistor and a 100 kΩ lower resistor. This divider has a ratio of 3.000.

The firmware uses this equation:

```text
Vbattery = analogReadMilliVolts(GPIO4) × 3.000 × BATTERY_CALIBRATION / 1000
```

The firmware averages 32 calibrated ADC samples. It removes the highest and lowest samples before it calculates the result.

The prior equation also used a 1.079 multiplier. That multiplier could show 4.4 V from a safe cell voltage.

`BATTERY_CALIBRATION` has a default value of `1.000`. Change this value only after a digital multimeter comparison.

Use this calibration procedure:

1. Disconnect USB power.
2. Let the cell rest for five minutes.
3. Measure the cell at the battery terminals.
4. Record the displayed battery voltage.
5. Divide the multimeter value by the displayed value.
6. Set `BATTERY_CALIBRATION` to the result.
7. Compile and upload the firmware.
8. Repeat the comparison.

The normal pages show two decimal places. The System Information page shows three decimal places.

## State-of-charge estimate

The firmware maps battery voltage to an 18650 discharge curve. It smooths the result to reduce load-step errors.

This result is an estimate. Cell age, temperature, load, and wiring resistance change the result.

The board has no current monitor or coulomb counter. The firmware cannot measure current consumption from GPIO 4.

The web interface estimates the time to 20 percent from the state-of-charge trend. It needs at least two hours of discharge data.

The estimate shows `Learning` during charge, stable voltage, or insufficient history.

Use a fuel gauge for a controlled runtime result. A MAX17048-class gauge gives a better estimate than voltage alone.

Use a current monitor for load measurements. An INA219-class monitor can measure current, but it adds shunt loss and idle current.

## SEN66 duty cycle

External-power mode keeps the SEN66 in continuous measurement.

Low-power mode uses this schedule:

| Local time | SEN66 state |
|---|---|
| First 90 seconds of each 10-minute slot | Measurement on |
| Remaining time in each 10-minute slot | Measurement off |
| Minute 50 through minute 59 | Measurement on |
| Top of the hour | Read and display current data |

The firmware uses the SEN66 stop-measurement command. It does not remove sensor power.

Sensirion states that stop and start commands retain the VOC state while the sensor has power.

The firmware also saves the eight-byte VOC algorithm state in NVS. It limits flash writes to one save per hour.

The SEN66 interface does not provide a NOx state export command. The firmware cannot save a separate NOx state.

The forced CO2 calibration overrides the duty cycle. The SEN66 stays active during the five-minute qualification period.

Recovery Tool keeps the SEN66 active for 30 minutes before its one-time FRC and
for 30 minutes after measurement restarts. It does not retry FRC.

## Wi-Fi schedule

External-power mode keeps Wi-Fi available and uses a ten-minute online refresh
interval.

Low-power mode disables Wi-Fi between update windows. The normal update interval is 30 minutes.

Recovery Tool keeps Wi-Fi available after the online start request so its web
status remains reachable when the access point is in range. Loss of the browser,
client laptop, or Wi-Fi connection does not stop the in-memory recovery sequence.

The firmware uses three OpenWeather calls per update: one-minute timeline,
15-minute timeline, and outdoor air data.

The current revision also requests the 15-minute One Call timeline for outside
temperature, precipitation probability, and alert identifiers. A new rain or
changed alert can show the 60-minute page once when that page is enabled. It
does not replace official severe-weather alerting.

If rain occurs in the next 30 minutes, the interval changes to 10 minutes. This mode lasts for two hours.

The same rain event cannot start another two-hour period. A dry forecast arms the next rain event.

Continuous 10-minute operation uses 432 OpenWeather calls per day. The firmware stops OpenWeather calls at 900 calls per day.

WeatherAPI remains the source for the three-day forecast, astronomy data, pressure, and location time data.

## Button control

GPIO 18 is the next-page button.

GPIO 0 is the previous-page button. GPIO 0 is also the ESP32 boot strap pin.

Hold GPIO 0 for one second to start a five-minute Wi-Fi window. A short press changes to the previous page.

Either button can wake the ESP32-S3 from light sleep. The firmware uses GPIO wake for both digital inputs.

Do not hold GPIO 0 during reset. The ESP32-S3 can enter its download mode.

## Display and CPU sleep

The firmware limits normal battery-mode display writes to 0.5 Hz. USB power or
any button press enables a 60-second interactive window with up to 4 Hz updates.

The display schedule sends the ST7305 sleep command after the firmware blanks the display. The wake path sends the ST7305 wake command.

Low-power mode uses 200 ms light-sleep intervals between work. Wi-Fi, alarms, timers, and CO2 calibration prevent light sleep.

The reflective display has no backlight. The ESP32-S3 and SEN66 remain the primary battery loads.

## Network setup portal

The firmware starts a setup access point after a failed boot connection. The access point remains active for ten minutes.

Use these network values:

```text
SSID: SEN66-Setup
Password: sen66-setup
```

The setup page accepts a Wi-Fi SSID, a Wi-Fi password, latitude, and longitude. It stores these values in NVS.

The page can request the phone location. Most browsers require HTTPS for geolocation.

The ESP32 setup page uses HTTP. Browser location can fail even after the user grants permission.

Enter latitude and longitude manually if browser location fails.

The setup access point does not provide internet access. Use it only for local commissioning.
