# System Architecture

## 1. Scope

The firmware is a single Arduino sketch with local companion files. It deliberately avoids a dynamic UI framework. Every LCD page is rendered into an Adafruit `GFXcanvas1` buffer and then transferred to the Waveshare reflective LCD through the board-specific ESP-IDF panel interface.

The design is organized around six operating domains:

1. acquisition of indoor SEN66 measurements.
2. acquisition and reduction of WeatherAPI and OpenWeather data.
3. timekeeping using NTP and the PCF85063A RTC.
4. deterministic monochrome page rendering.
5. local control through hardware buttons and an embedded HTTP server.
6. FATFS logging and persistent sensor history.

## 2. Main software components

| Component | Responsibility |
|---|---|
| `weather_dash.ino` | Application state, sensor acquisition, API parsing, AQI calculation, page rendering, alarms, audio, NTP, NVS, and HTTP handlers |
| `display_bsp.cpp` | SPI bus setup, reflective-LCD initialization, buffer transfer, and lookup-table acceleration |
| `display_bsp.h` | `DisplayPort` declaration and display constants |
| `font.h` | DSEG7 84-point Adafruit GFX bitmap font |
| `secfont.h` | DSEG7 36-point Adafruit GFX bitmap font |
| `secrets.example.h` | Credential schema only. contains no operating credentials |

## 3. Boot sequence

The implemented startup sequence is:

1. initialize Serial at 115200 baud.
2. load persistent settings from NVS namespace `dash`.
3. detect a connected USB host.
4. initialize the shared I2C controller on GPIO 13/14.
5. scan all usable I2C addresses and classify each responder.
6. mount the FATFS partition.
7. initialize ES8311/I2S audio.
8. initialize the RLCD display interface.
9. initialize the PCF85063A RTC.
10. reset the SEN66, restore the VOC state, and start measurement.
11. connect to the configured 2.4 GHz Wi-Fi network.
12. start the embedded HTTP server.
13. retrieve WeatherAPI and OpenWeather data.
14. synchronize UTC from NTP and write location-local time to the RTC.
15. acquire the first SHTC3 and SEN66 measurements.
16. restore six-hour history and start the audio indication.

Failures are reported on Serial and reflected in display state. Wi-Fi or remote-weather failure does not prevent indoor sensing and local display operation.

## 4. Runtime scheduling

The main Arduino loop is cooperative. There is no application-created FreeRTOS task and no blocking scheduler. Work is dispatched from `millis()` deadlines:

| Deadline | Action |
|---|---|
| 1 s | SEN66 processed measurement |
| 10 s | battery ADC |
| 15 s | SHTC3 temperature and humidity |
| 15 min | temperature/humidity history insertion |
| 30 min | WeatherAPI update |
| 30 min | Normal OpenWeather update |
| 10 min | OpenWeather update during a two-hour rain event |
| 250 ms | Maximum LCD update rate during button interaction |
| 2 s | Maximum LCD update rate outside button interaction |
| 24 h | NTP/RTC synchronization |
| configurable | automatic page change |

The ESP32 networking and web-server implementation uses framework facilities underneath the sketch. Application code remains single-threaded from the sketch's perspective. Web handlers set flags for operations that should run from the main loop rather than performing every long operation inside the HTTP callback.

## 5. Data ownership

### Indoor state

`Sen66Data indoor` contains all processed SEN66 channels.
It also contains particle AQI, validity, update time, and the serial number.

The `temperature` and `humidity` variables select the active display source.
They use SEN66 data while the SEN66 measures.
They use corrected SHTC3 data while the SEN66 is idle.

`SensorComparisonState sensorComparison` owns the paired statistics and qualification state.

### Outdoor state

`WeatherData weatherData` stores current conditions, outdoor PM2.5/AQI, three
forecast days, sunrise/sunset, pressure, update time, and validity.
`HourlyData hourlyData` stores six forecast records. The WeatherAPI location
object also updates the active IANA timezone, current UTC offset, and RTC.

The WeatherAPI response is reduced immediately. the original JSON body is not retained after parsing.

### Persistent state

ESP32 `Preferences` uses namespace `dash`. Persisted configuration is listed in [WEB_INTERFACE.md](WEB_INTERFACE.md).

FATFS stores `/sensor_compare.csv` and `/history.csv`.
NVS stores the sensor-comparison model and the SEN66 VOC state.

## 6. Display pipeline

1. The selected page draws into a 400 x 300 one-bit `GFXcanvas1`.
2. `pushCanvasToRLCD()` clears the device-side display buffer.
3. Each set canvas bit is converted to an RLCD pixel operation.
4. `DisplayPort::RLCD_Display()` transfers the prepared frame through SPI.

The low-level driver allocates PSRAM for its display buffer and pixel lookup tables. The application canvas consumes approximately 15,000 bytes before library overhead.

The reflective LCD holds its image without a backlight.
Low-power mode uses 0.5 Hz display writes.
A button press starts a 60-second high-power interaction window.

The display schedule sends ST7305 sleep and wake commands. Low-power mode also uses ESP32-S3 light sleep.

## 7. Navigation

There are 14 compiled pages. `pageEnabledMask` is a 14-bit inclusion mask:

- hardware-button next/previous navigation calls `nextEnabledPage()`.
- automatic cycling uses the same function.
- direct web selection can display any compiled page, even if it is excluded from the normal sequence.
- the HTTP handler rejects any operation that would leave zero enabled pages.

Alarm and timer events can force the Timers page regardless of its navigation-mask state.

## 8. Fault boundaries

| Fault | Application behavior |
|---|---|
| SEN66 missing at `0x6B` | indoor data remains invalid. system continues |
| SEN66 read error | error count increments. last valid display state remains |
| Wi-Fi unavailable | indoor functions remain operational. web and remote weather unavailable |
| WeatherAPI HTTP or parsing failure | current remote state is not marked successfully refreshed. last resolved timezone remains active |
| NTP unavailable | RTC remains the local time source |
| PSRAM allocation failure | driver assertion can halt startup |
| Invalid web location | HTTP 400. previous location is retained |
| Last page unchecked | HTTP 409. mask is not changed |

## 9. Architectural constraints

- WeatherAPI parsing uses positional string searches. A WeatherAPI schema change can break the parser.
- OpenWeather parsing uses filtered ArduinoJson documents.
- HTTP is unauthenticated and must remain on a trusted LAN.
- The code is sized for the 3 MB application partition. The generic 1.25 MB Arduino partition is close to capacity and is not the intended deployment configuration.
- Timezone and current UTC offset are resolved from WeatherAPI for the active
  coordinates. The offset is refreshed every 30 minutes while online, including
  across DST changes, and the last resolved value is retained in NVS for
  offline reboot behavior.
- External-power mode keeps the SEN66 active.
- Low-power mode uses a 90-second SEN66 window in each ten-minute slot.
- The SEN66 runs from minute 50 through minute 59 for the hourly update.
- The firmware saves the VOC algorithm state. The SEN66 does not export a separate NOx state.
