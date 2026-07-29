# Web Interface and Persistence

## 1. Network model

The ESP32 listens on TCP port 80 after joining the configured Wi-Fi network. The server is intended for a trusted local subnet. There is no TLS, login, session, role, or request-origin validation.

## 2. User-facing pages

| Path | Function |
|---|---|
| `/` | indoor summary, actions, location, audio, cycling, and LCD page controls |
| `/weather` | complete indoor readings, outdoor readings, and combined view |
| `/seasons` | Northern Hemisphere season summary |
| `/timers` | countdown timer, stopwatch, alarm configuration, and alarm audio |

The root page shows battery voltage, estimated charge, power mode, and estimated time to 20 percent.

The time estimate needs five samples and two hours of discharge data.

## 3. Control endpoints

Most legacy controls use HTTP GET requests for state-changing operations. The
calibration controls use POST because they initiate a persistent sensor change.
Neither pattern is appropriate for exposure outside a trusted network without
authentication and request-origin protection.

| Endpoint | Parameters | Effect |
|---|---|---|
| `/setpage` | `p` | immediately displays a compiled page |
| `/setpageenabled` | `p`, `v` | includes or excludes a page from navigation |
| `/setcycle` | `v` | enables/disables automatic cycling |
| `/setcyclesec` | `v` | sets 3-300 second dwell |
| `/setbeep` | `v` | enables/disables navigation audio |
| `/setalarmaudio` | `v` | enables/disables alarm/timer chimes |
| `/setbeepvol` | `v` | sets audio amplitude percentage |
| `/setinvert` | `v` | changes display inversion |
| `/setweatherlocation` | `v` | validates and stores decimal latitude/longitude |
| `/refresh` | none | schedules WeatherAPI and OpenWeather refresh |
| `/co2cal/start` (POST) | `confirmed=true` | downloads pressure and begins the guarded five-minute outdoor calibration |
| `/co2cal/cancel` (POST) | none | cancels qualification before the FRC write starts |
| `/co2cal/status` (GET) | none | returns calibration state, countdown, CO2, pressure, and reference-band status |
| `/syncntp` | none | schedules NTP synchronization |
| `/setsleep` | schedule arguments | stores display-sleep configuration |
| `/wakenow` | none | temporarily wakes the display |
| `/settimer` | name/duration | stores timer definition |
| `/timerstart`, `/timerstop`, `/timerreset` | none | controls countdown timer |
| `/swstart`, `/swstop`, `/swreset` | none | controls stopwatch |
| `/setalarm` | alarm fields | stores one alarm |
| `/screenshot` | none | returns current canvas as one-bit BMP |

## 4. Location validation

The location control accepts one comma-separated latitude/longitude pair:

```text
40.712800,-74.006000
```

Validation enforces:

- one comma
- numeric parsing of both fields
- latitude between -90 and +90
- longitude between -180 and +180
- maximum stored length.

The stored value replaces the compiled `weatherLocation` default for future
WeatherAPI requests. A successful response also updates the active IANA
timezone, UTC offset, system timezone environment, and RTC. It does not change
the Wi-Fi credentials, API key, or `secrets.h`.

## 5. SEN66 outdoor calibration control

The root page contains a persistent forced-CO2 recalibration control. The
operator must confirm that the complete SEN66 assembly is outdoors. Starting
the workflow then:

1. Require an active Wi-Fi connection and a valid SEN66 CO2 frame.
2. Request new WeatherAPI data for the configured coordinates.
3. Validate `pressure_mb` from 700 through 1200 hPa.
4. Send the pressure to the SEN66 before qualification.
5. Require five continuous minutes from 350 through 450 ppm.
6. Reset the timer after an invalid or out-of-band reading.
7. Stop continuous measurement.
8. Wait 1500 ms.
9. Run FRC at 400 ppm.
10. Restart continuous measurement.

The outdoor checkbox is an operator attestation. The device cannot determine
whether it is physically outdoors or whether the reference atmosphere is
traceable. The returned correction and failure state are shown in the web
status text and Serial Monitor.

## 6. NVS namespace

Namespace: `dash`

| Key | Type | Meaning |
|---|---|---|
| `disp_inv` | bool | display inversion |
| `beep_en` | bool | page-navigation audio |
| `alarm_audio` | bool | alarm/timer chimes |
| `beep_vol` | int | audio level |
| `cycle_en` | bool | automatic page cycling |
| `cycle_sec` | int | cycle dwell |
| `page_mask` | unsigned short | per-page inclusion bit mask |
| `sleep_en` | bool | display-sleep schedule enabled |
| `sleep_from` | int | sleep start hour |
| `sleep_to` | int | wake hour |
| `weather_loc` | string | active WeatherAPI latitude/longitude or location |
| `tz_id` | string | last WeatherAPI-resolved IANA timezone |
| `tz_off` | long | last current UTC offset in seconds |
| `alN_h` | byte | alarm hour |
| `alN_m` | byte | alarm minute |
| `alN_lbl` | string | alarm label |
| `alN_day` | byte | weekday mask |
| `alN_en` | bool | alarm enabled |
| `tmr_name` | string | timer label |
| `tmr_dur` | unsigned int | timer duration |

Runtime countdown and stopwatch progress are not persisted across reset.

## 7. Page inclusion behavior

- A checked page is available to short/long hardware-button navigation and auto-cycle.
- An unchecked page is skipped.
- Direct web selection is always permitted for diagnostics.
- At least one page must remain checked.
- Alarm or timer activation can force page 13.

## 8. Audio separation

`beep_en` controls page-change clicks only.

`alarm_audio` controls countdown and alarm chimes only.

Both flags are persistent. Disabling either flag does not remove the relevant visual state.

## 9. Recommended hardening

For any network other than a controlled home/lab LAN:

- Place the device on an isolated IoT VLAN.
- Block inbound connections from untrusted segments.
- Do not forward TCP port 80.
- Add authentication and CSRF protection before shared deployment.
- Do not return the SSID or precise location to unauthenticated clients.
- Change state control endpoints to authenticated POST requests.
- Apply request-rate limits.

## 10. Low-power web access

The firmware turns Wi-Fi off between remote data updates in low-power mode.

Hold GPIO 0 for one second to start a five-minute Wi-Fi window.

The web interface is unavailable after that window closes.

## 11. Setup access point

The firmware starts `SEN66-Setup` for ten minutes after a failed startup connection.

Connect a phone to this access point with the password `sen66-setup`.

The setup page stores a Wi-Fi SSID, a password, and coordinates in NVS.

The page can request the phone location. Browser security can block location access on local HTTP pages.

Enter the coordinates manually if the browser blocks location access.
