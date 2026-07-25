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

## 3. Control endpoints

The current implementation uses HTTP GET requests for state-changing operations. This is expedient for an embedded LAN tool but is not appropriate for exposure outside a trusted network.

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
| `/refresh` | none | schedules WeatherAPI refresh |
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

- one comma;
- numeric parsing of both fields;
- latitude between -90 and +90;
- longitude between -180 and +180;
- maximum stored length.

The stored value replaces the compiled `weatherLocation` default for future WeatherAPI requests. It does not change the Wi-Fi credentials, API key, POSIX time zone, or `secrets.h`.

## 5. NVS namespace

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
| `alN_h` | byte | alarm hour |
| `alN_m` | byte | alarm minute |
| `alN_lbl` | string | alarm label |
| `alN_day` | byte | weekday mask |
| `alN_en` | bool | alarm enabled |
| `tmr_name` | string | timer label |
| `tmr_dur` | unsigned int | timer duration |

Runtime countdown and stopwatch progress are not persisted across reset.

## 6. Page inclusion behavior

- A checked page is available to short/long hardware-button navigation and auto-cycle.
- An unchecked page is skipped.
- Direct web selection is always permitted for diagnostics.
- At least one page must remain checked.
- Alarm/timer firing can force page 12.

## 7. Audio separation

`beep_en` controls page-change clicks only.

`alarm_audio` controls countdown and alarm chimes only.

Both flags are persistent. Disabling either flag does not remove the relevant visual state.

## 8. Recommended hardening

For any network other than a controlled home/lab LAN:

- place the device on an isolated IoT VLAN;
- block inbound connections from untrusted segments;
- do not port-forward TCP 80;
- add authentication and CSRF protection before shared deployment;
- avoid returning SSID or precise location data to unauthenticated clients;
- migrate mutating endpoints to authenticated POST requests;
- apply request-rate limits.

