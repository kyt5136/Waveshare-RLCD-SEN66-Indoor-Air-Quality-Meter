# Hardware Integration

## 1. Target board

The target is the Waveshare ESP32-S3-RLCD-4.2. Manufacturer documentation identifies:

- ESP32-S3 dual-core MCU.
- 16 MB flash.
- 8 MB PSRAM.
- 300 x 400 fully reflective display, used here as 400 x 300 landscape.
- PCF85063 RTC.
- SHTC3 temperature and humidity sensor.
- ES7210 microphone ADC.
- ES8311 audio codec, amplifier, and speaker path.
- onboard 18650 holder and battery-management circuitry.
- external I2C and GPIO expansion.

Primary manufacturer reference: [Waveshare ESP32-S3-RLCD-4.2 documentation](https://docs.waveshare.com/ESP32-S3-RLCD-4.2).

## 2. Firmware pin assignment

| Function | GPIO | Direction / bus |
|---|---:|---|
| RLCD MOSI | 12 | SPI output |
| RLCD clock | 11 | SPI output |
| RLCD D/C | 5 | output |
| RLCD chip select | 40 | output |
| RLCD reset | 41 | output |
| External I2C SDA | 13 | bidirectional |
| External I2C SCL | 14 | output/open-drain |
| Previous page and Wi-Fi button | 0 | input with pull-up |
| Next page button | 18 | input with pull-up |
| Battery ADC | 4 | analog input |
| Audio amplifier enable | 46 | output |
| I2S BCLK | 9 | output |
| I2S LRCLK | 45 | output |
| I2S data out | 8 | output |
| I2S MCLK | 16 | output |

These assignments are board-specific. Do not transplant the sketch to a generic ESP32-S3 board without reconciling its schematic and strapping pins.

## 3. SEN66 interface

The Sensirion SEN66 uses address `0x6B`. The application performs an address probe before invoking the library driver.

| SEN66 signal | Board connection |
|---|---|
| SDA | external I2C SDA / GPIO 13 |
| SCL | external I2C SCL / GPIO 14 |
| GND | board ground |
| VDD | supply compatible with the exact SEN66 module/carrier |

Refer to the [Sensirion SEN6x datasheet](https://sensirion.com/media/documents/FAFC548D/693FBB15/PS_DS_SEN6x.pdf) for electrical limits, connector definition, airflow clearance, startup behavior, and operating environment.

### Integration requirements

- Maintain common ground between board and sensor.
- Do not add pull-ups without checking the pull-ups already present on the board and sensor carrier.
- Keep I2C wiring short and routed away from the speaker and high-current battery paths.
- Do not obstruct the SEN66 air inlet or outlet.
- Avoid locating the module directly above heat-producing regulators or the ESP32-S3 module.
- Treat temperature and humidity as locally influenced by enclosure geometry and self-heating.

## 4. Startup I2C inventory

The firmware scans addresses `0x01` through `0x7E` once during each boot.
It runs the scan after `Wire.begin(13, 14)` and before device initialization.

The scanner expects these responders:

| Address | Device | Source |
|---:|---|---|
| `0x18` | ES8311 audio codec | board schematic and audio implementation |
| `0x40` | ES7210 microphone ADC | board schematic |
| `0x51` | PCF85063A RTC | board schematic and RTC datasheet |
| `0x6B` | external SEN66 | SEN6x datasheet and project configuration |
| `0x70` | onboard SHTC3 | board schematic and SHTC3 datasheet |

The scanner wakes the SHTC3 before the scan.
This action prevents a retained sleep state from causing a false missing-device report.

The serial log identifies each expected responder.
It also identifies missing expected devices and unknown addresses.
An I2C address scan cannot prove a device identity by itself.
Use the later device-specific initialization results to confirm each device.

## 5. Display

`DisplayPort` initializes the ESP-IDF SPI panel interface at a 10 MHz pixel clock. The reflective display has no conventional backlight. The software writes one-bit black/white image data and uses PSRAM-resident lookup tables to reduce pixel-address calculation overhead.

The board display is a fragile structural element. Follow Waveshare's handling warning: do not use the screen as a force point while connecting USB or installing/removing the 18650 cell.

The ST7305 uses high-power mode during a 60-second button interaction window.
The driver returns to 0.5 Hz low-power mode after the window.
External power keeps the ST7305 in high-power mode.

## 6. RTC

The PCF85063A is accessed through the shared I2C bus. NTP supplies absolute UTC
time. WeatherAPI resolves the active coordinates to an IANA timezone,
location-local calendar time, and current UTC offset. The firmware applies that
offset before writing the RTC, which is then used for display time and alarms.

The last resolved timezone and offset are retained in NVS. A successful weather
update recalculates them, including after a web latitude/longitude change and
across DST transitions. No compiled geographic timezone selection is required.

## 7. Audio

The implemented signal path is:

```text
ESP32-S3 I2S -> ES8311 codec -> NS4150B amplifier -> onboard speaker
```

Navigation clicks and alarm/timer chimes have independent persistent enable flags. Disabling audio in the web interface does not disable the visual alarm state.

## 8. Battery measurement

The schematic uses a 200 kΩ upper resistor and a 100 kΩ lower resistor. The battery divider ratio is 3.000.

The firmware uses the calibrated Arduino ADC millivolt function. It averages 32 samples and removes two outliers.

```text
Vbattery = ADCmillivolts × 3.000 × BATTERY_CALIBRATION / 1000
```

The state-of-charge value uses a voltage curve. It is not a coulomb-counted result.

See [Power, battery, and network control](POWER_AND_NETWORK.md) for the calibration procedure.

## 9. Power behavior

The firmware uses light sleep when it does not detect a USB host.

It also stops SEN66 measurements between sample windows. Stop and start commands keep power on the sensor.

The firmware disables Wi-Fi between scheduled requests. A GPIO 0 hold starts a five-minute Wi-Fi window.

The audio amplifier enters shutdown after each sound. The display schedule uses the ST7305 sleep command.

Measure current at the battery terminals before you make a battery-life claim.
