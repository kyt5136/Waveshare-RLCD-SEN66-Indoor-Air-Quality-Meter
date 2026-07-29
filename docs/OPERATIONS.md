# Build, Commissioning, and Maintenance

## 1. Reference toolchain

The 2026-07-29 reference build used:

| Tool or library | Version |
|---|---:|
| Arduino-ESP32 | 3.3.11 |
| Adafruit GFX | 1.12.6 |
| ArduinoJson | 7.4.3 |
| Sensirion I2C SEN66 | 1.3.1 |
| Sensirion Core | 0.7.3 |
| Soldered PCF85063A RTC | 1.0.0 |

Arduino CLI validation target:

```powershell
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc" firmware/weather_dash
```

The generic FQBN validation checks compilation. Deployment still requires the Waveshare-appropriate flash, PSRAM, USB, and partition selections in Arduino IDE.

## 2. Clean checkout preparation

1. Install Arduino IDE and the Espressif ESP32 board package.
2. Install every listed library through Library Manager.
3. Copy `secrets.example.h` to `secrets.h`.
4. Populate credentials locally.
5. Confirm `git status` never lists `secrets.h`.
6. Select the 16 MB flash / 3 MB application partition configuration.
7. Compile.

Add `OpenWeatherMapApiKey` to `secrets.h`. Use decimal coordinates for `weatherLocation`.

## 3. Commissioning sequence

### Pre-power inspection

- Confirm SEN66 power polarity.
- Confirm SDA is connected to GPIO 13 and SCL to GPIO 14.
- Confirm common ground.
- Confirm no enclosure obstruction at the SEN66 airflow path.
- Confirm the 18650 cell is the correct chemistry, polarity, and condition for the board.
- Confirm the display is not being used as a mechanical support.

### First boot

Observe Serial Monitor at 115200 baud. Expected milestones:

```text
ESP32-S3 SEN66 WEATHER STATION
[I2C] Startup scan on SDA=GPIO13, SCL=GPIO14
[I2C] 0x18 ES8311 audio codec - EXPECTED
[I2C] 0x40 ES7210 microphone ADC - EXPECTED
[I2C] 0x51 PCF85063A RTC - EXPECTED
[I2C] 0x6B SEN66 external air-quality sensor - EXPECTED
[I2C] 0x70 SHTC3 onboard temperature/humidity sensor - EXPECTED
[I2C] Scan complete: 5 responder(s), 0 unknown
SEN66 started
WiFi connected
Outdoor weather loaded
Location clock synced
```

Exact wording may vary with state. Do not proceed directly to enclosure closure if the SEN66 probe, serial-number read, or continuous-measurement command fails.

Investigate each `MISSING`, `UNKNOWN`, or `BUS ERROR` I2C line.
An unknown address can indicate an added module or an address conflict.

If the Wi-Fi connection fails, join `SEN66-Setup` from a phone. Use `sen66-setup` as the password.

### Functional acceptance

Verify:

1. SEN66 page reports PM1.0, PM2.5, PM4.0, PM10, RH, temperature, VOC, NOx, and CO2.
2. Main page temperature is Fahrenheit.
3. Outdoor wind uses mph.
4. Hourly forecast starts with the next full hour.
5. The 60-minute page shows the next outside temperature and precipitation data.
6. The page shows inside temperature, humidity, CO2, the clock, and battery charge.
7. Sunrise and sunset correspond to the configured WeatherAPI location.
8. RTC survives a normal reset.
9. The displayed IANA timezone and UTC offset match the configured coordinates.
10. Changing latitude/longitude through the web interface updates weather,
   timezone, RTC date, and local clock without rebuilding.
11. Web location validation rejects out-of-range coordinates.
12. Page-mask changes survive reset.
13. Navigation audio and alarm audio operate independently.
14. GPIO 18 selects the next page.
15. A short GPIO 0 press selects the previous page.
16. A one-second GPIO 0 hold starts a five-minute Wi-Fi window.
17. Either button selects ST7305 high-power mode for 60 seconds.
18. The display returns to 0.5 Hz low-power mode after the interaction window.
19. Serial sensor lines show battery voltage with three decimal places.
20. `/history.csv` restores the last six hours after a reset.
21. The System Information page shows the SEN66-to-SHTC3 mean deltas.
22. `secrets.h` remains untracked.

### Forced CO2 calibration

Use this procedure only when there is a justified need to replace the stored
CO2 correction:

1. Place the complete powered SEN66 assembly outdoors in open, well-mixed air.
2. Keep it away from people, vehicles, combustion exhaust, doors, windows,
   HVAC outlets, and direct wind gusts.
3. Confirm that the configured WeatherAPI latitude/longitude describes the
   sensor location.
4. Open the root web page and select the outdoor-placement confirmation.
5. Press **Start 5-Min Calibration**.
6. Confirm that the displayed pressure was freshly downloaded and accepted.
7. Leave the unit undisturbed. The timer advances only while reported CO2
   remains in the 350-450 ppm reference band and restarts after any excursion.
8. Wait for the success result and recorded correction. Do not remove power
   while the state is `calibrating`.
9. Return the unit indoors only after continuous measurement has restarted.

The firmware issues an FRC target of 400 ppm. This changes persistent sensor
configuration and survives reset or power loss. Ordinary outdoor air is not a
traceable 400 ppm calibration gas. use a controlled reference for metrological
work.

## 4. Common faults

### SEN66 does not respond at `0x6B`

- inspect SDA/SCL reversal.
- verify supply and ground.
- confirm the exact connector pinout.
- scan the bus.
- check combined pull-up resistance.
- reduce cable length.
- confirm GPIO 13/14 are not reassigned.

### Sensor frame returns invalid VOC, NOx, or CO2

- allow startup/conditioning time.
- confirm continuous measurement was started.
- check Sensirion error codes on Serial.
- verify the installed SEN66 library matches the documented version.
- avoid repeated sensor reset loops.

### Compile fails at `ESP_I2S.h`

Install Arduino-ESP32 3.x. The application is not source-compatible with the old 2.x audio API.

### Runtime fails near display initialization

Confirm PSRAM is enabled and detected. The RLCD driver asserts if its PSRAM allocations fail.

### Weather location changes but displayed timezone does not

- confirm the location update produced a successful WeatherAPI response.
- confirm `tz_id`, `localtime_epoch`, and `localtime` are present in the response.
- inspect Serial Monitor for the `[TIME]` resolution line.
- confirm the device can reach NTP after the WeatherAPI response.
- reload the web page after the weather request completes.

The prior timezone remains active when a new location cannot be resolved.

### Web interface unavailable

- Hold GPIO 0 for one second.
- Read the assigned IP address from Serial or System Information.
- Confirm that the phone and device use the same subnet.
- Confirm that access-point client isolation is off.
- Confirm that port 80 is open.

Low-power mode closes Wi-Fi after five minutes. This behavior is intentional.

### Outdoor calibration will not start or never completes

- verify Wi-Fi is connected and WeatherAPI returns `pressure_mb`.
- confirm the active weather location matches the physical site.
- inspect Serial Monitor for `SEN66 FRC` and I2C error messages.
- keep the CO2 reading continuously between 350 and 450 ppm for five minutes.
- move people and combustion sources away from the sensor.
- do not expect the device to infer outdoor placement from its measurements.

## 5. Update discipline

Before upgrading a dependency:

1. record the current working version.
2. read the dependency changelog.
3. compile from a clean checkout.
4. run the full functional acceptance sequence.
5. compare binary size against the selected application partition.
6. verify NVS backward compatibility.
7. verify WeatherAPI field parsing against a captured response.
8. update `ATTRIBUTION.md` if the dependency or license changed.

## 6. Data-quality maintenance

- Keep the SEN66 airflow path clean and unobstructed.
- Avoid aerosol sprays, solvents, and direct cooking plumes during normal baseline operation.
- Compare temperature/RH against a known reference after enclosure assembly.
- Do not calibrate battery state-of-charge from the displayed voltage alone.
- Treat step changes in VOC/NOx indices as events requiring context, not automatic identification of a specific gas.
- Retain regulatory or reference-grade instruments for any safety-critical decision.
