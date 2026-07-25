# Build, Commissioning, and Maintenance

## 1. Reference toolchain

The 2026-07-24 reference build used:

| Tool or library | Version |
|---|---:|
| Arduino-ESP32 | 3.3.11 |
| Adafruit GFX | 1.12.6 |
| Sensirion I2C SEN66 | 1.3.1 |
| Sensirion Core | 0.7.3 |
| Soldered PCF85063A RTC | 1.0.0 |

Arduino CLI validation target:

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/weather_dash
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
SEN66 started
WiFi connected
RTC synced
Outdoor weather loaded
```

Exact wording may vary with state. Do not proceed directly to enclosure closure if the SEN66 probe, serial-number read, or continuous-measurement command fails.

### Functional acceptance

Verify:

1. SEN66 page reports PM1.0, PM2.5, PM4.0, PM10, RH, temperature, VOC, NOx, and CO2.
2. Main page temperature is Fahrenheit.
3. Outdoor wind is mph.
4. Hourly forecast begins with the next full hour and contains six entries across midnight.
5. Sunrise and sunset correspond to the configured WeatherAPI location.
6. RTC survives a normal reset.
7. Web location validation rejects out-of-range coordinates.
8. Page-mask changes survive reset.
9. Navigation audio and alarm audio operate independently.
10. `secrets.h` remains untracked.

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
traceable 400 ppm calibration gas; use a controlled reference for metrological
work.

## 4. Common faults

### SEN66 does not respond at `0x6B`

- inspect SDA/SCL reversal;
- verify supply and ground;
- confirm the exact connector pinout;
- scan the bus;
- check combined pull-up resistance;
- reduce cable length;
- confirm GPIO 13/14 are not reassigned.

### Sensor frame returns invalid VOC, NOx, or CO2

- allow startup/conditioning time;
- confirm continuous measurement was started;
- check Sensirion error codes on Serial;
- verify the installed SEN66 library matches the documented version;
- avoid repeated sensor reset loops.

### Compile fails at `ESP_I2S.h`

Install Arduino-ESP32 3.x. The application is not source-compatible with the old 2.x audio API.

### Runtime fails near display initialization

Confirm PSRAM is enabled and detected. The RLCD driver asserts if its PSRAM allocations fail.

### Weather location changes but displayed time zone does not

This is expected. Weather location and POSIX time zone are independent. Modify `posixTZ` and rebuild for a different local time zone.

### Web interface unavailable

- read the assigned IP address from Serial or System Info;
- confirm client and device are on the same permitted subnet;
- confirm AP client isolation is not enabled;
- confirm port 80 is not blocked;
- verify Wi-Fi connection state.

### Outdoor calibration will not start or never completes

- verify Wi-Fi is connected and WeatherAPI returns `pressure_mb`;
- confirm the active weather location matches the physical site;
- inspect Serial Monitor for `SEN66 FRC` and I2C error messages;
- keep the CO2 reading continuously between 350 and 450 ppm for five minutes;
- move people and combustion sources away from the sensor;
- do not expect the device to infer outdoor placement from its measurements.

## 5. Update discipline

Before upgrading a dependency:

1. record the current working version;
2. read the dependency changelog;
3. compile from a clean checkout;
4. run the full functional acceptance sequence;
5. compare binary size against the selected application partition;
6. verify NVS backward compatibility;
7. verify WeatherAPI field parsing against a captured response;
8. update `ATTRIBUTION.md` if the dependency or license changed.

## 6. Data-quality maintenance

- Keep the SEN66 airflow path clean and unobstructed.
- Avoid aerosol sprays, solvents, and direct cooking plumes during normal baseline operation.
- Compare temperature/RH against a known reference after enclosure assembly.
- Do not calibrate battery state-of-charge from the displayed voltage alone.
- Treat step changes in VOC/NOx indices as events requiring context, not automatic identification of a specific gas.
- Retain regulatory or reference-grade instruments for any safety-critical decision.
