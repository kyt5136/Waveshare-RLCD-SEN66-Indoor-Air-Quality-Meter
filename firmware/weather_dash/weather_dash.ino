/*
 * Waveshare-RLCD SEN66 Indoor Air Quality Meter
 *
 * Immediate application baseline:
 *   JohnWillieGee/Waveshare-RLCD-ESP32S3-Weather-dashboard
 *   audited at 01dfda32398427422586dd58fd0441e5796cb510
 *
 * Earlier MIT-licensed baseline:
 *   juanjocastillo/Waveshare-RLCD-ESP32S3-Dashboard-v12_1
 *
 * This revision replaces the indoor sensor path with Sensirion SEN66,
 * removes unrelated space/ATS subsystems, localizes units and seasons,
 * and adds AQI processing, alarms, persistence, page filtering, and the
 * embedded control interface. See repository ATTRIBUTION.md before reuse.
 */

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <math.h>
#include <Wire.h>
#include <SensirionI2cSen66.h>
#include <SensirionCore.h>
struct AqiBreakpoint;
struct GraphBounds;
struct SeasonEvents;
struct SeasonInfo;
#include "display_bsp.h"
#include "font.h"
#include "secfont.h"
#include "PCF85063A-SOLDERED.h"
#include <ESP_I2S.h>   // ESP32 Arduino core 3.x — I2SClass for beep generation
#include <WebServer.h>   // Built-in ESP32 Arduino core — no extra library needed
#include <Preferences.h> // Built-in NVS key-value storage — persists across reboots



// ===== CONFIGURATION =====
#include "secrets.h"   // WiFi SSID/password, API key, location — keep out of version control

const char* ntpServer          = "us.pool.ntp.org";

// North American Eastern time. The POSIX rule applies U.S./Canadian DST.
// Use CST6CDT, MST7MDT, PST8PDT, or MST7 for other North American zones.
const char* posixTZ = "EST5EDT,M3.2.0,M11.1.0";

// gmtOffset_sec is now computed at runtime from the system clock after NTP sync.
// It reflects the current UTC offset including DST. Do NOT hardcode this.
long gmtOffset_sec = -18000;  // default EST; overwritten after NTP sync

const int BTN_LEFT    = 0;
const int BTN_MIDDLE  = 18;
const int BAT_ADC_PIN = 4;
static const uint8_t SEN66_ADDR = 0x6B;

#define FONT_SMALL   FreeSans9pt7b
#define FONT_MEDIUM  FreeSans12pt7b
#define FONT_LARGE   FreeSans18pt7b
#define FONT_XLARGE  FreeSans24pt7b



static const int W = 400;
static const int H = 300;
DisplayPort RlcdPort(12, 11, 5, 40, 41, W, H);
GFXcanvas1  canvas(W, H);
PCF85063A   rtc;
SensirionI2cSen66 sen66;

float temperature    = 0.0f;
float humidity       = 0.0f;
float batteryVoltage = 0.0f;
int   wifiRSSI       = 0;
int   hour24         = 0;
int   minuteVal      = 0;
int   secondVal      = 0;
bool  wifiConnected  = false;
unsigned long lastSensorReadMs = 0;
unsigned long lastBatteryReadMs = 0;
unsigned long ntpLastSync = 0;
int   sensorReadCount     = 0;
int   sensorFailCount     = 0;

static inline float cToF(float celsius) {
  return celsius * 9.0f / 5.0f + 32.0f;
}

int  currentPage  = 0;
const int totalPages = 13;
const uint16_t ALL_PAGE_MASK = (1U << totalPages) - 1U;
uint16_t pageEnabledMask = ALL_PAGE_MASK;
char activeWeatherLocation[64] = "";

bool pageIsEnabled(int page) {
  return page >= 0 && page < totalPages && (pageEnabledMask & (1U << page));
}

int nextEnabledPage(int fromPage, int direction) {
  for (int step = 1; step <= totalPages; step++) {
    int candidate = (fromPage + direction * step) % totalPages;
    if (candidate < 0) candidate += totalPages;
    if (pageIsEnabled(candidate)) return candidate;
  }
  return fromPage;
}

// Flash state for Earth dot on seasons orbit page
bool     earthFlashOn    = true;
unsigned long earthFlashLast = 0;
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200;
bool btn_left_pressed   = false;
bool btn_middle_pressed = false;

// Long-press navigation
const unsigned long LONG_PRESS_MS = 700;
unsigned long lastBtnLeftDown   = 0;
unsigned long lastBtnMiddleDown = 0;
bool btnLeftPrev    = true;
bool btnMiddlePrev  = true;
bool btnLeftHeld    = false;
bool btnMiddleHeld  = false;

unsigned long bootMillis     = 0;

const int HISTORY_SIZE = 24;

struct WeatherData {
  float  currentTemp;
  float  feelsLike;
  String condition;
  int    humidity;
  float  windSpeed;
  String windDir;
  float  uvIndex;
  float  precipMM;
  int    airQualityIndex;
  String airQualityText;
  float  pm25;
  int    pmAqi;
  String sunrise;
  String sunset;
  struct Forecast {
    String day;
    float  maxTemp;
    float  minTemp;
    String condition;
    float  precipMM;
    int    rainChance;
  } forecast[3];
  unsigned long lastUpdate;
  bool valid;
} weatherData;

struct Sen66Data {
  float pm1;
  float pm25;
  float pm4;
  float pm10;
  float humidity;
  float temperature;
  float vocIndex;
  float noxIndex;
  uint16_t co2;
  int particleAqi;
  bool valid;
  unsigned long lastUpdate;
  char serialNumber[32];
} indoor;

struct HourlyData {
  float  temp[6];
  int    rainChance[6];
  float  rainMM[6];
  float  uvIndex[6];
  float  windSpeed[6];
  String time[6];
  bool   valid;
} hourlyData;

struct HistoricalData {
  float tempHistory[HISTORY_SIZE];
  float humidityHistory[HISTORY_SIZE];
  int   currentIndex;
  unsigned long lastLogTime;
  bool  initialized;
  int   sampleCount;
} history;

struct GraphBounds { float mn, mx, rng; };

struct SeasonEvents {
  int marchEq;
  int juneSol;
  int septEq;
  int decSol;
};

struct SeasonInfo {
  const char* name;
  int daysSince;
  int daysUntil;
  const char* nextEvent;
  int nextEventDoy;
};

// ===== AUDIO ENGINE =====
// Hardware path: ESP32 I2S → ES8311 codec (I2C 0x18) → NS4150B amp (PA_CTRL GPIO46) → speaker
// Uses ESP32 Arduino core 3.x I2SClass — requires core v3.0+

static const int SPK_EN_PIN    = 46;   // NS4150B amp enable (HIGH = on)
static const int I2S_BCLK_PIN  = 9;
static const int I2S_LRCLK_PIN = 45;
static const int I2S_DOUT_PIN  = 8;
static const int I2S_MCLK_PIN  = 16;

I2SClass i2sAudio;
bool audioReady  = false;   // I2S bus initialised
bool codecReady  = false;   // ES8311 responded and configured

unsigned long lastBatteryBeepMs = 0;  // throttle low-battery beep to once per minute

// ===== WEB UI GLOBALS =====
WebServer    webServer(80);
Preferences  prefs;

bool          displayInvert    = false;  // controlled via web UI toggle; persists in NVS
bool          beepEnabled      = true;   // page-change click on/off; persists in NVS
bool          alarmAudioEnabled = true;  // alarm/timer chimes on/off; persists in NVS
int           beepVolume       = 50;     // beep amplitude 1-100%; persists in NVS
bool          autoCycleEnabled = false;  // auto-advance pages; persists in NVS
int           autoCycleSeconds = 10;     // seconds per page when auto-cycling
unsigned long autoCycleLastMs  = 0;      // millis() of last auto-cycle step

// ===== DISPLAY SLEEP =====
bool          sleepEnabled    = false;   // NVS "sleep_en"  — disabled by default
int           sleepFromHour   = 23;      // NVS "sleep_from"
int           sleepToHour     = 6;       // NVS "sleep_to"
unsigned long sleepWakeUntil  = 0;       // millis() deadline for 10 s button wake

// ===== TIMERS & ALARMS =====
#define ALARM_COUNT 3

struct AlarmDef {
  uint8_t  hour;           // 0-23
  uint8_t  minute;         // 0-59
  char     label[13];      // 12 chars + null
  uint8_t  dayMask;        // bits 0-6 = Mon-Sun; 0x00 = one-shot
  bool     enabled;
};

struct TimerState {
  char     name[21];       // 20 chars + null
  uint32_t durationSecs;   // set duration
  uint32_t remainingSecs;  // countdown value (updated each second)
  unsigned long lastTick;  // millis() of last second decrement
  bool     running;
  bool     expired;
};

struct StopwatchState {
  unsigned long startMs;   // millis() when started (or last resumed)
  unsigned long elapsed;   // accumulated ms before last pause
  bool     running;
};

AlarmDef      alarms[ALARM_COUNT];
TimerState    timerState;
StopwatchState swState;

// Alarm runtime state
bool          alarmFiring        = false;   // an alarm is currently sounding
int           alarmFiringIdx     = -1;      // which alarm (0-2)
int           alarmRepeatCount   = 0;       // how many chime sequences played
unsigned long alarmNextChimeMs   = 0;       // when to play the next chime
bool          timerFiring        = false;   // countdown timer hit zero
int           timerRepeatCount   = 0;
unsigned long timerNextChimeMs   = 0;
unsigned long alarmSnoozeUntil   = 0;       // millis() snooze deadline (0 = not snoozing)
int           alarmSnoozeIdx     = -1;      // which alarm is snoozed (-1 = none)

// Web UI alarm/timer control flags (set by handlers, actioned in loop())
volatile bool webTimerStart  = false;
volatile bool webTimerStop   = false;
volatile bool webTimerReset  = false;
volatile bool webSwStart     = false;
volatile bool webSwStop      = false;
volatile bool webSwReset     = false;

// Flags set by web handlers; actioned in loop() on the main thread
volatile bool webRefreshWeather = false;
volatile bool webSyncNTP        = false;
volatile bool webPageBeep       = false;

static bool es8311WriteReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool es8311ReadReg(uint8_t reg, uint8_t* value) {
  Wire.beginTransmission(0x18);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)0x18, 1) != 1) return false;
  *value = Wire.read();
  return true;
}

// Full ES8311 DAC init at 16 kHz / 16-bit stereo.
// Register sequence from Espressif ESP-ADF ES8311 driver.
// MCLK = 4.096 MHz (board oscillator), div=256 -> exact 16 kHz.
static bool es8311InitPlayback() {
  uint8_t chipId1 = 0, chipId2 = 0;
  if (!es8311ReadReg(0xFD, &chipId1) || !es8311ReadReg(0xFE, &chipId2)) {
    Serial.println("[AUDIO] ES8311 not responding on I2C");
    return false;
  }
  bool ok = true;
  ok &= es8311WriteReg(0x44, 0x08);
  ok &= es8311WriteReg(0x44, 0x08);
  ok &= es8311WriteReg(0x01, 0x30);
  ok &= es8311WriteReg(0x02, 0x00);
  ok &= es8311WriteReg(0x03, 0x10);
  ok &= es8311WriteReg(0x16, 0x24);
  ok &= es8311WriteReg(0x04, 0x10);
  ok &= es8311WriteReg(0x05, 0x00);
  ok &= es8311WriteReg(0x0B, 0x00);
  ok &= es8311WriteReg(0x0C, 0x00);
  ok &= es8311WriteReg(0x10, 0x1F);
  ok &= es8311WriteReg(0x11, 0x7F);
  ok &= es8311WriteReg(0x00, 0x80);
  if (!ok) { Serial.println("[AUDIO] ES8311 open sequence failed"); return false; }

  uint8_t regv = 0;
  if (!es8311ReadReg(0x00, &regv)) return false;
  regv &= 0xBF;  // slave mode
  if (!es8311WriteReg(0x00, regv)) return false;

  regv = 0x3F; regv &= ~0x40;  // use MCLK, no invert
  if (!es8311WriteReg(0x01, regv)) return false;

  if (!es8311ReadReg(0x06, &regv)) return false;
  regv &= ~0x20;  // SCLK not inverted
  if (!es8311WriteReg(0x06, regv)) return false;

  ok  = es8311WriteReg(0x13, 0x10);
  ok &= es8311WriteReg(0x1B, 0x0A);
  ok &= es8311WriteReg(0x1C, 0x6A);
  ok &= es8311WriteReg(0x44, 0x58);
  if (!ok) return false;

  // Clock: 16 kHz with 4.096 MHz MCLK
  if (!es8311ReadReg(0x02, &regv)) return false;
  regv &= 0x07; regv |= (0 << 5); regv |= (0 << 3);
  if (!es8311WriteReg(0x02, regv)) return false;
  if (!es8311WriteReg(0x05, 0x00)) return false;

  if (!es8311ReadReg(0x03, &regv)) return false;
  regv &= 0x80; regv |= 0x10;
  if (!es8311WriteReg(0x03, regv)) return false;

  if (!es8311ReadReg(0x04, &regv)) return false;
  regv &= 0x80; regv |= 0x20;
  if (!es8311WriteReg(0x04, regv)) return false;

  if (!es8311ReadReg(0x07, &regv)) return false;
  regv &= 0xC0;
  if (!es8311WriteReg(0x07, regv)) return false;
  if (!es8311WriteReg(0x08, 0xFF)) return false;

  if (!es8311ReadReg(0x06, &regv)) return false;
  regv &= 0xE0; regv |= 0x03;  // bclk_div=4
  if (!es8311WriteReg(0x06, regv)) return false;

  // I2S, 16-bit
  uint8_t dacIface = 0, adcIface = 0;
  if (!es8311ReadReg(0x09, &dacIface) || !es8311ReadReg(0x0A, &adcIface)) return false;
  dacIface |= 0x0C; adcIface |= 0x0C;
  dacIface &= 0xFC; adcIface &= 0xFC;
  if (!es8311WriteReg(0x09, dacIface) || !es8311WriteReg(0x0A, adcIface)) return false;

  // DAC playback enabled, ADC muted
  if (!es8311ReadReg(0x09, &dacIface) || !es8311ReadReg(0x0A, &adcIface)) return false;
  dacIface &= 0xBF;
  adcIface &= 0xBF; adcIface |= 0x40;
  if (!es8311WriteReg(0x09, dacIface) || !es8311WriteReg(0x0A, adcIface)) return false;

  ok  = es8311WriteReg(0x17, 0xBF);
  ok &= es8311WriteReg(0x0E, 0x02);
  ok &= es8311WriteReg(0x12, 0x00);
  ok &= es8311WriteReg(0x14, 0x1A);
  if (!ok) return false;

  if (!es8311ReadReg(0x14, &regv)) return false;
  regv &= ~0x40;  // DMIC off
  if (!es8311WriteReg(0x14, regv)) return false;

  ok  = es8311WriteReg(0x0D, 0x01);
  ok &= es8311WriteReg(0x15, 0x40);
  ok &= es8311WriteReg(0x37, 0x08);
  ok &= es8311WriteReg(0x45, 0x00);
  if (!ok) return false;

  // Volume ~80%, DAC unmuted
  if (!es8311WriteReg(0x32, 0xCC)) return false;
  if (!es8311ReadReg(0x31, &regv)) return false;
  regv &= 0x9F;
  if (!es8311WriteReg(0x31, regv)) return false;

  Serial.printf("[AUDIO] ES8311 OK (chip %02X%02X)\n", chipId1, chipId2);
  return true;
}

bool initAudio() {
  pinMode(SPK_EN_PIN, OUTPUT);
  digitalWrite(SPK_EN_PIN, HIGH);  // enable NS4150B amp
  delay(5);
  i2sAudio.setPins(I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DOUT_PIN, -1, I2S_MCLK_PIN);
  audioReady = i2sAudio.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (!audioReady) { Serial.println("[AUDIO] I2S init failed"); return false; }
  codecReady = es8311InitPlayback();
  if (!codecReady) { Serial.println("[AUDIO] ES8311 init failed"); return false; }
  return true;
}

void audioSilenceMs(uint16_t ms) {
  if (!audioReady || ms == 0) return;
  int samples = (16000 * (int)ms) / 1000;
  uint8_t frame[4] = {0, 0, 0, 0};
  for (int i = 0; i < samples; i++) i2sAudio.write(frame, 4);
}

// Square-wave tone generator — writes directly to I2S buffer, no extra library needed
// amp parameter is the base amplitude; scaled by beepVolume (1-100%)
void playToneHz(uint16_t freq, uint16_t ms, int16_t amp = 2800) {
  if (!audioReady || !codecReady || freq < 20 || ms == 0) return;
  int vol = constrain(beepVolume, 1, 100);
  int16_t scaledAmp = (int16_t)((int)amp * vol / 100);
  if (scaledAmp < 100) scaledAmp = 100;  // floor so it's always audible
  int samples  = (16000 * (int)ms) / 1000;
  int halfWave = max(1, 16000 / ((int)freq * 2));
  int16_t sample = scaledAmp;
  uint8_t frame[4];
  for (int i = 0; i < samples; i++) {
    if ((i % halfWave) == 0) sample = -sample;
    frame[0] = (uint8_t)(sample & 0xFF);
    frame[1] = (uint8_t)((sample >> 8) & 0xFF);
    frame[2] = frame[0];
    frame[3] = frame[1];
    i2sAudio.write(frame, 4);
  }
}

// Short click on page change (~45ms) — respects beepEnabled setting
void beepPageChange() {
  if (!beepEnabled) return;
  playToneHz(1400, 45);
}

// Two-tone ascending chime at end of successful boot (~230ms total)
void beepBootOk() {
  playToneHz(1318, 80);
  audioSilenceMs(30);
  playToneHz(1760, 120);
}

// Descending double-tone for low battery warning (~320ms total)
void beepLowBattery() {
  playToneHz(900, 120);
  audioSilenceMs(40);
  playToneHz(700, 160);
}

// C5-E5-G5 major arpeggio alarm chime (~500ms total)
// volumePct: 1-100, allows ramping across repeats
void beepAlarmChime(int volumePct) {
  int savedVol = beepVolume;
  beepVolume = constrain(volumePct, 1, 100);
  playToneHz(523, 120);   // C5
  audioSilenceMs(30);
  playToneHz(659, 120);   // E5
  audioSilenceMs(30);
  playToneHz(784, 180);   // G5
  beepVolume = savedVol;
}

// Returns the ramped volume % for a given chime repeat count
// Repeats 0-2: 30%, repeats 3-5: 60%, repeat 6+: 100%
int alarmVolume(int repeatCount) {
  if (repeatCount < 3) return 30;
  if (repeatCount < 6) return 60;
  return 100;
}

// ===== ALARM & TIMER LOGIC =====

// Call once per loop() — ticks the countdown timer
void updateTimer() {
  if (!timerState.running || timerState.expired) return;
  unsigned long nowMs = millis();
  if (timerState.lastTick == 0) { timerState.lastTick = nowMs; return; }
  if (nowMs - timerState.lastTick >= 1000UL) {
    timerState.lastTick += 1000UL;
    if (timerState.remainingSecs > 0) {
      timerState.remainingSecs--;
    }
    if (timerState.remainingSecs == 0) {
      timerState.running = false;
      timerState.expired = true;
      timerFiring      = true;
      timerRepeatCount = 0;
      timerNextChimeMs = millis();
      currentPage = 12;   // jump to timers page
    }
  }
}

// Returns elapsed stopwatch milliseconds (accounts for running state)
unsigned long swElapsedMs() {
  if (swState.running) return swState.elapsed + (millis() - swState.startMs);
  return swState.elapsed;
}

// Check alarms — call once per loop() near the top
// Uses RTC hour/minute; only fires on exact minute match, once per minute
static int lastAlarmMinuteChecked = -1;
void checkAlarms() {
  if (alarmFiring || alarmSnoozeUntil > millis()) return;  // already firing or snoozed
  int h = rtc.getHour();
  int m = rtc.getMinute();
  // Avoid re-triggering within the same minute
  int minuteKey = h * 60 + m;
  if (minuteKey == lastAlarmMinuteChecked) return;

  for (int i = 0; i < ALARM_COUNT; i++) {
    if (!alarms[i].enabled) continue;
    if (alarms[i].hour != h || alarms[i].minute != m) continue;

    // Check day mask — dayMask 0x00 = one-shot (always fires if enabled)
    if (alarms[i].dayMask != 0x00) {
      // bits 0-6 = Mon(0) to Sun(6); PCF85063A getWeekday() returns 0=Sun,1=Mon...6=Sat
      int dow = rtc.getWeekday();  // 0=Sun..6=Sat
      int bit = (dow == 0) ? 6 : dow - 1;  // convert to Mon=0..Sun=6
      if (!(alarms[i].dayMask & (1 << bit))) continue;
    }

    // Fire this alarm
    lastAlarmMinuteChecked = minuteKey;
    alarmFiring      = true;
    alarmFiringIdx   = i;
    alarmRepeatCount = 0;
    alarmNextChimeMs = millis();
    alarmSnoozeUntil = 0;
    alarmSnoozeIdx   = -1;
    currentPage = 12;  // jump to timers/alarms page
    return;
  }
  lastAlarmMinuteChecked = minuteKey;
}

// Dismiss the currently firing alarm
void dismissAlarm() {
  if (!alarmFiring) return;
  alarmFiring = false;
  // One-shot: auto-disable
  if (alarmFiringIdx >= 0 && alarms[alarmFiringIdx].dayMask == 0x00) {
    alarms[alarmFiringIdx].enabled = false;
    nvsSave();
  }
  alarmFiringIdx   = -1;
  alarmRepeatCount = 0;
  alarmSnoozeUntil = 0;
  alarmSnoozeIdx   = -1;
}

// Snooze the currently firing alarm for 5 minutes
void snoozeAlarm() {
  if (!alarmFiring) return;
  alarmSnoozeIdx   = alarmFiringIdx;   // remember which alarm is snoozed
  alarmFiring      = false;
  alarmSnoozeUntil = millis() + 300000UL;  // 5 minutes
  alarmRepeatCount = 0;
  alarmFiringIdx   = -1;
  currentPage      = 12;  // stay on timers page to show snooze countdown
}

// Dismiss the firing timer
void dismissTimer() {
  timerFiring      = false;
  timerRepeatCount = 0;
  timerState.expired = false;
  timerState.remainingSecs = timerState.durationSecs;
}

// Service firing alarms/timers — plays chimes at intervals
void serviceAlarmChimes() {
  if (!alarmAudioEnabled) return;
  unsigned long nowMs = millis();
  if (alarmFiring && nowMs >= alarmNextChimeMs) {
    beepAlarmChime(alarmVolume(alarmRepeatCount));
    alarmRepeatCount++;
    alarmNextChimeMs = nowMs + 800UL;  // gap between chimes
  }
  if (timerFiring && nowMs >= timerNextChimeMs) {
    beepAlarmChime(alarmVolume(timerRepeatCount));
    timerRepeatCount++;
    timerNextChimeMs = nowMs + 800UL;
  }
}

// ===== TZ LABEL HELPER =====
const char* getTZLabel() {
  time_t now = time(nullptr);
  struct tm* ti = localtime(&now);
  bool dst = (ti && ti->tm_isdst > 0);
  if (gmtOffset_sec == -14400) return "EDT";
  if (gmtOffset_sec == -18000) return dst ? "CDT" : "EST";
  if (gmtOffset_sec == -21600) return dst ? "MDT" : "CST";
  if (gmtOffset_sec == -25200) return dst ? "PDT" : "MST";
  if (gmtOffset_sec == -28800) return "PST";
  if (gmtOffset_sec == 0) return "UTC";
  return "LOCAL";
}

// ===== CENTERED TEXT HELPER =====
void printCentered(const GFXfont* font, int y, const char* text) {
  canvas.setFont(font);
  int16_t x1, y1; uint16_t tw, th;
  canvas.getTextBounds(text, 0, y, &x1, &y1, &tw, &th);
  canvas.setCursor((W - tw) / 2 - x1, y);
  canvas.print(text);
}

// ===== SEN66 + PARTICLE AQI =====
#ifdef NO_ERROR
#undef NO_ERROR
#endif
#define NO_ERROR 0

struct AqiBreakpoint {
  float cLow;
  float cHigh;
  int iLow;
  int iHigh;
};

// U.S. EPA PM2.5 breakpoints effective May 6, 2024.
static const AqiBreakpoint PM25_BP[] = {
  {0.0f, 9.0f, 0, 50}, {9.1f, 35.4f, 51, 100},
  {35.5f, 55.4f, 101, 150}, {55.5f, 125.4f, 151, 200},
  {125.5f, 225.4f, 201, 300}, {225.5f, 325.4f, 301, 500}
};

static const AqiBreakpoint PM10_BP[] = {
  {0.0f, 54.0f, 0, 50}, {55.0f, 154.0f, 51, 100},
  {155.0f, 254.0f, 101, 150}, {255.0f, 354.0f, 151, 200},
  {355.0f, 424.0f, 201, 300}, {425.0f, 604.0f, 301, 500}
};

int interpolateAqi(float c, const AqiBreakpoint* bp, size_t count) {
  c = max(0.0f, c);
  for (size_t i = 0; i < count; i++) {
    if (c <= bp[i].cHigh) {
      float result = (float)(bp[i].iHigh - bp[i].iLow) /
                     (bp[i].cHigh - bp[i].cLow) *
                     (c - bp[i].cLow) + bp[i].iLow;
      return constrain((int)lroundf(result), 0, 500);
    }
  }
  return 500;
}

int pm25ToAqi(float pm25) {
  float truncated = floorf(max(0.0f, pm25) * 10.0f) / 10.0f;
  return interpolateAqi(truncated, PM25_BP, sizeof(PM25_BP) / sizeof(PM25_BP[0]));
}

int pm10ToAqi(float pm10) {
  float truncated = floorf(max(0.0f, pm10));
  return interpolateAqi(truncated, PM10_BP, sizeof(PM10_BP) / sizeof(PM10_BP[0]));
}

const char* aqiCategory(int aqi) {
  if (aqi <= 50) return "GOOD";
  if (aqi <= 100) return "MODERATE";
  if (aqi <= 150) return "SENSITIVE";
  if (aqi <= 200) return "UNHEALTHY";
  if (aqi <= 300) return "VERY UNHEALTHY";
  return "HAZARDOUS";
}

int blendedParticleAqi() {
  bool haveIndoor = indoor.valid;
  bool haveOutdoor = weatherData.valid && weatherData.pmAqi >= 0;
  if (haveIndoor && haveOutdoor)
    return (int)lroundf((indoor.particleAqi + weatherData.pmAqi) / 2.0f);
  if (haveIndoor) return indoor.particleAqi;
  if (haveOutdoor) return weatherData.pmAqi;
  return -1;
}

bool initSen66() {
  Wire.beginTransmission(SEN66_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("[SEN66] No response at 0x6B");
    return false;
  }

  sen66.begin(Wire, SEN66_I2C_ADDR_6B);
  int16_t error = sen66.deviceReset();
  if (error != NO_ERROR) {
    Serial.printf("[SEN66] Reset failed: %d\n", error);
    return false;
  }
  delay(1200);

  int8_t serial[32] = {0};
  error = sen66.getSerialNumber(serial, sizeof(serial));
  if (error != NO_ERROR) {
    Serial.printf("[SEN66] Serial read failed: %d\n", error);
    return false;
  }
  strncpy(indoor.serialNumber, (const char*)serial, sizeof(indoor.serialNumber) - 1);

  error = sen66.startContinuousMeasurement();
  if (error != NO_ERROR) {
    Serial.printf("[SEN66] Start failed: %d\n", error);
    return false;
  }
  Serial.printf("[SEN66] Started, serial %s\n", indoor.serialNumber);
  return true;
}

bool readSen66() {
  int16_t error = sen66.readMeasuredValues(
    indoor.pm1, indoor.pm25, indoor.pm4, indoor.pm10,
    indoor.humidity, indoor.temperature, indoor.vocIndex,
    indoor.noxIndex, indoor.co2);
  if (error != NO_ERROR) {
    Serial.printf("[SEN66] Read failed: %d\n", error);
    return false;
  }

  indoor.particleAqi = max(pm25ToAqi(indoor.pm25), pm10ToAqi(indoor.pm10));
  indoor.valid = true;
  indoor.lastUpdate = millis();
  temperature = indoor.temperature;
  humidity = indoor.humidity;
  return true;
}

// ===== BATTERY =====
float readBatteryVoltage() {
  int raw = analogRead(BAT_ADC_PIN);
  return (raw / 4095.0f) * 3.3f * 3.0f * 1.079f;
}

// Returns true if the display should currently be sleeping.
// Handles midnight-spanning windows (e.g. 23:00 -> 06:00).
// Overridden to false when a 10 s button-wake is active.
bool isDisplaySleeping() {
  if (!sleepEnabled) return false;
  if (millis() < sleepWakeUntil) return false;
  int h = rtc.getHour();
  if (sleepFromHour <= sleepToHour) {
    return (h >= sleepFromHour && h < sleepToHour);
  } else {
    // Window spans midnight (e.g. 23 -> 06)
    return (h >= sleepFromHour || h < sleepToHour);
  }
}

// Build an HTML <option> list for a 24-hour hour selector
String buildHourOptions(int selected) {
  String s = "";
  for (int h = 0; h < 24; h++) {
    char buf[52];  // "<option value='23' selected>23:00</option>" = 42 chars + null
    snprintf(buf, sizeof(buf), "<option value='%d'%s>%02d:00</option>",
             h, (h == selected ? " selected" : ""), h);
    s += String(buf);
  }
  return s;
}

int batteryToSegments(float vbat) {
  if (vbat >= 4.0f)  return 5;
  if (vbat >= 3.90f) return 4;
  if (vbat >= 3.80f) return 3;
  if (vbat >= 3.65f) return 2;
  if (vbat >= 3.50f) return 1;
  return 0;
}



// ===== WEATHER API =====
bool fetchWeatherData() {
  if (!wifiConnected) return false;
  HTTPClient http;
  String encodedLocation = String(activeWeatherLocation);
  encodedLocation.replace(" ", "%20");
  String url = "https://api.weatherapi.com/v1/forecast.json?key=" + String(weatherApiKey) +
               "&q=" + encodedLocation + "&days=3&aqi=yes&alerts=no";
  http.begin(url); http.setTimeout(15000);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String payload = http.getString(); http.end();
  if (payload.length() < 5000) return false;

  auto pfloat = [&](const char* key, int from, int to) -> float {
    int p = payload.indexOf(key, from);
    if (p < 0 || (to > 0 && p > to)) return 0.0f;
    p += strlen(key);
    int e1 = payload.indexOf(",", p), e2 = payload.indexOf("}", p);
    int e = (e1 > 0 && (e2 < 0 || e1 < e2)) ? e1 : e2;
    String s = payload.substring(p, e); s.trim(); return s.toFloat();
  };
  auto pint = [&](const char* key, int from, int to) -> int {
    int p = payload.indexOf(key, from);
    if (p < 0 || (to > 0 && p > to)) return 0;
    p += strlen(key);
    int e1 = payload.indexOf(",", p), e2 = payload.indexOf("}", p);
    int e = (e1 > 0 && (e2 < 0 || e1 < e2)) ? e1 : e2;
    String s = payload.substring(p, e); s.trim(); return s.toInt();
  };
  auto pstr = [&](const char* key, int from) -> String {
    int p = payload.indexOf(key, from);
    if (p < 0) return "";
    p += strlen(key);
    return payload.substring(p, payload.indexOf("\"", p));
  };

  int curPos   = payload.indexOf("\"current\":");
  int fcastPos = payload.indexOf("\"forecast\":");


  weatherData.currentTemp = pfloat("\"temp_c\":",      curPos, fcastPos);
  weatherData.feelsLike   = pfloat("\"feelslike_c\":", curPos, fcastPos);
  weatherData.humidity    = pint  ("\"humidity\":",    curPos, fcastPos);
  weatherData.windSpeed   = pfloat("\"wind_kph\":",    curPos, fcastPos);
  weatherData.windDir     = pstr  ("\"wind_dir\":\"",  curPos);
  weatherData.condition   = pstr  ("\"text\":\"",      curPos);
  weatherData.precipMM    = pfloat("\"precip_mm\":",   curPos, fcastPos);

  {
    int uvPos = payload.indexOf("\"uv\":", curPos);
    if (uvPos > 0 && uvPos < fcastPos) {
      uvPos += 5;
      int e1 = payload.indexOf(",", uvPos), e2 = payload.indexOf("}", uvPos);
      int e = (e1 > 0 && (e2 < 0 || e1 < e2)) ? e1 : e2;
      String s = payload.substring(uvPos, e); s.trim();
      weatherData.uvIndex = s.toFloat();
    } else weatherData.uvIndex = 0.0f;
  }

  weatherData.airQualityIndex = pint("\"us-epa-index\":", curPos, fcastPos);
  weatherData.pm25            = pfloat("\"pm2_5\":",       curPos, fcastPos);
  weatherData.pmAqi           = pm25ToAqi(weatherData.pm25);
  switch (weatherData.airQualityIndex) {
    case 1: weatherData.airQualityText = "Good";           break;
    case 2: weatherData.airQualityText = "Moderate";       break;
    case 3: weatherData.airQualityText = "Unhealthy+";     break;
    case 4: weatherData.airQualityText = "Unhealthy";      break;
    case 5: weatherData.airQualityText = "Very Unhealthy"; break;
    case 6: weatherData.airQualityText = "Hazardous";      break;
    default:weatherData.airQualityText = "Unknown";        break;
  }

  int arrPos = payload.indexOf("\"forecastday\":[");
  if (arrPos > 0) {
    int astroPos = payload.indexOf("\"astro\":", arrPos);
    if (astroPos > 0) {
      weatherData.sunrise = pstr("\"sunrise\":\"", astroPos);
      weatherData.sunset  = pstr("\"sunset\":\"", astroPos);
    }
    int sPos = arrPos;
    for (int i = 0; i < 3; i++) {
      int p = payload.indexOf("\"date\":\"", sPos); if (p < 0) break;
      p += 8; int e = payload.indexOf("\"", p);
      weatherData.forecast[i].day = payload.substring(p, e); sPos = e;
      int dObj = payload.indexOf("\"day\":{", sPos); if (dObj < 0) break;
      weatherData.forecast[i].maxTemp    = pfloat("\"maxtemp_c\":",        dObj, dObj+2000);
      weatherData.forecast[i].minTemp    = pfloat("\"mintemp_c\":",        dObj, dObj+2000);
      weatherData.forecast[i].precipMM   = pfloat("\"totalprecip_mm\":",   dObj, dObj+2000);
      weatherData.forecast[i].rainChance = pint  ("\"daily_chance_of_rain\":", dObj, dObj+2000);
      int cPos = payload.indexOf("\"condition\":", dObj);
      if (cPos > 0) { weatherData.forecast[i].condition = pstr("\"text\":\"", cPos); sPos = cPos+100; }
      else sPos = dObj+100;
    }
  }


  // Select the next six full forecast hours by Unix epoch. Scanning all three
  // forecast days allows the list to roll cleanly through midnight.
  int hourPos = payload.indexOf("\"hour\":[", arrPos);
  if (hourPos > 0) {
    int sPos = hourPos, found = 0;
    time_t nowEpoch = time(nullptr);
    if (nowEpoch < 1000000000) {
      nowEpoch = (time_t)pint("\"localtime_epoch\":", 0, curPos);
    }
    for (int h = 0; h < 72 && found < 6; h++) {
      int recordStart = sPos;
      int tp = payload.indexOf("\"time\":\"", sPos); if (tp < 0) break;
      tp += 8; int te = payload.indexOf("\"", tp);
      String ts = payload.substring(tp, te);
      int nextTp = payload.indexOf("\"time\":\"", te);
      int recordEnd = (nextTp > 0) ? nextTp : min((int)payload.length(), te + 2500);
      int epoch = pint("\"time_epoch\":", recordStart, tp);
      sPos = te;
      if ((time_t)epoch <= nowEpoch) continue;
      hourlyData.time[found]       = ts.substring(11, 16);
      hourlyData.temp[found]       = pfloat("\"temp_c\":",         te, recordEnd);
      hourlyData.rainChance[found] = pint  ("\"chance_of_rain\":", te, recordEnd);
      hourlyData.rainMM[found]     = pfloat("\"precip_mm\":",      te, recordEnd);
      hourlyData.uvIndex[found]    = pfloat("\"uv\":",             te, recordEnd);
      hourlyData.windSpeed[found]  = pfloat("\"wind_kph\":",       te, recordEnd);
      found++;
    }
    hourlyData.valid = (found == 6);
  }

  weatherData.lastUpdate = millis();
  weatherData.valid = true;
  return true;
}

// ===== DISPLAY HELPERS =====
void pushCanvasToRLCD(bool invert = false) {
  uint8_t *buf = canvas.getBuffer();
  const int bpr = (W + 7) / 8;
  RlcdPort.RLCD_ColorClear(ColorWhite);
  for (int y = 0; y < H; y++) {
    uint8_t *row = buf + y * bpr;
    for (int bx = 0; bx < bpr; bx++) {
      uint8_t v = invert ? row[bx] ^ 0xFF : row[bx];
      int x0 = bx * 8;
      for (int bit = 0; bit < 8; bit++) {
        int x = x0 + bit; if (x >= W) break;
        if (v & (0x80 >> bit)) RlcdPort.RLCD_SetPixel((uint16_t)x, (uint16_t)y, ColorBlack);
      }
    }
  }
  RlcdPort.RLCD_Display();
}

void drawThermometerIcon(int x, int y) {
  canvas.drawCircle(x+3, y+18, 4, 1); canvas.fillCircle(x+3, y+18, 2, 1);
  canvas.fillRect(x+1, y, 4, 16, 1);  canvas.fillRect(x+2, y, 2, 16, 0);
}

void drawDropletIcon(int x, int y) {
  canvas.fillCircle(x+4, y+10, 4, 1);
  canvas.fillTriangle(x+4, y, x, y+8, x+8, y+8, 1);
  canvas.fillCircle(x+4, y+10, 2, 0);
}

void drawWiFiIcon(int x, int y, int rssi) {
  int bars = 0;
  if      (rssi > -50) bars = 4;
  else if (rssi > -60) bars = 3;
  else if (rssi > -70) bars = 2;
  else if (rssi > -80) bars = 1;
  for (int i = 0; i < 4; i++) {
    int h = (i+1) * 4;
    if (i < bars && wifiConnected) canvas.fillRect(x+i*6, y+16-h, 4, h, 1);
    else                           canvas.drawRect(x+i*6, y+16-h, 4, h, 1);
  }
}


// ===== PAGE 0: DASHBOARD =====
void drawDashboardPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  canvas.fillRect(8, 8, 384, 26, 1);
  canvas.setTextColor(0); canvas.setFont(&FONT_SMALL);
  canvas.setCursor(15, 27); canvas.print("SEN66 INDOOR AIR");
  char timeStr[6]; snprintf(timeStr, sizeof(timeStr), "%02d:%02d", hour24, minuteVal);
  canvas.setCursor(327, 27); canvas.print(timeStr);
  canvas.setTextColor(1);

  // Three primary measurements.
  const int topY = 42, topH = 82, topW = 122;
  canvas.drawRect(8, topY, topW, topH, 1);
  canvas.drawRect(139, topY, topW, topH, 1);
  canvas.drawRect(270, topY, topW, topH, 1);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(15, 61); canvas.print("TEMP");
  canvas.setCursor(146, 61); canvas.print("HUMIDITY");
  canvas.setCursor(277, 61); canvas.print("CO2");

  canvas.setFont(&FONT_LARGE);
  canvas.setCursor(15, 101);
  if (indoor.valid) {
    canvas.print(cToF(indoor.temperature), 1); canvas.print(" F");
  } else canvas.print("--");
  canvas.setCursor(146, 101);
  if (indoor.valid) { canvas.print((int)indoor.humidity); canvas.print(" %"); }
  else canvas.print("--");
  canvas.setCursor(277, 101);
  if (indoor.valid && indoor.co2 != 0xFFFF) canvas.print(indoor.co2);
  else canvas.print("--");
  canvas.setFont(&FONT_SMALL); canvas.setCursor(277, 117); canvas.print("ppm");

  // VOC plus separate indoor and outdoor particle AQI values.
  const int midY = 132, midH = 72, midW = 122;
  canvas.drawRect(8, midY, midW, midH, 1);
  canvas.drawRect(139, midY, midW, midH, 1);
  canvas.drawRect(270, midY, midW, midH, 1);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(15, 151); canvas.print("VOC INDEX");
  canvas.setCursor(146, 151); canvas.print("INDOOR AQI");
  canvas.setCursor(277, 151); canvas.print("OUT AQI");
  canvas.setFont(&FONT_LARGE);
  canvas.setCursor(15, 190);
  if (indoor.valid && !isnan(indoor.vocIndex)) canvas.print(indoor.vocIndex, 0);
  else canvas.print("--");
  canvas.setCursor(146, 190);
  if (indoor.valid) canvas.print(indoor.particleAqi); else canvas.print("--");
  canvas.setCursor(277, 190);
  if (weatherData.valid) canvas.print(weatherData.pmAqi); else canvas.print("--");

  // Location-local sun times and device status footer.
  canvas.drawRect(8, 212, 384, 76, 1);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(15, 230); canvas.print("SUNRISE");
  canvas.setCursor(205, 230); canvas.print("SUNSET");
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(15, 253);
  canvas.print(weatherData.valid && weatherData.sunrise.length() ? weatherData.sunrise : "--");
  canvas.setCursor(205, 253);
  canvas.print(weatherData.valid && weatherData.sunset.length() ? weatherData.sunset : "--");
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(15, 280);
  if (!indoor.valid) {
    canvas.print("SEN66 WAITING");
  } else if (isnan(indoor.vocIndex) || isnan(indoor.noxIndex) || indoor.co2 == 0xFFFF) {
    canvas.print("SEN66 ACTIVE / PREHEATING");
  } else {
    canvas.print("SEN66 ACTIVE / READY");
  }
  canvas.setCursor(267, 280);
  canvas.print(batteryVoltage, 1); canvas.print("V");
  drawWiFiIcon(350, 265, wifiRSSI);

  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 1: ANALOGUE CLOCK =====
void drawAnalogClockPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  // Inverted header bar — location + timezone
  canvas.fillRect(8, 8, 384, 22, 1);
  canvas.setTextColor(0);
  canvas.setFont(&FONT_SMALL);
  char hdrBuf[32];
  snprintf(hdrBuf, sizeof(hdrBuf), "%s  %s", activeWeatherLocation, getTZLabel());
  int16_t hx1, hy1; uint16_t htw, hth;
  canvas.getTextBounds(hdrBuf, 0, 24, &hx1, &hy1, &htw, &hth);
  canvas.setCursor((W - htw) / 2 - hx1, 24);
  canvas.print(hdrBuf);
  canvas.setTextColor(1);

  // Clock geometry — filled black face, R=110
  const int cx = 200, cy = 152, R = 110;

  // Filled black face
  canvas.fillCircle(cx, cy, R, 1);

  // Hour markers — white, thick at 12/3/6/9, medium elsewhere
  for (int i = 0; i < 12; i++) {
    float a = (i * 30 - 90) * 3.14159f / 180.0f;
    bool isCard = (i % 3 == 0);
    int r1 = R - 2, r2 = isCard ? R - 20 : R - 12;
    int x1 = cx + (int)(r1 * cosf(a)), y1 = cy + (int)(r1 * sinf(a));
    int x2 = cx + (int)(r2 * cosf(a)), y2 = cy + (int)(r2 * sinf(a));
    // Draw thick white marker as 2-3 adjacent lines
    canvas.drawLine(x1, y1, x2, y2, 0);
    if (isCard) {
      // Extra adjacent pixels for thick cardinal markers
      int px1 = cx + (int)(r1 * cosf(a + 0.04f)), py1 = cy + (int)(r1 * sinf(a + 0.04f));
      int px2 = cx + (int)(r2 * cosf(a + 0.04f)), py2 = cy + (int)(r2 * sinf(a + 0.04f));
      canvas.drawLine(px1, py1, px2, py2, 0);
      int qx1 = cx + (int)(r1 * cosf(a - 0.04f)), qy1 = cy + (int)(r1 * sinf(a - 0.04f));
      int qx2 = cx + (int)(r2 * cosf(a - 0.04f)), qy2 = cy + (int)(r2 * sinf(a - 0.04f));
      canvas.drawLine(qx1, qy1, qx2, qy2, 0);
    }
  }

  // Minute ticks — white, thin
  for (int i = 0; i < 60; i++) {
    if (i % 5 == 0) continue;
    float a = (i * 6 - 90) * 3.14159f / 180.0f;
    int x1 = cx + (int)((R-2)  * cosf(a)), y1 = cy + (int)((R-2)  * sinf(a));
    int x2 = cx + (int)((R-7) * cosf(a)), y2 = cy + (int)((R-7) * sinf(a));
    canvas.drawLine(x1, y1, x2, y2, 0);
  }

  // Hour numerals 12, 3, 6, 9 in white (setTextColor 0 = white on filled face)
  canvas.setTextColor(0);
  canvas.setFont(&FONT_MEDIUM);
  struct { const char* n; int a; } hnums[] = {{"12",0},{"3",90},{"6",180},{"9",270}};
  for (int i = 0; i < 4; i++) {
    float rad = (hnums[i].a - 90) * 3.14159f / 180.0f;
    int nr = R - 30;
    int16_t nx1, ny1; uint16_t ntw, nth;
    canvas.getTextBounds(hnums[i].n, 0, 0, &nx1, &ny1, &ntw, &nth);
    int tx = cx + (int)(nr * cosf(rad)) - ntw/2 - nx1;
    int ty = cy + (int)(nr * sinf(rad)) + nth/2;
    canvas.setCursor(tx, ty);
    canvas.print(hnums[i].n);
  }
  canvas.setTextColor(1);

  // Compute hand angles
  int h12    = hour24 % 12;
  float secF  = (float)secondVal;
  float minF  = (float)minuteVal + secF / 60.0f;
  float hourF = (float)h12 + minF / 60.0f;
  float hourAngle = hourF * 30.0f;   // degrees
  float minAngle  = minF  * 6.0f;
  float secAngle  = secF  * 6.0f;

  // Hour hand — white, thick (3 adjacent lines)
  float hRad = (hourAngle - 90.0f) * 3.14159f / 180.0f;
  int hx = cx + (int)(63 * cosf(hRad)), hy = cy + (int)(63 * sinf(hRad));
  canvas.drawLine(cx, cy, hx, hy, 0);
  canvas.drawLine(cx+1, cy,   hx+1, hy,   0);
  canvas.drawLine(cx,   cy+1, hx,   hy+1, 0);
  canvas.drawLine(cx-1, cy,   hx-1, hy,   0);
  canvas.drawLine(cx,   cy-1, hx,   hy-1, 0);

  // Minute hand — white, medium (2 adjacent lines)
  float mRad = (minAngle - 90.0f) * 3.14159f / 180.0f;
  int mhx = cx + (int)(90 * cosf(mRad)), mhy = cy + (int)(90 * sinf(mRad));
  canvas.drawLine(cx, cy, mhx, mhy, 0);
  canvas.drawLine(cx+1, cy,   mhx+1, mhy,   0);
  canvas.drawLine(cx,   cy+1, mhx,   mhy+1, 0);

  // Second hand — white, single pixel with tail
  float sRad = (secAngle - 90.0f) * 3.14159f / 180.0f;
  int shx = cx + (int)(98  * cosf(sRad)), shy  = cy + (int)(98  * sinf(sRad));
  int stx = cx + (int)(19  * cosf(sRad + 3.14159f)), sty = cy + (int)(19 * sinf(sRad + 3.14159f));
  canvas.drawLine(stx, sty, shx, shy, 0);

  // Centre cap — white filled, black dot
  canvas.fillCircle(cx, cy, 5, 0);
  canvas.fillCircle(cx, cy, 2, 1);

  // Bottom strip — divider line then date and indoor data
  canvas.fillRect(8, 270, 384, 2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  // Date left
  char dateBuf[16];
  const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  int dow = rtc.getWeekday();
  snprintf(dateBuf, sizeof(dateBuf), "%s %d/%d/%02d",
    (dow>=0&&dow<=6)?days[dow]:"---",
    rtc.getDay(), rtc.getMonth(), rtc.getYear()%100);
  canvas.setCursor(14, 290); canvas.print(dateBuf);
  // Temp/humidity/battery right-aligned
  char infoBuf[24];
  snprintf(infoBuf, sizeof(infoBuf), "%.1fF  %d%%  %.1fV",
    cToF(temperature), (int)humidity, batteryVoltage);
  int16_t ix1, iy1; uint16_t itw, ith;
  canvas.getTextBounds(infoBuf, 0, 290, &ix1, &iy1, &itw, &ith);
  canvas.setCursor(W - 14 - itw - ix1, 290); canvas.print(infoBuf);

  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 2: NORTH AMERICAN TIME ZONES =====
void drawTimeZonePage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  printCentered(&FONT_SMALL, 24, "NORTH AMERICAN TIME ZONES");

  time_t utcNow = time(nullptr);
  struct tm* localInfo = localtime(&utcNow);
  bool dst = localInfo && localInfo->tm_isdst > 0;
  struct Zone { const char* city; const char* abbr; int offsetMinutes; };
  Zone zones[] = {
    {"New York",    dst ? "EDT" : "EST", dst ? -4*60 : -5*60},
    {"Chicago",     dst ? "CDT" : "CST", dst ? -5*60 : -6*60},
    {"Denver",      dst ? "MDT" : "MST", dst ? -6*60 : -7*60},
    {"Los Angeles", dst ? "PDT" : "PST", dst ? -7*60 : -8*60},
    {"Phoenix",     "MST",                -7*60},
    {"UTC",         "UTC",                     0}
  };

  for (int i = 0; i < 6; i++) {
    time_t zoneNow = utcNow + zones[i].offsetMinutes * 60;
    struct tm z;
    gmtime_r(&zoneNow, &z);
    int col = i % 2, row = i / 2;
    int x = 8 + col * 196, y = 38 + row * 82;
    canvas.drawRect(x, y, 188, 70, 1);
    canvas.setFont(&FONT_SMALL); canvas.setCursor(x+7, y+18);
    canvas.print(zones[i].city); canvas.print(" "); canvas.print(zones[i].abbr);
    char timeBuf[9]; snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", z.tm_hour, z.tm_min, z.tm_sec);
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(x+32, y+50); canvas.print(timeBuf);
  }
  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 3: CURRENT CONDITIONS =====
void drawCurrentWeatherPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  if (!weatherData.valid) {
    canvas.setFont(&FONT_LARGE); canvas.setTextColor(1);
    canvas.setCursor(60, 130); canvas.print("NO WEATHER DATA");
    canvas.setFont(&FONT_SMALL); canvas.setCursor(110, 165); canvas.print("Press KEY button");
    pushCanvasToRLCD(displayInvert); return;
  }

  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 25); canvas.print("CURRENT CONDITIONS  "); canvas.print(activeWeatherLocation);

  canvas.setFont(&FONT_XLARGE); canvas.setCursor(12, 75); canvas.print(cToF(weatherData.currentTemp), 1);
  canvas.setFont(&FONT_LARGE);  canvas.print(" F");

  // Condition text — word-wrap into two lines if longer than ~20 chars
  canvas.setFont(&FONT_SMALL);
  String cond = weatherData.condition;
  const int condMaxChars = 20;
  if (cond.length() <= condMaxChars) {
    canvas.setCursor(12, 95); canvas.print(cond);
  } else {
    int sp = cond.lastIndexOf(' ', condMaxChars);
    if (sp < 1) sp = condMaxChars;
    canvas.setCursor(12, 95);  canvas.print(cond.substring(0, sp));
    canvas.setCursor(12, 109); canvas.print(cond.substring(sp + 1, min((int)cond.length(), sp + condMaxChars + 1)));
  }

  // Feels like — always on line 3
  canvas.setFont(&FONT_SMALL); canvas.setCursor(12, 123);
  canvas.print("Feels like "); canvas.print(cToF(weatherData.feelsLike), 1); canvas.print(" F");

  canvas.fillRect(200, 34, 2, 102, 1);
  canvas.fillRect(302, 34, 2, 102, 1);

  const int boxTop = 34, boxBot = 138, slotH = (boxBot - boxTop) / 2;
  canvas.setFont(&FONT_SMALL);  canvas.setCursor(208, boxTop + 16);         canvas.print("HIGH");
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(208, boxTop + 38);         canvas.print(cToF(weatherData.forecast[0].maxTemp), 1); canvas.setFont(&FONT_SMALL); canvas.print(" F");
  canvas.setFont(&FONT_SMALL);  canvas.setCursor(208, boxTop + slotH + 16); canvas.print("LOW");
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(208, boxTop + slotH + 38); canvas.print(cToF(weatherData.forecast[0].minTemp), 1); canvas.setFont(&FONT_SMALL); canvas.print(" F");
  canvas.setFont(&FONT_SMALL);  canvas.setCursor(310, boxTop + 16);         canvas.print("UV INDEX");
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(310, boxTop + 38);         canvas.print(weatherData.uvIndex, 1);
  canvas.setFont(&FONT_SMALL);  canvas.setCursor(310, boxTop + slotH + 16); canvas.print("HUMIDITY");
  canvas.setFont(&FONT_MEDIUM); canvas.setCursor(310, boxTop + slotH + 38); canvas.print(weatherData.humidity); canvas.print(" %");

  canvas.fillRect(8, 134, 384, 2, 1);

  int lx = 12, rx = 210, gy = 152, rowH = 42;
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(lx, gy); canvas.print("WIND");
  canvas.setCursor(rx, gy); canvas.print("OUTDOOR TEMP");
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(lx, gy+20); canvas.print(weatherData.windSpeed * 0.621371f, 1); canvas.print(" mph "); canvas.print(weatherData.windDir);
  canvas.setCursor(rx, gy+20); canvas.print(cToF(weatherData.currentTemp), 1); canvas.print(" F");
  gy += rowH;
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(lx, gy); canvas.print("AIR QUALITY");
  canvas.setCursor(rx, gy); canvas.print("PM2.5");
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(lx, gy+20); canvas.print(weatherData.airQualityText);
  canvas.setCursor(rx, gy+20); canvas.print(weatherData.pm25, 1); canvas.print(" ug/m3");
  gy += rowH;
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(lx, gy); canvas.print("LAST UPDATED");
  canvas.setCursor(rx, gy); canvas.print("RAINFALL");
  canvas.setFont(&FONT_MEDIUM);
  canvas.setCursor(lx, gy+20); canvas.print((millis() - weatherData.lastUpdate) / 60000); canvas.print(" min ago");
  canvas.setCursor(rx, gy+20); canvas.print(weatherData.precipMM, 1); canvas.print(" mm");

  canvas.fillRect(200, 136, 2, 132, 1);
  canvas.fillRect(8, 268, 384, 2, 1);
  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 5: 3-DAY FORECAST =====
void drawForecastPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  if (!weatherData.valid) {
    canvas.setFont(&FONT_LARGE); canvas.setTextColor(1);
    canvas.setCursor(60, 130); canvas.print("NO WEATHER DATA");
    canvas.setFont(&FONT_SMALL); canvas.setCursor(110, 165); canvas.print("Press KEY button");
    pushCanvasToRLCD(displayInvert); return;
  }

  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 25); canvas.print("3-DAY FORECAST  "); canvas.print(activeWeatherLocation);

  const int cardW = 122, cardY = 36, hdrH = 24;
  const int rowH = 40, condH = 52;
  const int cardH = hdrH + rowH + rowH + condH + rowH + rowH;

  for (int i = 0; i < 3; i++) {
    int cx = 10 + i * (cardW + 4);
    canvas.drawRect(cx, cardY, cardW, cardH, 1);
    canvas.fillRect(cx+1, cardY+1, cardW-2, hdrH-1, 1);
    String dayStr = weatherData.forecast[i].day;
    if (dayStr.length() >= 10) dayStr = dayStr.substring(5);
    canvas.setTextColor(0); canvas.setFont(&FONT_SMALL);
    canvas.setCursor(cx+6, cardY+17); canvas.print(dayStr);
    canvas.setTextColor(1);

    int y = cardY + hdrH + 2;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("HIGH");
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(cx+6, y+30); canvas.print(cToF(weatherData.forecast[i].maxTemp), 0); canvas.print(" F");
    y += rowH;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("LOW");
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(cx+6, y+30); canvas.print(cToF(weatherData.forecast[i].minTemp), 0); canvas.print(" F");
    y += rowH;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("COND");
    String fcond = weatherData.forecast[i].condition;
    if (fcond.length() > 13) {
      int sp = fcond.lastIndexOf(' ', 13);
      if (sp > 0) {
        canvas.setCursor(cx+6, y+28); canvas.print(fcond.substring(0, sp));
        canvas.setCursor(cx+6, y+42); canvas.print(fcond.substring(sp+1, min((int)fcond.length(), sp+14)));
      } else {
        canvas.setCursor(cx+6, y+28); canvas.print(fcond.substring(0, 13));
      }
    } else {
      canvas.setCursor(cx+6, y+28); canvas.print(fcond);
    }
    y += condH;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("RAIN");
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(cx+6, y+30); canvas.print(weatherData.forecast[i].precipMM, 1); canvas.print("mm");
    y += rowH;
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(cx+6, y+11); canvas.print("CHANCE");
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(cx+6, y+30);
    if (i == 0 && hourlyData.valid) {
      int maxChance = 0;
      for (int h = 0; h < 6; h++) if (hourlyData.rainChance[h] > maxChance) maxChance = hourlyData.rainChance[h];
      canvas.print(maxChance); canvas.print(" %");
    } else {
      canvas.print(weatherData.forecast[i].rainChance); canvas.print(" %");
    }
  }

  // No bottom divider line — cards contain all content
  canvas.setFont(&FONT_SMALL); canvas.setCursor(12, 285);
  canvas.print("Updated "); canvas.print((millis() - weatherData.lastUpdate) / 60000); canvas.print("m ago");
  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 13: SYSTEM INFO =====
void drawSystemPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);

  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 24); canvas.print("SYSTEM INFO");

  // 5 rows at 44px each — label at y+12, value at y+32, 18px gap between them
  const int lx = 14, rx = 204, rowH = 44, gy0 = 36;
  canvas.fillRect(198, gy0, 2, rowH * 5, 1);

  auto drawDetail = [&](int x, int y, const char* lbl, String val, bool useSmall = false) {
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(x, y+12); canvas.print(lbl);
    canvas.setFont(useSmall ? &FONT_SMALL : &FONT_MEDIUM); canvas.setCursor(x, y+32); canvas.print(val);
  };

  // Build value strings
  String wifiVal = wifiConnected ? String(wifiRSSI) + " dBm" : "Offline";
  String ipStr   = wifiConnected ? WiFi.localIP().toString() : "Not connected";
  String wxVal   = "No data";
  if (weatherData.valid) {
    unsigned long nf = 1800000UL - min((unsigned long)1800000UL, millis() - weatherData.lastUpdate);
    wxVal = String(nf/60000) + "m " + String((nf%60000)/1000) + "s";
  }
  unsigned long upSec = millis() / 1000;
  char uptimeStr[12];
  sprintf(uptimeStr, "%02d:%02d:%02d", (int)(upSec/3600), (int)((upSec%3600)/60), (int)(upSec%60));
  String ntpStr = "Never";
  if (ntpLastSync > 0) {
    unsigned long syncAgo = (millis() - ntpLastSync) / 60000;
    ntpStr = syncAgo < 60 ? String(syncAgo) + " min ago" : String(syncAgo/60) + " hr ago";
  }
  String ssidStr = wifiConnected ? String(ssid) : "--";
  if (ssidStr.length() > 20) ssidStr = ssidStr.substring(0, 20); // FONT_SMALL safe up to ~26 chars
  String chanStr  = wifiConnected ? String(WiFi.channel()) : "--";
  int total = sensorReadCount + sensorFailCount;
  String readsStr = total > 0 ? String(sensorReadCount) + "/" + String(total) + " ok" : "No reads";

  int gy = gy0;
  drawDetail(lx, gy, "WIFI",         wifiVal);
  drawDetail(rx, gy, "IP ADDRESS",   ipStr);       gy += rowH;
  drawDetail(lx, gy, "WEATHER",      weatherData.valid ? "Fresh" : "No Data");
  drawDetail(rx, gy, "WX REFRESH",   wxVal);        gy += rowH;
  drawDetail(lx, gy, "UPTIME",       String(uptimeStr));
  drawDetail(rx, gy, "NTP SYNC",     ntpStr);       gy += rowH;
  drawDetail(lx, gy, "NETWORK",      ssidStr, true);
  drawDetail(rx, gy, "CHANNEL",      chanStr);      gy += rowH;
  drawDetail(lx, gy, "SENSOR",       (sensorFailCount == 0) ? "OK" : "Errors");
  drawDetail(rx, gy, "SENSOR READS", readsStr);

  canvas.fillRect(8, 264, 384, 2, 1);
  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 11: COMPLETE SEN66 OUTPUT =====
void drawSen66DetailsPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.fillRect(8, 8, 384, 26, 1);
  canvas.setTextColor(0); canvas.setFont(&FONT_SMALL);
  canvas.setCursor(15, 27); canvas.print("SEN66 COMPLETE SENSOR OUTPUT");
  canvas.setTextColor(1);

  const int lx = 14, rx = 204, rowH = 44, gy0 = 40;
  canvas.fillRect(198, gy0, 2, rowH * 5, 1);
  for (int r = 1; r < 5; r++) canvas.drawFastHLine(8, gy0 + r * rowH, 384, 1);

  auto reading = [&](int x, int y, const char* label, const String& value) {
    canvas.setFont(&FONT_SMALL); canvas.setCursor(x, y + 14); canvas.print(label);
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(x, y + 36); canvas.print(value);
  };
  String unavailable = "--";
  reading(lx, gy0, "TEMPERATURE", indoor.valid ? String(cToF(indoor.temperature), 1) + " F" : unavailable);
  reading(rx, gy0, "HUMIDITY", indoor.valid ? String(indoor.humidity, 1) + " %" : unavailable);
  reading(lx, gy0 + rowH, "CO2", indoor.valid && indoor.co2 != 0xFFFF ? String(indoor.co2) + " ppm" : unavailable);
  reading(rx, gy0 + rowH, "VOC INDEX", indoor.valid ? String(indoor.vocIndex, 1) : unavailable);
  reading(lx, gy0 + rowH * 2, "NOx INDEX", indoor.valid ? String(indoor.noxIndex, 1) : unavailable);
  reading(rx, gy0 + rowH * 2, "INDOOR AQI", indoor.valid ? String(indoor.particleAqi) : unavailable);
  reading(lx, gy0 + rowH * 3, "PM1.0", indoor.valid ? String(indoor.pm1, 1) + " ug/m3" : unavailable);
  reading(rx, gy0 + rowH * 3, "PM2.5", indoor.valid ? String(indoor.pm25, 1) + " ug/m3" : unavailable);
  reading(lx, gy0 + rowH * 4, "PM4.0", indoor.valid ? String(indoor.pm4, 1) + " ug/m3" : unavailable);
  reading(rx, gy0 + rowH * 4, "PM10", indoor.valid ? String(indoor.pm10, 1) + " ug/m3" : unavailable);

  canvas.drawRect(8, 266, 384, 24, 1);
  canvas.setFont(&FONT_SMALL); canvas.setCursor(14, 283);
  canvas.print("Serial "); canvas.print(indoor.serialNumber[0] ? indoor.serialNumber : "--");
  canvas.setCursor(302, 283);
  if (indoor.valid) { canvas.print((millis() - indoor.lastUpdate) / 1000); canvas.print("s ago"); }
  else canvas.print("NO DATA");
  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 4: HOURLY FORECAST ===== (note: function defined here, called from draw())
void drawHourlyPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 24); canvas.print("NEXT 6 HOURS  "); canvas.print(activeWeatherLocation);

  if (!hourlyData.valid) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(80, 150); canvas.print("NO HOURLY DATA");
    pushCanvasToRLCD(displayInvert); return;
  }

  const int cols = 6, colW = 62, colGap = 2, startX = 11;
  const int topY = 36, hdrH = 24, rowH = 38, totalH = hdrH + rowH * 5;
  const int yTime = topY, yTemp = yTime+hdrH, yRainPc = yTemp+rowH, yRainMM = yRainPc+rowH, yUV = yRainMM+rowH, yWind = yUV+rowH;

  for (int i = 0; i < cols; i++) {
    int cx = startX + i * (colW + colGap);
    canvas.drawRect(cx, topY, colW, totalH, 1);
    canvas.fillRect(cx+1, yTime+1, colW-2, hdrH-2, 1);
    canvas.setTextColor(0); canvas.setFont(&FONT_SMALL);
    canvas.setCursor(cx+5, yTime+17); canvas.print(hourlyData.time[i]);
    canvas.setTextColor(1); canvas.setFont(&FONT_SMALL);
    canvas.setCursor(cx+4, yTemp+13);   canvas.print("TEMP");
    canvas.setCursor(cx+4, yTemp+30);   canvas.print((int)lroundf(cToF(hourlyData.temp[i]))); canvas.print("F");
    canvas.setCursor(cx+4, yRainPc+13); canvas.print("RAIN");
    canvas.setCursor(cx+4, yRainPc+30); canvas.print(hourlyData.rainChance[i]); canvas.print("%");
    canvas.setCursor(cx+4, yRainMM+13); canvas.print("MM");
    canvas.setCursor(cx+4, yRainMM+30); canvas.print(hourlyData.rainMM[i], 1);
    canvas.setCursor(cx+4, yUV+13);     canvas.print("UV");
    canvas.setCursor(cx+4, yUV+30);     canvas.print(hourlyData.uvIndex[i], 1);
    canvas.setCursor(cx+4, yWind+13);   canvas.print("WIND");
    canvas.setCursor(cx+4, yWind+30);   canvas.print((int)lroundf(hourlyData.windSpeed[i] * 0.621371f)); canvas.print("mph");
  }

  // No divider line — footer sits cleanly below the grid
  canvas.setFont(&FONT_SMALL); canvas.setCursor(12, 285);
  canvas.print("Upd ");
  if (weatherData.valid) { canvas.print((millis() - weatherData.lastUpdate) / 60000); canvas.print("m ago"); }
  else { canvas.print("--"); }
  pushCanvasToRLCD(displayInvert);
}

// ===== GRAPH HELPERS =====
GraphBounds calcGraphBounds(float* data, int count, float minRange, float pad) {
  GraphBounds b = { 999.0f, -999.0f, 0.0f };
  for (int i = 0; i < count; i++) { if (data[i] < b.mn) b.mn = data[i]; if (data[i] > b.mx) b.mx = data[i]; }
  b.rng = b.mx - b.mn;
  if (b.rng < minRange) b.rng = minRange;
  b.mn -= pad; b.mx += pad; b.rng = b.mx - b.mn;
  return b;
}

int calcTrend(float* data, int si, int pts) {
  if (pts < 3) return 0;
  float delta = data[(si + pts - 1) % HISTORY_SIZE] - data[(si + pts - 3) % HISTORY_SIZE];
  if (delta > 0.5f) return 1; if (delta < -0.5f) return -1; return 0;
}

void drawTrendArrow(int x, int y, int trend) {
  if (trend == 1)       { canvas.fillTriangle(x, y-8, x-5, y, x+5, y, 1); canvas.fillRect(x-2, y, 4, 5, 1); }
  else if (trend == -1) { canvas.fillTriangle(x, y+8, x-5, y, x+5, y, 1); canvas.fillRect(x-2, y-5, 4, 5, 1); }
  else                  { canvas.fillRect(x-6, y-3, 12, 2, 1); canvas.fillRect(x-6, y+1, 12, 2, 1); }
}

void drawEnhancedGraph(float* data, int si, int pts, int gX, int gY, int gW, int gH, GraphBounds b, const char* unit) {
  canvas.drawRect(gX-1, gY-1, gW+2, gH+2, 1);
  for (int i = 1; i <= 3; i++) canvas.drawLine(gX, gY+(i*gH/4), gX+gW, gY+(i*gH/4), 1);
  for (int i = 0; i <= 4; i++) canvas.drawLine(gX+(i*gW/4), gY, gX+(i*gW/4), gY+gH, 1);

  int minIdx = -1, maxIdx = -1; float minVal = 9999.0f, maxVal = -9999.0f;
  for (int i = 0; i < pts; i++) {
    float v = data[(si+i) % HISTORY_SIZE];
    if (v < minVal) { minVal = v; minIdx = i; }
    if (v > maxVal) { maxVal = v; maxIdx = i; }
  }
  for (int i = 0; i < pts-1; i++) {
    int i1 = (si+i) % HISTORY_SIZE, i2 = (si+i+1) % HISTORY_SIZE;
    int x1 = gX+(i*gW/(HISTORY_SIZE-1)), x2 = gX+((i+1)*gW/(HISTORY_SIZE-1));
    int y1 = gY+gH-(int)((data[i1]-b.mn)/b.rng*gH), y2 = gY+gH-(int)((data[i2]-b.mn)/b.rng*gH);
    canvas.drawLine(x1, y1, x2, y2, 1); canvas.drawLine(x1, y1-1, x2, y2-1, 1);
  }
  if (minIdx >= 0) {
    int mx = gX+(minIdx*gW/(HISTORY_SIZE-1)), my = gY+gH-(int)((minVal-b.mn)/b.rng*gH);
    canvas.fillTriangle(mx, my+2, mx-4, my-5, mx+4, my-5, 1);
    canvas.setFont(&FONT_SMALL); canvas.setCursor(constrain(mx-8,gX,gX+gW-20), my+14); canvas.print(minVal, 1);
  }
  if (maxIdx >= 0) {
    int mx = gX+(maxIdx*gW/(HISTORY_SIZE-1)), my = gY+gH-(int)((maxVal-b.mn)/b.rng*gH);
    canvas.fillTriangle(mx, my-2, mx-4, my+5, mx+4, my+5, 1);
    canvas.setFont(&FONT_SMALL); canvas.setCursor(constrain(mx-8,gX,gX+gW-20), my-5); canvas.print(maxVal, 1);
  }
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(gX-38, gY+5);      canvas.print(b.mx, 0); canvas.print(unit);
  canvas.setCursor(gX-38, gY+gH/2+4); canvas.print(((b.mx+b.mn)/2.0f), 0); canvas.print(unit);
  canvas.setCursor(gX-38, gY+gH-2);   canvas.print(b.mn, 0); canvas.print(unit);
  const char* xLabels[] = { "6h", "4.5h", "3h", "1.5h", "0h" };
  for (int i = 0; i <= 4; i++) { int lx = gX+(i*gW/4)-(i==4?12:6); canvas.setCursor(lx, gY+gH+14); canvas.print(xLabels[i]); }
}

// ===== PAGE 11: TEMP GRAPH =====
void drawTempGraphPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  drawThermometerIcon(16, 14);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(38, 22); canvas.print("INDOOR TEMP");
  canvas.setCursor(38, 38); canvas.print("6 HOUR HISTORY");
  // Current value — FONT_LARGE fits header box without clipping
  canvas.setFont(&FONT_LARGE); canvas.setCursor(220, 38); canvas.print(cToF(temperature), 1);
  canvas.setFont(&FONT_SMALL); canvas.print(" F");
  int tTrend = calcTrend(history.tempHistory, history.currentIndex, history.sampleCount);
  drawTrendArrow(375, 22, tTrend);

  if (history.sampleCount < 2) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(85, 155); canvas.print("COLLECTING DATA");
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(95, 178); canvas.print("Graph available in 15 mins");
    pushCanvasToRLCD(displayInvert); return;
  }

  int gX = 44, gY = 52, gW = 336, gH = 168;
  GraphBounds b = calcGraphBounds(history.tempHistory, HISTORY_SIZE, 5.0f, 1.0f);
  drawEnhancedGraph(history.tempHistory, history.currentIndex, min((int)history.sampleCount,(int)HISTORY_SIZE), gX, gY, gW, gH, b, "F");

  // Bottom stats bar — verified column positions, min 7px between all items
  canvas.drawRect(8, 244, 384, 24, 1);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(8,   261); canvas.print("0h:");
  canvas.setCursor(39,  261); canvas.print(cToF(temperature), 1); canvas.print("F");
  canvas.setCursor(90,  261); canvas.print("MIN:");
  canvas.setCursor(134, 261); canvas.print(b.mn+1.0f, 1); canvas.print("F");
  canvas.setCursor(200, 261); canvas.print("MAX:");
  canvas.setCursor(250, 261); canvas.print(b.mx-1.0f, 1); canvas.print("F");
  canvas.setCursor(308, 261);
  if (tTrend == 1) canvas.print("RISING"); else if (tTrend == -1) canvas.print("FALLING"); else canvas.print("STABLE");
  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 12: HUMIDITY GRAPH =====
void drawHumidityGraphPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  drawDropletIcon(16, 14);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(38, 22); canvas.print("INDOOR HUMIDITY");
  canvas.setCursor(38, 38); canvas.print("6 HOUR HISTORY");
  // Current value — FONT_LARGE fits header box without clipping
  canvas.setFont(&FONT_LARGE); canvas.setCursor(220, 38); canvas.print((int)humidity);
  canvas.setFont(&FONT_SMALL); canvas.print(" %");
  int hTrend = calcTrend(history.humidityHistory, history.currentIndex, history.sampleCount);
  drawTrendArrow(375, 22, hTrend);

  if (history.sampleCount < 2) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(85, 155); canvas.print("COLLECTING DATA");
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(95, 178); canvas.print("Graph available in 15 mins");
    pushCanvasToRLCD(displayInvert); return;
  }

  int gX = 44, gY = 52, gW = 336, gH = 168;
  GraphBounds b = calcGraphBounds(history.humidityHistory, HISTORY_SIZE, 10.0f, 2.0f);
  if (b.mn < 0.0f) b.mn = 0.0f; if (b.mx > 100.0f) b.mx = 100.0f;
  b.rng = b.mx - b.mn; if (b.rng < 1.0f) b.rng = 1.0f;
  drawEnhancedGraph(history.humidityHistory, history.currentIndex, min((int)history.sampleCount,(int)HISTORY_SIZE), gX, gY, gW, gH, b, "%");

  // Bottom stats bar — verified column positions, min 7px between all items
  canvas.drawRect(8, 244, 384, 24, 1);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(8,   261); canvas.print("0h:");
  canvas.setCursor(39,  261); canvas.print((int)humidity); canvas.print("%");
  canvas.setCursor(90,  261); canvas.print("MIN:");
  canvas.setCursor(134, 261); canvas.print(b.mn+2.0f, 1); canvas.print("%");
  canvas.setCursor(200, 261); canvas.print("MAX:");
  canvas.setCursor(250, 261); canvas.print(b.mx-2.0f, 1); canvas.print("%");
  canvas.setCursor(308, 261);
  if (hTrend == 1) canvas.print("RISING"); else if (hTrend == -1) canvas.print("FALLING"); else canvas.print("STABLE");
  pushCanvasToRLCD(displayInvert);
}

// ===== SEASONS HELPER =====
// Northern Hemisphere astronomical seasons.
int dayOfYear(int day, int month, int year) {
  const int dpm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  int doy = 0;
  for (int m = 0; m < month - 1; m++) doy += dpm[m];
  if (leap && month > 2) doy++;
  return doy + day;
}

SeasonEvents calcSeasonEvents(int year) {
  SeasonEvents e;
  float y = year + 0.5f;
  e.marchEq = (int)(79.3125f + 0.2422f * (y - 2000) - (int)((y - 2000) / 4.0f));
  e.juneSol = (int)(171.3125f + 0.2422f * (y - 2000) - (int)((y - 2000) / 4.0f));
  e.septEq  = (int)(264.3125f + 0.2422f * (y - 2000) - (int)((y - 2000) / 4.0f));
  e.decSol  = (int)(354.3125f + 0.2422f * (y - 2000) - (int)((y - 2000) / 4.0f));
  return e;
}

SeasonInfo getSeasonInfo(int doy, int year) {
  SeasonEvents e = calcSeasonEvents(year);
  SeasonInfo s;
  if (doy >= e.decSol || doy < e.marchEq) {
    s.name = "WINTER";
    s.daysSince = doy >= e.decSol ? doy-e.decSol : doy+365-e.decSol;
    s.daysUntil = doy >= e.decSol ? e.marchEq+365-doy : e.marchEq-doy;
    s.nextEvent = "Spring Equinox"; s.nextEventDoy = e.marchEq;
  } else if (doy < e.juneSol) {
    s.name = "SPRING"; s.daysSince = doy-e.marchEq; s.daysUntil = e.juneSol-doy;
    s.nextEvent = "Summer Solstice"; s.nextEventDoy = e.juneSol;
  } else if (doy < e.septEq) {
    s.name = "SUMMER"; s.daysSince = doy-e.juneSol; s.daysUntil = e.septEq-doy;
    s.nextEvent = "Fall Equinox"; s.nextEventDoy = e.septEq;
  } else {
    s.name = "FALL"; s.daysSince = doy-e.septEq; s.daysUntil = e.decSol-doy;
    s.nextEvent = "Winter Solstice"; s.nextEventDoy = e.decSol;
  }
  return s;
}

String doyToDateStr(int doy, int year) {
  const int dpm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const char* mon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  bool leap = (year%4==0 && year%100!=0) || (year%400==0);
  int m=0;
  while (m<12) {
    int days=dpm[m]+((leap&&m==1)?1:0);
    if (doy<=days) break;
    doy-=days; m++;
  }
  return String(doy)+" "+mon[constrain(m,0,11)];
}

// ===== PAGE 9: EARTH & SEASONS =====
void drawSeasonsPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 24); canvas.print("EARTH & SEASONS  "); canvas.print(activeWeatherLocation);

  int day   = rtc.getDay();
  int month = rtc.getMonth();
  int year  = rtc.getYear();
  int doy   = dayOfYear(day, month, year);
  SeasonEvents e = calcSeasonEvents(year);
  SeasonInfo   s = getSeasonInfo(doy, year);

  // ===== ORBIT DIAGRAM — full height, no bottom panel =====
  // Centre at y=158, orx=130, ory=100 — fits within border with clearance for outside labels
  const int ox = 200, oy = 158, orx = 130, ory = 100;

  // Orbit ellipse
  for (int deg = 0; deg < 360; deg += 1) {
    float rad = deg * 3.14159f / 180.0f;
    int x = ox + (int)(orx * cosf(rad));
    int y = oy + (int)(ory * sinf(rad));
    if (x >= 0 && x < W && y >= 0 && y < H) canvas.drawPixel(x, y, 1);
  }

  // Sun at centre
  canvas.fillCircle(ox, oy, 7, 1);
  canvas.fillCircle(ox, oy, 3, 0);
  for (int i = 0; i < 8; i++) {
    float a = i * 3.14159f / 4.0f;
    canvas.drawLine(ox+(int)(9*cosf(a)), oy+(int)(9*sinf(a)),
                    ox+(int)(13*cosf(a)), oy+(int)(13*sinf(a)), 1);
  }
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(0);
  canvas.setCursor(ox-4, oy+4); canvas.print("S");
  canvas.setTextColor(1);

  // Season markers — EQU/SOL + date, mathematically positioned clear of orbit and borders
  struct { int mdoy; const char* lbl; float angle; } markers[] = {
    { e.marchEq, "EQU", 0.0f   },
    { e.juneSol, "SOL", 270.0f },
    { e.septEq,  "EQU", 180.0f },
    { e.decSol,  "SOL", 90.0f  },
  };

  for (int i = 0; i < 4; i++) {
    float rad = markers[i].angle * 3.14159f / 180.0f;
    int mx = ox + (int)(orx * cosf(rad));
    int my = oy + (int)(ory * sinf(rad));
    canvas.fillCircle(mx, my, 4, 1);
    int tx, ty;
    // Right (MAR EQ) — both rows above dot, 8px clear of orbit
    if (markers[i].angle == 0.0f)   { tx = mx + 10; ty = my - 25; }
    // Left (SEP EQ) — pushed fully left of orbit; '22 Sep'=57px wide, mx=70, need tx<5
    if (markers[i].angle == 180.0f) { tx = mx - 68; ty = my - 25; }
    // Top (JUN SOL) — below dot inside ellipse, pushed down clear of orbit
    if (markers[i].angle == 270.0f) { tx = mx - 16; ty = my + 22; }
    // Bottom (DEC SOL) — both rows above dot, 9px clear of orbit
    if (markers[i].angle == 90.0f)  { tx = mx - 16; ty = my - 26; }
    canvas.setFont(&FONT_SMALL);
    canvas.setCursor(tx, ty);    canvas.print(markers[i].lbl);
    canvas.setCursor(tx, ty+14); canvas.print(doyToDateStr(markers[i].mdoy, year));
  }

  // Season names in the 4 corners — fixed positions outside the ellipse
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(14,  52);  canvas.print("SUMMER");  // top-left  (JUN SOL side)
  canvas.setCursor(316, 52);  canvas.print("SPRING");  // top-right (MAR EQ side)
  canvas.setCursor(14,  272); canvas.print("FALL");    // bot-left  (SEP EQ side)
  canvas.setCursor(316, 272); canvas.print("WINTER");  // bot-right (DEC SOL side)

  // Earth position
  float earthAngle;
  if      (doy >= e.decSol)                  earthAngle = 90.0f  - (float)(doy - e.decSol)    / (float)(e.marchEq + 365 - e.decSol) * 90.0f;
  else if (doy < e.marchEq)                  earthAngle = 90.0f  - (float)(doy + 365 - e.decSol) / (float)(e.marchEq + 365 - e.decSol) * 90.0f;
  else if (doy >= e.marchEq && doy < e.juneSol) earthAngle = 360.0f - (float)(doy - e.marchEq) / (float)(e.juneSol - e.marchEq) * 90.0f;
  else if (doy >= e.juneSol && doy < e.septEq)  earthAngle = 270.0f - (float)(doy - e.juneSol) / (float)(e.septEq  - e.juneSol) * 90.0f;
  else                                           earthAngle = 180.0f - (float)(doy - e.septEq)  / (float)(e.decSol  - e.septEq)  * 90.0f;

  float erad = earthAngle * 3.14159f / 180.0f;
  int ex = ox + (int)(orx * cosf(erad));
  int ey = oy + (int)(ory * sinf(erad));
  canvas.fillCircle(ex, ey, 6, 1);
  canvas.fillCircle(ex, ey, 3, 0);
  int elx = ex + (ex > ox ? 9 : -28);
  int ely = ey + (ey > oy ? 14 : -6);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(elx, ely);    canvas.print("NOW");
  canvas.setCursor(elx, ely+12); canvas.print(s.daysUntil); canvas.print("d");

  pushCanvasToRLCD(displayInvert);
}
// ===== PAGE 10: SEASONS ORBIT DIAGRAM =====
void drawSeasonsOrbitPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  int day_  = rtc.getDay();
  int mon_  = rtc.getMonth();
  int year_ = rtc.getYear();
  int doy   = dayOfYear(day_, mon_, year_);
  SeasonEvents e = calcSeasonEvents(year_);
  SeasonInfo   s = getSeasonInfo(doy, year_);

  // Days until each event
  int dJun = e.juneSol - doy; if (dJun <= 0) dJun += 365;
  int dSep = e.septEq  - doy; if (dSep <= 0) dSep += 365;
  int dDec = e.decSol  - doy; if (dDec <= 0) dDec += 365;
  int dMar = e.marchEq - doy; if (dMar <= 0) dMar += 365;
  int minDays = dJun;
  if (dSep < minDays) minDays = dSep;
  if (dDec < minDays) minDays = dDec;
  if (dMar < minDays) minDays = dMar;

  // ── Header ───────────────────────────────────────────────────────────────
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(12, 24);
  canvas.print("EARTH & SEASONS");

  String evtStr = String(s.nextEvent);
  int sp = evtStr.indexOf(' ');
  if (sp > 0) evtStr = evtStr.substring(sp + 1);
  evtStr.replace("Equinox",  "EQ");
  evtStr.replace("Solstice", "SOL");
  String evtFull = evtStr + " " + doyToDateStr(s.nextEventDoy, year_);
  int16_t ex1, ey1; uint16_t etw, eth;
  canvas.getTextBounds(evtFull.c_str(), 0, 0, &ex1, &ey1, &etw, &eth);
  canvas.setCursor(388 - (int)etw, 24);
  canvas.print(evtFull);

  // ── Corner countdown labels ───────────────────────────────────────────────
  canvas.setFont(&FONT_SMALL);
  int cDays[4]  = { dSep, dJun, dDec, dMar }; // TL=SEP EQU, TR=JUN SOL, BL=DEC SOL, BR=MAR EQU
  int cX[4]     = { 10,   390,  10,   390  };
  int cY[4]     = { 52,   52,   261,  261  };
  bool cRight[4]= { false,true, false,true  };
  for (int i = 0; i < 4; i++) {
    String lbl = (cDays[i] == 0) ? String("TODAY") : String(cDays[i]) + "d";
    int16_t bx1, by1; uint16_t btw, bth;
    canvas.getTextBounds(lbl.c_str(), 0, 0, &bx1, &by1, &btw, &bth);
    int tx = cRight[i] ? cX[i] - (int)btw - 4 : cX[i] + 2;
    canvas.setCursor(tx, cY[i]);
    canvas.print(lbl);
    if (cDays[i] == minDays) {
      canvas.drawRect(tx - 4, cY[i] - 16, (int)btw + 8, 23, 1);
    }
  }

  // ── Orbit geometry ────────────────────────────────────────────────────────
  const int ocx = 200, ocy = 158, orx = 130, ory = 100;

  // ── Quadrant fills (scanline) ─────────────────────────────────────────────
  for (int fy = 0; fy < H; fy++) {
    float fdy = fy - ocy;
    float disc = 1.0f - (fdy * fdy) / (float)(ory * ory);
    if (disc < 0.0f) continue;
    float fex = orx * sqrtf(disc);
    for (int fx = ocx - (int)fex; fx <= ocx + (int)fex; fx++) {
      float fdx = fx - ocx;
      float fangle = atan2f(fdy, fdx) * 180.0f / 3.14159f;
      if (fangle < 0.0f) fangle += 360.0f;
      // All 4 quadrants = entire ellipse interior
      canvas.drawPixel(fx, fy, 1);
    }
  }

  // ── Quadrant dividing lines (crosshairs) drawn white to split fills ───────
  // Horizontal line (SEP EQ to MAR EQ)
  for (int fx = ocx - orx + 2; fx <= ocx + orx - 2; fx++)
    canvas.drawPixel(fx, ocy, 0);
  // Vertical line (JUN SOL to DEC SOL)
  for (int fy = ocy - ory + 2; fy <= ocy + ory - 2; fy++)
    canvas.drawPixel(ocx, fy, 0);

  // ── Dashed white crosshairs over the solid white lines ────────────────────
  for (int fx = ocx - orx + 2; fx < ocx + orx - 2; fx += 9) {
    for (int k = 0; k < 5 && fx+k < ocx+orx-2; k++)
      canvas.drawPixel(fx+k, ocy, 0);
  }
  for (int fy = ocy - ory + 2; fy < ocy + ory - 2; fy += 9) {
    for (int k = 0; k < 5 && fy+k < ocy+ory-2; k++)
      canvas.drawPixel(ocx, fy+k, 0);
  }

  // ── Ellipse outline: white gap then black ─────────────────────────────────
  for (int deg = 0; deg < 360; deg++) {
    float rd = deg * 3.14159f / 180.0f;
    canvas.drawPixel(ocx + (int)(orx * cosf(rd)), ocy + (int)(ory * sinf(rd)), 0);
  }
  for (int deg = 0; deg < 360; deg++) {
    float rd = deg * 3.14159f / 180.0f;
    canvas.drawPixel(ocx + (int)(orx * cosf(rd)), ocy + (int)(ory * sinf(rd)), 1);
  }

  // ── Earth angle ───────────────────────────────────────────────────────────
  float earthAngle;
  if      (doy >= e.decSol)                        earthAngle = 90.0f  - (float)(doy - e.decSol)       / (float)(e.marchEq + 365 - e.decSol) * 90.0f;
  else if (doy < e.marchEq)                        earthAngle = 90.0f  - (float)(doy + 365 - e.decSol) / (float)(e.marchEq + 365 - e.decSol) * 90.0f;
  else if (doy >= e.marchEq && doy < e.juneSol)    earthAngle = 360.0f - (float)(doy - e.marchEq)      / (float)(e.juneSol - e.marchEq)      * 90.0f;
  else if (doy >= e.juneSol && doy < e.septEq)     earthAngle = 270.0f - (float)(doy - e.juneSol)      / (float)(e.septEq  - e.juneSol)      * 90.0f;
  else                                              earthAngle = 180.0f - (float)(doy - e.septEq)       / (float)(e.decSol  - e.septEq)       * 90.0f;

  // ── Progress arc (white, inside orbit at 88%) ─────────────────────────────
  float nextAngle;
  if      (dMar == minDays) nextAngle = 0.0f;
  else if (dDec == minDays) nextAngle = 90.0f;
  else if (dSep == minDays) nextAngle = 180.0f;
  else                      nextAngle = 270.0f;

  float arcRX = orx * 0.88f, arcRY = ory * 0.88f;
  float sweepEnd = nextAngle;
  if (sweepEnd > earthAngle) sweepEnd -= 360.0f;
  for (float aa = earthAngle; aa >= sweepEnd; aa -= 0.5f) {
    float rd = aa * 3.14159f / 180.0f;
    int apx = ocx + (int)(arcRX * cosf(rd));
    int apy = ocy + (int)(arcRY * sinf(rd));
    canvas.drawPixel(apx,   apy,   0);
    canvas.drawPixel(apx,   apy-1, 0);
    canvas.drawPixel(apx,   apy+1, 0);
  }

  // ── Sun at centre ─────────────────────────────────────────────────────────
  canvas.fillCircle(ocx, ocy, 16, 0);
  canvas.fillCircle(ocx, ocy, 9,  1);
  canvas.fillCircle(ocx, ocy, 5,  0);
  for (int i = 0; i < 8; i++) {
    float ra = i * 3.14159f / 4.0f;
    canvas.drawLine(ocx+(int)(11*cosf(ra)), ocy+(int)(11*sinf(ra)),
                    ocx+(int)(15*cosf(ra)), ocy+(int)(15*sinf(ra)), 1);
  }
  canvas.setTextColor(0); canvas.setCursor(ocx-4, ocy+4); canvas.print("S");
  canvas.setTextColor(1);

  // ── Season names in white ─────────────────────────────────────────────────
  const char* sNames[] = { "WINTER", "FALL", "SUMMER", "SPRING" };
  float       sAngles[] = { 45.0f, 135.0f, 225.0f, 315.0f };
  canvas.setFont(&FONT_SMALL);
  for (int i = 0; i < 4; i++) {
    float rd = sAngles[i] * 3.14159f / 180.0f;
    int snx = ocx + (int)(orx * 0.56f * cosf(rd));
    int sny = ocy + (int)(ory * 0.56f * sinf(rd));
    int16_t sx1, sy1; uint16_t stw, sth;
    canvas.getTextBounds(sNames[i], 0, 0, &sx1, &sy1, &stw, &sth);
    canvas.setTextColor(0);
    canvas.setCursor(snx - (int)stw/2, sny + 4);
    canvas.print(sNames[i]);
    canvas.setTextColor(1);
  }

  // ── Month ticks + labels ──────────────────────────────────────────────────
  const char* months[] = {"Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec","Jan","Feb"};
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  for (int i = 0; i < 12; i++) {
    float mrad = -i * 30.0f * 3.14159f / 180.0f;
    int mix = ocx + (int)(orx * cosf(mrad));
    int miy = ocy + (int)(ory * sinf(mrad));
    int mtx = ocx + (int)((orx+9) * cosf(mrad));
    int mty = ocy + (int)((ory+9) * sinf(mrad));
    canvas.drawLine(mix, miy, mtx, mty, 1);
    float mlr  = (i == 0 || i == 6) ? orx + 30.0f : orx + 20.0f;
    float mlry = (i == 0 || i == 6) ? ory + 30.0f : ory + 20.0f;
    int mlx = ocx + (int)(mlr  * cosf(mrad));
    int mly = ocy + (int)(mlry * sinf(mrad));
    int16_t mx1, my1; uint16_t mtw, mth;
    canvas.getTextBounds(months[i], 0, 0, &mx1, &my1, &mtw, &mth);
    canvas.setCursor(mlx - (int)mtw/2, mly + 4);
    canvas.print(months[i]);
  }

  // ── Bullseye markers at SOL/EQ points ────────────────────────────────────
  int bullA[] = {0, 90, 180, 270};
  for (int i = 0; i < 4; i++) {
    float rd = bullA[i] * 3.14159f / 180.0f;
    int bpx = ocx + (int)(orx * cosf(rd));
    int bpy = ocy + (int)(ory * sinf(rd));
    canvas.fillCircle(bpx, bpy, 8, 0);
    canvas.drawCircle(bpx, bpy, 6, 1);
    canvas.drawCircle(bpx, bpy, 5, 1);
    canvas.fillCircle(bpx, bpy, 4, 0);
    canvas.fillCircle(bpx, bpy, 2, 1);
  }

  // ── Earth dot — flashing ─────────────────────────────────────────────────
  unsigned long nowMs = millis();
  if (nowMs - earthFlashLast >= 600) {
    earthFlashOn   = !earthFlashOn;
    earthFlashLast = nowMs;
  }
  if (earthFlashOn) {
    float erd = earthAngle * 3.14159f / 180.0f;
    int epx = ocx + (int)(orx * cosf(erd));
    int epy = ocy + (int)(ory * sinf(erd));
    canvas.fillCircle(epx, epy, 8, 0);
    canvas.drawCircle(epx, epy, 8, 1);
    canvas.fillCircle(epx, epy, 5, 1);
    canvas.fillCircle(epx, epy, 3, 0);
  }

  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 20: TIMERS / STOPWATCH / ALARMS =====
void drawTimersPage() {
  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W-2, H-2, 1);

  // Header bar
  canvas.fillRect(3, 3, 394, 20, 1);
  canvas.setTextColor(0);
  canvas.setFont(&FONT_SMALL);
  printCentered(&FONT_SMALL, 17, "TIMERS & ALARMS");
  canvas.setTextColor(1);

  // ── Top half: Timer or Stopwatch display ─────────────────────────────────
  bool showTimer = timerState.running || timerState.expired ||
                   timerState.remainingSecs != timerState.durationSecs;
  bool showSw    = swState.running || swState.elapsed > 0;
  bool isSnoozed = (alarmSnoozeUntil > 0 && millis() < alarmSnoozeUntil && alarmSnoozeIdx >= 0);

  // Mode label
  canvas.setFont(&FONT_SMALL);
  if (timerFiring) {
    printCentered(&FONT_SMALL, 42, "TIMER EXPIRED");
  } else if (alarmFiring && alarmFiringIdx >= 0) {
    printCentered(&FONT_SMALL, 42, alarms[alarmFiringIdx].label[0] ?
                  alarms[alarmFiringIdx].label : "ALARM");
  } else if (isSnoozed) {
    // Combine label + SNOOZED on one line e.g. "hello — SNOOZED"
    char snoozeLbl[32];
    const char* lbl = alarms[alarmSnoozeIdx].label[0] ? alarms[alarmSnoozeIdx].label : "ALARM";
    snprintf(snoozeLbl, sizeof(snoozeLbl), "%s \xe2\x80\x94 SNOOZED", lbl);
    printCentered(&FONT_SMALL, 42, snoozeLbl);
  } else if (timerState.running) {
    printCentered(&FONT_SMALL, 42, timerState.name[0] ? timerState.name : "TIMER");
  } else if (swState.running || swState.elapsed > 0) {
    printCentered(&FONT_SMALL, 42, "STOPWATCH");
  } else {
    printCentered(&FONT_SMALL, 42, "NO ACTIVE TIMER");
  }

  // Large DSEG7 time display
  char timeBuf[9];
  if (isSnoozed) {
    // Show snooze countdown in DSEG7
    unsigned long secsLeft = (alarmSnoozeUntil - millis()) / 1000UL;
    unsigned long sm = secsLeft / 60;
    unsigned long ss = secsLeft % 60;
    snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", sm, ss);
  } else if (timerState.running || timerFiring || (showTimer && !showSw)) {
    uint32_t s = timerState.remainingSecs;
    uint32_t h = s / 3600; s %= 3600;
    uint32_t m = s / 60;   s %= 60;
    if (h > 0)
      snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    else
      snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
  } else {
    unsigned long elMs = swElapsedMs();
    unsigned long elS  = elMs / 1000;
    unsigned long elH  = elS / 3600; elS %= 3600;
    unsigned long elM  = elS / 60;   elS %= 60;
    if (elH > 0)
      snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu:%02lu", elH, elM, elS);
    else
      snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu", elM, elS);
  }

  // DSEG7 84pt for MM:SS, 36pt for HH:MM:SS — centred in the top zone
  bool longFmt = (strlen(timeBuf) > 5);
  if (longFmt) {
    canvas.setFont(&DSEG7_Classic_Bold_36);
    int16_t tx1, ty1; uint16_t tw, th;
    canvas.getTextBounds(timeBuf, 0, 115, &tx1, &ty1, &tw, &th);
    canvas.setCursor((W - tw) / 2 - tx1, 115);
  } else {
    canvas.setFont(&DSEG7_Classic_Bold_84);
    int16_t tx1, ty1; uint16_t tw, th;
    canvas.getTextBounds(timeBuf, 0, 140, &tx1, &ty1, &tw, &th);
    canvas.setCursor((W - tw) / 2 - tx1, 140);
  }
  canvas.print(timeBuf);

  // Divider
  canvas.fillRect(8, 158, 384, 2, 1);

  // ── Bottom half: Alarm list ───────────────────────────────────────────────
  // 3 alarms at 44px spacing → rows at y=185, 229, 273
  const char* dayChars = "MTWTFSS";
  int alarmY = 185;
  for (int i = 0; i < ALARM_COUNT; i++) {
    // Bell icon
    int bx = 12, by = alarmY - 11;
    canvas.drawCircle(bx+4, by+8, 5, 1);
    canvas.fillRect(bx+1, by+4, 7, 5, 1);
    canvas.fillRect(bx+2, by+12, 5, 2, 1);
    canvas.drawPixel(bx+4, by+1, 1);
    if (alarms[i].enabled) canvas.fillCircle(bx+4, by+8, 3, 1);

    // Time
    char alBuf[6];
    snprintf(alBuf, sizeof(alBuf), "%02d:%02d", alarms[i].hour, alarms[i].minute);
    canvas.setFont(&FONT_MEDIUM);
    canvas.setCursor(28, alarmY);
    canvas.print(alBuf);

    // Label
    canvas.setFont(&FONT_SMALL);
    canvas.setCursor(90, alarmY);
    if (alarms[i].label[0]) canvas.print(alarms[i].label);
    else                     canvas.print("--");

    // Day mask / one-shot
    canvas.setFont(&FONT_SMALL);
    int dx = 230;
    if (alarms[i].dayMask == 0x00) {
      canvas.setCursor(dx, alarmY); canvas.print("one-shot");
    } else {
      for (int d = 0; d < 7; d++) {
        if (alarms[i].dayMask & (1 << d)) {
          char dc[2] = {dayChars[d], '\0'};
          canvas.setCursor(dx + d*22, alarmY); canvas.print(dc);
        }
      }
    }

    // Enabled checkbox
    canvas.drawRect(381, alarmY-11, 12, 12, 1);
    if (alarms[i].enabled) canvas.fillRect(383, alarmY-9, 8, 8, 1);

    alarmY += 44;
  }

  pushCanvasToRLCD(displayInvert);
}

void draw() {
  switch (currentPage) {
    case 0:  drawDashboardPage();       break;
    case 1:  drawAnalogClockPage();     break;
    case 2:  drawTimeZonePage();        break;
    case 3:  drawCurrentWeatherPage();  break;
    case 4:  drawHourlyPage();          break;
    case 5:  drawForecastPage();        break;
    case 6:  drawSeasonsPage();         break;
    case 7:  drawSeasonsOrbitPage();    break;
    case 8:  drawTempGraphPage();       break;
    case 9:  drawHumidityGraphPage();   break;
    case 10: drawSystemPage();           break;
    case 11: drawSen66DetailsPage();     break;
    case 12: drawTimersPage();           break;
  }
}

// ===== WIFI & NTP =====
void connectWiFi() {
  Serial.print("Connecting: "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); Serial.print("."); attempts++; }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK"); Serial.println(WiFi.localIP());
    wifiConnected = true; wifiRSSI = WiFi.RSSI();
  } else { Serial.println("\nWiFi failed"); wifiConnected = false; }
}

void syncRTCWithNTP() {
  if (!wifiConnected) return;
  // Use POSIX TZ string — ESP32 handles DST transitions automatically.
  // Pass 0,0 for gmtOffset/daylightOffset; the TZ string contains all the rules.
  configTime(0, 0, ntpServer);
  setenv("TZ", posixTZ, 1);
  tzset();

  struct tm timeinfo; int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 10) { delay(500); retries++; }
  if (getLocalTime(&timeinfo)) {
    // Push correct local time (already DST-adjusted by the TZ rule) to RTC
    rtc.setTime(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    rtc.setDate(timeinfo.tm_wday, timeinfo.tm_mday, timeinfo.tm_mon+1, timeinfo.tm_year+1900);
    ntpLastSync = millis();

    // Derive current UTC offset by comparing local epoch to UTC directly.
    // Use gmtime_r to get UTC fields, then compute the difference in minutes
    // by comparing hours/minutes — avoids mktime() double-conversion issue on newlib.
    time_t localEpoch = time(nullptr);
    struct tm utcCheck;
    gmtime_r(&localEpoch, &utcCheck);
    int localMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int utcMinutes   = utcCheck.tm_hour  * 60 + utcCheck.tm_min;
    int diffMins     = localMinutes - utcMinutes;
    // Handle day boundary wrap
    if (diffMins >  720) diffMins -= 1440;
    if (diffMins < -720) diffMins += 1440;
    gmtOffset_sec = (long)(diffMins * 60);

    Serial.print("RTC synced! UTC offset: ");
    Serial.print(gmtOffset_sec / 3600);
    Serial.print("h, DST: ");
    Serial.println(timeinfo.tm_isdst ? "YES" : "NO");
  } else Serial.println("NTP failed");
}

// ===== WEB UI =====
// Served at http://<device-ip>/ — no login required.
// NVS namespace "dash" also stores the runtime weather location.

void nvsSave() {
  prefs.begin("dash", false);
  prefs.putBool("disp_inv",  displayInvert);
  prefs.putBool("beep_en",   beepEnabled);
  prefs.putBool("alarm_audio", alarmAudioEnabled);
  prefs.putInt ("beep_vol",  beepVolume);
  prefs.putBool("cycle_en",  autoCycleEnabled);
  prefs.putInt ("cycle_sec", autoCycleSeconds);
  prefs.putUShort("page_mask", pageEnabledMask);
  prefs.putBool("sleep_en",   sleepEnabled);
  prefs.putInt ("sleep_from", sleepFromHour);
  prefs.putInt ("sleep_to",   sleepToHour);
  prefs.putString("weather_loc", activeWeatherLocation);
  // Alarms
  for (int i = 0; i < ALARM_COUNT; i++) {
    char k[16];
    snprintf(k, sizeof(k), "al%d_h",   i); prefs.putUChar(k, alarms[i].hour);
    snprintf(k, sizeof(k), "al%d_m",   i); prefs.putUChar(k, alarms[i].minute);
    snprintf(k, sizeof(k), "al%d_lbl", i); prefs.putString(k, alarms[i].label);
    snprintf(k, sizeof(k), "al%d_day", i); prefs.putUChar(k, alarms[i].dayMask);
    snprintf(k, sizeof(k), "al%d_en",  i); prefs.putBool(k, alarms[i].enabled);
  }
  // Timer definition
  prefs.putString("tmr_name", timerState.name);
  prefs.putUInt  ("tmr_dur",  timerState.durationSecs);
  prefs.end();
}

void nvsLoad() {
  prefs.begin("dash", true);
  displayInvert    = prefs.getBool("disp_inv",  false);
  beepEnabled      = prefs.getBool("beep_en",   true);
  alarmAudioEnabled = prefs.getBool("alarm_audio", true);
  beepVolume       = prefs.getInt ("beep_vol",  50);
  autoCycleEnabled = prefs.getBool("cycle_en",  false);
  autoCycleSeconds = prefs.getInt ("cycle_sec", 10);
  pageEnabledMask = prefs.getUShort("page_mask", ALL_PAGE_MASK) & ALL_PAGE_MASK;
  if (pageEnabledMask == 0) pageEnabledMask = ALL_PAGE_MASK;
  sleepEnabled  = prefs.getBool("sleep_en",   false);
  sleepFromHour = prefs.getInt ("sleep_from", 23);
  sleepToHour   = prefs.getInt ("sleep_to",   6);
  String savedWeatherLocation = prefs.getString("weather_loc", weatherLocation);
  savedWeatherLocation.toCharArray(activeWeatherLocation, sizeof(activeWeatherLocation));
  // Alarms — defaults: disabled, 07:00, one-shot, blank label
  for (int i = 0; i < ALARM_COUNT; i++) {
    char k[16];
    snprintf(k, sizeof(k), "al%d_h",   i); alarms[i].hour    = prefs.getUChar(k, 7);
    snprintf(k, sizeof(k), "al%d_m",   i); alarms[i].minute  = prefs.getUChar(k, 0);
    snprintf(k, sizeof(k), "al%d_lbl", i);
    String lbl = prefs.getString(k, "");
    strncpy(alarms[i].label, lbl.c_str(), sizeof(alarms[i].label)-1);
    alarms[i].label[sizeof(alarms[i].label)-1] = '\0';
    snprintf(k, sizeof(k), "al%d_day", i); alarms[i].dayMask = prefs.getUChar(k, 0x00);  // one-shot
    snprintf(k, sizeof(k), "al%d_en",  i); alarms[i].enabled = prefs.getBool(k, false);
  }
  // Timer
  String tn = prefs.getString("tmr_name", "Timer");
  strncpy(timerState.name, tn.c_str(), sizeof(timerState.name)-1);
  timerState.name[sizeof(timerState.name)-1] = '\0';
  timerState.durationSecs  = prefs.getUInt("tmr_dur", 300);  // default 5 min
  timerState.remainingSecs = timerState.durationSecs;
  timerState.running = false;
  timerState.expired = false;
  timerState.lastTick = 0;
  swState.running = false;
  swState.elapsed = 0;
  swState.startMs = 0;
  prefs.end();
}

// Minimal CSS shared across all pages
static const char* kCSS =
  "<style>"
  ":root{--bg:#0b1119;--panel:#101a27;--acc:#39d98a;--txt:#e7eef7;--muted:#93a4b8;--brd:#2a3b50;}"
  "body{margin:0;background:var(--bg);color:var(--txt);font-family:Consolas,Monaco,monospace;}"
  ".w{max-width:820px;margin:24px auto;padding:0 14px;}"
  ".card{background:var(--panel);border:1px solid var(--brd);border-radius:12px;padding:18px;margin-bottom:16px;}"
  ".tabs{display:flex;gap:4px;margin-bottom:16px;}"
  ".tab{padding:8px 18px;border-radius:8px 8px 0 0;background:var(--panel);border:1px solid var(--brd);border-bottom:0;cursor:pointer;color:var(--muted);font-size:13px;text-decoration:none;}"
  ".tab.active{background:#1a2e47;color:var(--txt);border-color:#4f7aa6;}"
  ".tabpane{display:none;}.tabpane.active{display:block;}"
  "h1{font-size:20px;color:var(--acc);margin:0 0 14px 0;}"
  "h2{font-size:14px;color:var(--muted);margin:0 0 10px 0;text-transform:uppercase;letter-spacing:1px;}"
  ".row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:10px;}"
  ".kv{flex:1;min-width:140px;}"
  ".k{color:var(--muted);font-size:12px;}"
  ".v{font-size:15px;font-weight:700;}"
  ".btn{background:var(--acc);color:#062013;border:0;padding:9px 16px;border-radius:8px;"
       "font-weight:700;cursor:pointer;text-decoration:none;display:inline-block;font-size:14px;}"
  ".btn.sec{background:#1e3a52;color:var(--txt);}"
  ".btn.red{background:#c0392b;color:#fff;}"
  ".pgrid{display:grid;grid-template-columns:1fr;gap:8px;margin-top:8px;}"
  ".prow{display:grid;grid-template-columns:auto minmax(0,1fr);gap:10px;align-items:center;}"
  ".pcheck{width:18px;height:18px;accent-color:var(--acc);cursor:pointer;}"
  ".pb{background:#182637;color:#d8e6f7;border:1px solid #36506c;padding:9px 10px;"
      "border-radius:8px;text-decoration:none;text-align:center;font-weight:700;cursor:pointer;border:0;width:100%;}"
  ".pb.active{background:#274161;border:1px solid #4f7aa6;color:#fff;}"
  ".sw{display:flex;align-items:center;justify-content:space-between;background:#0f1b2a;"
      "border:1px solid #2c425f;border-radius:10px;padding:10px 14px;margin-bottom:8px;}"
  ".swlbl{font-size:14px;}"
  ".switch{position:relative;display:inline-block;width:46px;height:24px;flex:0 0 auto;}"
  ".switch input{opacity:0;width:0;height:0;}"
  ".slider{position:absolute;cursor:pointer;inset:0;background:#2a3f57;transition:.2s;border-radius:24px;}"
  ".slider:before{position:absolute;content:'';height:18px;width:18px;left:3px;top:3px;"
                 "background:#d6e6f7;transition:.2s;border-radius:50%;}"
  ".switch input:checked+.slider{background:var(--acc);}"
  ".switch input:checked+.slider:before{transform:translateX(22px);background:#062013;}"
  ".graph{width:100%;height:80px;display:block;margin-top:6px;}"
  "input[type=range]{width:100%;accent-color:var(--acc);}"
  "input[type=number]{background:#0b1421;color:var(--txt);border:1px solid #2c425f;"
                     "border-radius:6px;padding:6px 8px;width:70px;}"
  "input[type=text]{background:#0b1421;color:var(--txt);border:1px solid #2c425f;"
                   "border-radius:6px;padding:9px 10px;min-width:220px;}"
  "@media(max-width:600px){.pgrid{grid-template-columns:1fr;}}"
  "</style>";

// Helper: send a redirect back to /
void webRedirect() {
  webServer.sendHeader("Location", "/");
  webServer.send(303, "text/plain", "");
}

// Build page-selector button grid (called from root handler)
String buildPageGrid() {
  static const char* names[totalPages] = {
    "Indoor Air","Analog Clock","North America Time Zones","Outdoor Conditions",
    "Hourly Forecast","3-Day Forecast","Seasons","Season Orbit",
    "Temperature History","Humidity History","System Info","SEN66 Details","Timers"
  };
  String out = "<div class='pgrid'>";
  for (int i = 0; i < totalPages; i++) {
    out += "<div class='prow'><input class='pcheck' type='checkbox' id='page_en_" + String(i) +
           "' title='Include in LCD navigation'";
    if (pageIsEnabled(i)) out += " checked";
    out += " onchange='setPageEnabled(" + String(i) + ",this)'>";
    out += "<button class='pb' onclick='setPage(" + String(i) + ")'>" +
           String(i) + ": " + names[i] + "</button></div>";
  }
  return out + "</div>";
}

String buildSparkline(float* data, int si, int count, float mn, float mx, const char* colour, const char* unit) {
  if (count < 2) return "<svg class='graph' viewBox='0 0 400 80' xmlns='http://www.w3.org/2000/svg'>"
    "<text x='10' y='44' fill='#93a4b8' font-size='13' font-family='monospace'>Not enough data yet — samples log every 15 min</text></svg>";
  float rng = mx - mn; if (rng < 0.1f) rng = 0.1f;
  const float lm = 38.0f, w = 358.0f;
  String s = "<svg class='graph' viewBox='0 0 400 80' preserveAspectRatio='none' xmlns='http://www.w3.org/2000/svg'>";
  char buf[12];
  snprintf(buf, sizeof(buf), "%.1f%s", mx, unit);
  s += "<text x='0' y='10' fill='#93a4b8' font-size='10' font-family='monospace'>" + String(buf) + "</text>";
  snprintf(buf, sizeof(buf), "%.1f%s", mn, unit);
  s += "<text x='0' y='78' fill='#93a4b8' font-size='10' font-family='monospace'>" + String(buf) + "</text>";
  float mid = (mx + mn) / 2.0f;
  snprintf(buf, sizeof(buf), "%.1f", mid);
  s += "<line x1='" + String(lm,0) + "' y1='40' x2='398' y2='40' stroke='#1e3a52' stroke-width='1'/>";
  s += "<text x='0' y='43' fill='#555e6e' font-size='9' font-family='monospace'>" + String(buf) + "</text>";
  s += "<polyline fill='none' stroke='" + String(colour) + "' stroke-width='2' points='";
  for (int i = 0; i < count; i++) {
    int idx = (si - count + 1 + i + HISTORY_SIZE) % HISTORY_SIZE;
    float x = lm + (float)i / (count - 1) * w;
    float y = 74.0f - ((data[idx] - mn) / rng) * 68.0f;
    s += String(x, 1) + "," + String(y, 1) + " ";
  }
  s += "'/></svg>";
  return s;
}

// Builds the Display Sleep Schedule section for the Settings card.
// Pre-built as a function to avoid fragile mid-chain String concatenation.
String buildSleepSection() {
  String s = "<div style='margin-top:14px;border-top:1px solid var(--brd);padding-top:14px;'>";
  s += "<h2 style='margin-bottom:10px;'>Display Sleep Schedule</h2>";

  if (isDisplaySleeping()) {
    s += "<div style='background:#0e2218;border:1px solid #39d98a;border-radius:8px;"
         "padding:10px 14px;margin-bottom:10px;display:flex;align-items:center;"
         "justify-content:space-between;'>"
         "<span style='color:#39d98a;font-size:13px;'>&#128274; Display is sleeping</span>"
         "<button class='btn' onclick=\"doAction('/wakenow','Waking...','Awake for 10 s')\">Wake Now</button>"
         "</div>";
  } else {
    s += "<div style='font-size:12px;color:var(--muted);margin-bottom:8px;'>Display is currently awake.</div>";
  }

  s += "<div class='sw'><span class='swlbl'>Enable sleep schedule</span>"
       "<label class='switch'><input type='checkbox' id='sw_sleep'";
  if (sleepEnabled) s += " checked";
  s += " onchange=\"setSleep()\"><span class='slider'></span></label></div>";

  // Two selects on their own row — built separately to avoid concat issues
  s += "<div style='margin-top:10px;display:flex;align-items:center;gap:12px;flex-wrap:wrap;'>";
  s += "<span style='color:var(--muted);font-size:12px;white-space:nowrap;'>Sleep from:</span>";
  s += "<select id='sl_from' style='background:#0b1421;color:var(--txt);border:1px solid #2c425f;"
       "border-radius:6px;padding:6px 8px;'>";
  s += buildHourOptions(sleepFromHour);
  s += "</select>";
  s += "<span style='color:var(--muted);font-size:12px;white-space:nowrap;'>Wake at:</span>";
  s += "<select id='sl_to' style='background:#0b1421;color:var(--txt);border:1px solid #2c425f;"
       "border-radius:6px;padding:6px 8px;'>";
  s += buildHourOptions(sleepToHour);
  s += "</select>";
  s += "<button class='btn sec' onclick=\"setSleep()\">Set</button>";
  s += "</div>";  // end row

  s += "</div>";  // end sleep sub-section
  s += "</div>";  // end Settings card
  return s;
}

// GET / — main dashboard page
// Shared navigation for the embedded web UI.
String webTabs(const char* active) {
  String s = "<div class='tabs'>";
  s += "<a class='tab" + String(strcmp(active,"control")==0?" active":"") + "' href='/'>Control</a>";
  s += "<a class='tab" + String(strcmp(active,"weather")==0?" active":"") + "' href='/weather'>Indoor &amp; Weather</a>";
  s += "<a class='tab" + String(strcmp(active,"seasons")==0?" active":"") + "' href='/seasons'>Seasons</a>";
  s += "<a class='tab' href='/timers'>Timers &amp; Alarms</a></div>";
  return s;
}

String metric(const char* label, const String& value) {
  return "<div class='kv'><div class='k'>" + String(label) + "</div><div class='v'>" + value + "</div></div>";
}

// GET / â€” control and live indoor summary
void handleRoot() {
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Indoor Weather Station</title>" + String(kCSS) + "</head><body><div class='w'>" +
    webTabs("control") +
    "<div class='card'><h1>SEN66 Indoor Weather Station</h1><div class='row'>" +
    metric("Temperature", indoor.valid ? String(indoor.temperature*9.0f/5.0f+32.0f,1)+" &deg;F" : "--") +
    metric("Humidity", indoor.valid ? String(indoor.humidity,1)+"%" : "--") +
    metric("CO2", indoor.valid && indoor.co2!=0xFFFF ? String(indoor.co2)+" ppm" : "--") +
    metric("VOC Index", indoor.valid ? String(indoor.vocIndex,0) : "--") +
    metric("Indoor particle AQI", indoor.valid ? String(indoor.particleAqi)+" - "+aqiCategory(indoor.particleAqi) : "--") +
    metric("Outdoor PM2.5 AQI", weatherData.valid ? String(weatherData.pmAqi)+" - "+aqiCategory(weatherData.pmAqi) : "--") +
    "</div></div>"
    "<div class='card'><h2>Particle detail</h2><div class='row'>" +
    metric("Indoor PM2.5", indoor.valid ? String(indoor.pm25,1)+" ug/m3" : "--") +
    metric("Indoor PM10", indoor.valid ? String(indoor.pm10,1)+" ug/m3" : "--") +
    metric("Indoor particle AQI", indoor.valid ? String(indoor.particleAqi) : "--") +
    metric("Outdoor PM2.5 AQI", weatherData.valid ? String(weatherData.pmAqi) : "--") +
    "</div></div>"
    "<div class='card'><h2>Actions</h2><div class='row'>"
    "<button class='btn' onclick=\"fetch('/refresh').then(()=>location.reload())\">Refresh Weather</button>"
    "<button class='btn sec' onclick=\"fetch('/syncntp').then(()=>location.reload())\">Sync NTP</button>"
    "<a class='btn sec' href='/timers'>Timers</a></div></div>"
    "<div class='card'><h2>Weather Location</h2>"
    "<div class='k'>Enter decimal latitude and longitude, for example 40.7128,-74.0060.</div>"
    "<div class='row' style='margin-top:10px'>"
    "<input type='text' id='weather_location' maxlength='63' value='" + String(activeWeatherLocation) + "'>"
    "<button class='btn' onclick='setWeatherLocation()'>Update Location</button></div>"
    "<div id='weather_location_status' class='k'></div></div>"
    "<div class='card'><h2>Navigation Audio</h2>"
    "<div class='sw'><span class='swlbl'>Sound beeps when changing LCD pages</span>"
    "<label class='switch'><input type='checkbox' id='navigation_audio'" +
    String(beepEnabled ? " checked" : "") +
    " onchange='setNavigationAudio(this.checked)'><span class='slider'></span></label></div>"
    "<div id='navigation_audio_status' class='k'>Alarm audio is controlled separately on the Timers &amp; Alarms page.</div></div>"
    "<div class='card'><h2>Automatic Page Cycling</h2>"
    "<div class='sw'><span class='swlbl'>Cycle through LCD pages</span>"
    "<label class='switch'><input type='checkbox' id='cycle_enabled'" + String(autoCycleEnabled ? " checked" : "") +
    " onchange='setCycle(this.checked)'><span class='slider'></span></label></div>"
    "<div class='row' style='margin-top:10px'>"
    "<label class='k' for='cycle_seconds'>Seconds per page</label>"
    "<input type='number' id='cycle_seconds' min='3' max='300' value='" + String(autoCycleSeconds) + "'>"
    "<button class='btn sec' onclick='setCycleSeconds()'>Apply</button></div>"
    "<div id='cycle_status' class='k'></div></div>"
    "<div class='card'><h2>Select LCD Page</h2>"
    "<div class='k'>Checked pages are included when using the LCD buttons or automatic cycling. Click a page name to preview it directly.</div>" +
    buildPageGrid() + "<div id='page_status' class='k' style='margin-top:8px'></div></div>"
    "<script>"
    "function setPage(p){fetch('/setpage?p='+p).then(()=>location.reload())}"
    "function setNavigationAudio(v){fetch('/setbeep?v='+v).then(()=>{"
    "document.getElementById('navigation_audio_status').textContent=v?'Page-change audio enabled':'Page-change audio disabled';"
    "})}"
    "function setPageEnabled(p,el){fetch('/setpageenabled?p='+p+'&v='+el.checked).then(async r=>{"
    "let t=await r.text();if(!r.ok){el.checked=!el.checked;throw new Error(t)}"
    "document.getElementById('page_status').textContent=el.checked?'Page included':'Page skipped';"
    "}).catch(e=>document.getElementById('page_status').textContent=e.message)}"
    "function cycleMessage(m){document.getElementById('cycle_status').textContent=m}"
    "function setCycle(v){fetch('/setcycle?v='+v).then(()=>cycleMessage(v?'Page cycling enabled':'Page cycling disabled'))}"
    "function setCycleSeconds(){let v=document.getElementById('cycle_seconds').value;"
    "fetch('/setcyclesec?v='+v).then(()=>cycleMessage('Cycle time set to '+v+' seconds'))}"
    "function setWeatherLocation(){let v=document.getElementById('weather_location').value;"
    "let s=document.getElementById('weather_location_status');s.textContent='Updating weather...';"
    "fetch('/setweatherlocation?v='+encodeURIComponent(v)).then(async r=>{let t=await r.text();"
    "if(!r.ok)throw new Error(t);s.textContent='Location saved. Fetching new weather data.';"
    "setTimeout(()=>location.reload(),2500)}).catch(e=>s.textContent=e.message)}"
    "</script>"
    "</div></body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
}

// GET /weather â€” indoor SEN66 readings plus online outdoor weather
void handleWeather() {
  int avgAqi = blendedParticleAqi();
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Indoor and Outdoor Weather</title>" + String(kCSS) + "</head><body><div class='w'>" +
    webTabs("weather") +
    "<div class='card'><h1>Indoor - SEN66</h1><div class='row'>" +
    metric("Temperature", indoor.valid ? String(indoor.temperature*9.0f/5.0f+32.0f,1)+" &deg;F" : "--") +
    metric("Humidity", indoor.valid ? String(indoor.humidity,1)+"%" : "--") +
    metric("CO2", indoor.valid && indoor.co2!=0xFFFF ? String(indoor.co2)+" ppm" : "--") +
    metric("VOC Index", indoor.valid ? String(indoor.vocIndex,0) : "--") +
    metric("NOx Index", indoor.valid ? String(indoor.noxIndex,0) : "--") +
    metric("PM1.0", indoor.valid ? String(indoor.pm1,1)+" ug/m3" : "--") +
    metric("PM2.5", indoor.valid ? String(indoor.pm25,1)+" ug/m3" : "--") +
    metric("PM4.0", indoor.valid ? String(indoor.pm4,1)+" ug/m3" : "--") +
    metric("PM10", indoor.valid ? String(indoor.pm10,1)+" ug/m3" : "--") +
    metric("Indoor particle AQI", indoor.valid ? String(indoor.particleAqi)+" - "+aqiCategory(indoor.particleAqi) : "--") +
    "</div></div>"
    "<div class='card'><h1>Outdoor - " + String(activeWeatherLocation) + "</h1><div class='row'>" +
    metric("Temperature", weatherData.valid ? String(weatherData.currentTemp*9.0f/5.0f+32.0f,1)+" &deg;F" : "--") +
    metric("Feels like", weatherData.valid ? String(weatherData.feelsLike*9.0f/5.0f+32.0f,1)+" &deg;F" : "--") +
    metric("Humidity", weatherData.valid ? String(weatherData.humidity)+"%" : "--") +
    metric("Condition", weatherData.valid ? weatherData.condition : "--") +
    metric("Wind", weatherData.valid ? String(weatherData.windSpeed*0.621371f,1)+" mph "+weatherData.windDir : "--") +
    metric("PM2.5", weatherData.valid ? String(weatherData.pm25,1)+" ug/m3" : "--") +
    metric("Outdoor PM2.5 AQI", weatherData.valid ? String(weatherData.pmAqi)+" - "+aqiCategory(weatherData.pmAqi) : "--") +
    "</div></div>"
    "<div class='card'><h2>Combined view</h2><div class='row'>" +
    metric("Average particle AQI", avgAqi>=0 ? String(avgAqi)+" - "+aqiCategory(avgAqi) : "--") +
    "</div><div class='k'>This arithmetic mean is a dashboard indicator, not an EPA AQI calculation or health diagnosis.</div></div>"
    "</div></body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
}

// GET /seasons â€” Northern Hemisphere season summary
void handleSeasons() {
  int year=rtc.getYear(), doy=dayOfYear(rtc.getDay(),rtc.getMonth(),year);
  SeasonInfo s=getSeasonInfo(doy,year);
  SeasonEvents e=calcSeasonEvents(year);
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Seasons</title>" + String(kCSS) + "</head><body><div class='w'>" +
    webTabs("seasons") +
    "<div class='card'><h1>Northern Hemisphere Seasons</h1><div class='row'>" +
    metric("Current season", s.name) +
    metric("Days into season", String(s.daysSince)) +
    metric("Next event", String(s.nextEvent)+" in "+String(s.daysUntil)+" days") +
    "</div></div><div class='card'><h2>Approximate astronomical dates</h2><div class='row'>" +
    metric("Spring equinox", doyToDateStr(e.marchEq,year)) +
    metric("Summer solstice", doyToDateStr(e.juneSol,year)) +
    metric("Fall equinox", doyToDateStr(e.septEq,year)) +
    metric("Winter solstice", doyToDateStr(e.decSol,year)) +
    "</div></div></div></body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
}
// GET /state — returns minimal JSON for polling
void handleState() {
  String j = "{\"page\":"  + String(currentPage) +
             ",\"inv\":"   + String(displayInvert   ? "true":"false") +
             ",\"beep\":"  + String(beepEnabled      ? "true":"false") +
             ",\"bvol\":"  + String(beepVolume) +
             ",\"cycle\":" + String(autoCycleEnabled ? "true":"false") + "}";
  webServer.send(200, "application/json", j);
}

// GET /setpage?p=N
void handleSetPage() {
  if (webServer.hasArg("p")) {
    int p = webServer.arg("p").toInt();
    if (p >= 0 && p < totalPages) {
      currentPage = p;
      webPageBeep = true;  // trigger beep on main thread
    }
  }
  webServer.send(200, "text/plain", "ok");
}

// GET /setpageenabled?p=N&v=true|false
void handleSetPageEnabled() {
  if (!webServer.hasArg("p") || !webServer.hasArg("v")) {
    webServer.send(400, "text/plain", "Missing page selection.");
    return;
  }
  int page = webServer.arg("p").toInt();
  if (page < 0 || page >= totalPages) {
    webServer.send(400, "text/plain", "Invalid page.");
    return;
  }
  uint16_t bit = (1U << page);
  if (webServer.arg("v") == "true") {
    pageEnabledMask |= bit;
  } else {
    uint16_t updatedMask = pageEnabledMask & ~bit;
    if (updatedMask == 0) {
      webServer.send(409, "text/plain", "At least one LCD page must remain checked.");
      return;
    }
    pageEnabledMask = updatedMask;
  }
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /setbeepvol?v=N (1-100)
void handleSetBeepVol() {
  if (webServer.hasArg("v")) {
    int v = webServer.arg("v").toInt();
    if (v >= 1 && v <= 100) beepVolume = v;
  }
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /setinvert?v=true|false
void handleSetInvert() {
  if (webServer.hasArg("v")) displayInvert = (webServer.arg("v") == "true");
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /setbeep?v=true|false
void handleSetBeep() {
  if (webServer.hasArg("v")) beepEnabled = (webServer.arg("v") == "true");
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /setalarmaudio?v=true|false
void handleSetAlarmAudio() {
  if (webServer.hasArg("v")) alarmAudioEnabled = (webServer.arg("v") == "true");
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /setcycle?v=true|false
void handleSetCycle() {
  if (webServer.hasArg("v")) {
    autoCycleEnabled = (webServer.arg("v") == "true");
    autoCycleLastMs  = millis();
  }
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /setcyclesec?v=N
void handleSetCycleSec() {
  if (webServer.hasArg("v")) {
    int s = webServer.arg("v").toInt();
    if (s >= 3 && s <= 300) autoCycleSeconds = s;
  }
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /setweatherlocation?v=LAT,LON
void handleSetWeatherLocation() {
  if (!webServer.hasArg("v")) {
    webServer.send(400, "text/plain", "Enter latitude and longitude.");
    return;
  }
  String value = webServer.arg("v");
  value.trim();
  int comma = value.indexOf(',');
  if (value.length() < 3 || value.length() >= (int)sizeof(activeWeatherLocation) ||
      comma <= 0 || comma >= (int)value.length() - 1 || value.indexOf(',', comma + 1) >= 0) {
    webServer.send(400, "text/plain", "Use decimal latitude,longitude.");
    return;
  }
  String latText = value.substring(0, comma);
  String lonText = value.substring(comma + 1);
  latText.trim();
  lonText.trim();
  char* latEnd = nullptr;
  char* lonEnd = nullptr;
  float latitude = strtof(latText.c_str(), &latEnd);
  float longitude = strtof(lonText.c_str(), &lonEnd);
  if (!latEnd || *latEnd != '\0' || !lonEnd || *lonEnd != '\0' ||
      latitude < -90.0f || latitude > 90.0f ||
      longitude < -180.0f || longitude > 180.0f) {
    webServer.send(400, "text/plain",
                   "Latitude must be -90..90 and longitude -180..180.");
    return;
  }
  String normalized = String(latitude, 6) + "," + String(longitude, 6);
  normalized.toCharArray(activeWeatherLocation, sizeof(activeWeatherLocation));
  weatherData.valid = false;
  weatherData.lastUpdate = 0;
  hourlyData.valid = false;
  webRefreshWeather = true;
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /refresh — trigger weather refresh on next loop()
void handleRefresh() {
  webRefreshWeather = true;
  webServer.send(200, "text/plain", "ok");
}

// GET /syncntp — trigger NTP re-sync on next loop()
void handleSyncNTP() {
  webSyncNTP = true;
  webServer.send(200, "text/plain", "ok");
}

// GET /screenshot — serves the current canvas as a 1-bit BMP image
// BMP structure: 14-byte file header + 40-byte DIB header + 8-byte colour table + pixel data
// Negative height in DIB = top-down rows (matches GFXcanvas1 order, no reversal needed)
// Streamed row-by-row so no large RAM allocation required (~15.7 KB total)
void handleScreenshot() {
  const int BPR      = (W + 7) / 8;    // canvas bytes per row = 50
  const int ROW_PAD  = (4 - (BPR % 4)) % 4;  // BMP row padding = 2
  const int BMP_ROW  = BPR + ROW_PAD;  // BMP bytes per row = 52
  const int PIX_SIZE = BMP_ROW * H;    // pixel data bytes = 15600
  const int FILE_SIZE = 14 + 40 + 8 + PIX_SIZE;  // = 15662

  // ── File header (14 bytes) ────────────────────────────────────────────────
  uint8_t fh[14] = {
    'B','M',
    (uint8_t)(FILE_SIZE),       (uint8_t)(FILE_SIZE>>8),
    (uint8_t)(FILE_SIZE>>16),   (uint8_t)(FILE_SIZE>>24),
    0,0, 0,0,           // reserved
    62,0,0,0            // pixel data offset = 62
  };

  // ── DIB header / BITMAPINFOHEADER (40 bytes) ──────────────────────────────
  // Negative height = top-down (matches canvas row order)
  int32_t negH = -H;
  uint8_t dib[40] = {
    40,0,0,0,           // header size
    (uint8_t)(W),(uint8_t)(W>>8),(uint8_t)(W>>16),(uint8_t)(W>>24),  // width
    (uint8_t)(negH),(uint8_t)(negH>>8),(uint8_t)(negH>>16),(uint8_t)(negH>>24),  // height (neg)
    1,0,                // colour planes
    1,0,                // bits per pixel
    0,0,0,0,            // compression = BI_RGB
    (uint8_t)(PIX_SIZE),(uint8_t)(PIX_SIZE>>8),(uint8_t)(PIX_SIZE>>16),(uint8_t)(PIX_SIZE>>24),
    0x13,0x0b,0,0,      // X pixels/metre (72 dpi)
    0x13,0x0b,0,0,      // Y pixels/metre
    2,0,0,0,            // colours in table
    2,0,0,0             // important colours
  };

  // ── Colour table (8 bytes) ────────────────────────────────────────────────
  // [0]=white (canvas 0 = background), [1]=black (canvas 1 = ink)
  // Apply displayInvert: if inverted, swap so display matches what user sees
  uint8_t ct[8];
  if (displayInvert) {
    ct[0]=0x00; ct[1]=0x00; ct[2]=0x00; ct[3]=0x00;  // [0] = black
    ct[4]=0xFF; ct[5]=0xFF; ct[6]=0xFF; ct[7]=0x00;  // [1] = white
  } else {
    ct[0]=0xFF; ct[1]=0xFF; ct[2]=0xFF; ct[3]=0x00;  // [0] = white
    ct[4]=0x00; ct[5]=0x00; ct[6]=0x00; ct[7]=0x00;  // [1] = black
  }

  // ── Stream response ───────────────────────────────────────────────────────
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.setContentLength(FILE_SIZE);
  webServer.send(200, "image/bmp", "");
  webServer.sendContent((const char*)fh, 14);
  webServer.sendContent((const char*)dib, 40);
  webServer.sendContent((const char*)ct, 8);

  uint8_t* buf = canvas.getBuffer();
  uint8_t pad[4] = {0, 0, 0, 0};
  for (int y = 0; y < H; y++) {
    webServer.sendContent((const char*)(buf + y * BPR), BPR);
    if (ROW_PAD > 0) webServer.sendContent((const char*)pad, ROW_PAD);
  }
}

// Helper: build a season event date card, highlighted if it's the next event
static String buildEventCard(const char* label, const String& date, const char* col, bool isNext, int daysUntil) {
  String border = isNext ? String(col) : String("#2c425f");
  String s = "<div style='background:#0f1b2a;border:1px solid " + border + ";border-radius:8px;padding:10px;'>";
  s += "<div style='font-size:11px;color:" + String(col) + ";margin-bottom:3px;'>" + String(label);
  if (isNext) s += " &#9654;";  // ▶ next event indicator
  s += "</div>";
  s += "<div style='font-size:15px;font-weight:700;'>" + date + "</div>";
  if (isNext) {
    s += "<div style='font-size:11px;color:" + String(col) + ";margin-top:3px;'>" + String(daysUntil) + " days</div>";
  }
  s += "</div>";
  return s;
}

// Parse "06:55 AM" / "04:53 PM" time string to decimal hours
static float parseTimeToHours(const String& t) {
  if (t.length() < 5) return 6.0f;
  int h = t.substring(0, 2).toInt();
  int m = t.substring(3, 5).toInt();
  bool pm = (t.indexOf("PM") >= 0 || t.indexOf("pm") >= 0);
  if (pm && h != 12) h += 12;
  if (!pm && h == 12) h = 0;
  return h + m / 60.0f;
}

// GET /setsleep?en=true|false&from=N&to=N  — update sleep schedule and persist to NVS
void handleSetSleep() {
  if (webServer.hasArg("en"))   sleepEnabled  = (webServer.arg("en") == "true");
  if (webServer.hasArg("from")) sleepFromHour = constrain(webServer.arg("from").toInt(), 0, 23);
  if (webServer.hasArg("to"))   sleepToHour   = constrain(webServer.arg("to").toInt(),   0, 23);
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /wakenow — temporary 10 s wake from web UI (same as a button press)
void handleWakeNow() {
  sleepWakeUntil = millis() + 10000UL;
  webServer.send(200, "text/plain", "ok");
}

// ===== TIMER / ALARM WEB HANDLERS =====

// GET /timerstart
void handleTimerStart() { webTimerStart = true; webServer.send(200, "text/plain", "ok"); }
// GET /timerstop
void handleTimerStop()  { webTimerStop  = true; webServer.send(200, "text/plain", "ok"); }
// GET /timerreset
void handleTimerReset() { webTimerReset = true; webServer.send(200, "text/plain", "ok"); }
// GET /swstart
void handleSwStart() { webSwStart = true; webServer.send(200, "text/plain", "ok"); }
// GET /swstop
void handleSwStop()  { webSwStop  = true; webServer.send(200, "text/plain", "ok"); }
// GET /swreset
void handleSwReset() { webSwReset = true; webServer.send(200, "text/plain", "ok"); }

// GET /settimer?name=X&dur=HH:MM:SS
void handleSetTimer() {
  if (webServer.hasArg("name")) {
    String n = webServer.arg("name");
    strncpy(timerState.name, n.c_str(), sizeof(timerState.name)-1);
    timerState.name[sizeof(timerState.name)-1] = '\0';
  }
  if (webServer.hasArg("dur")) {
    String d = webServer.arg("dur");
    // Parse HH:MM:SS or MM:SS
    int h = 0, m = 0, s = 0;
    int c1 = d.indexOf(':'), c2 = d.lastIndexOf(':');
    if (c1 >= 0 && c2 > c1) {
      h = d.substring(0, c1).toInt();
      m = d.substring(c1+1, c2).toInt();
      s = d.substring(c2+1).toInt();
    } else if (c1 >= 0) {
      m = d.substring(0, c1).toInt();
      s = d.substring(c1+1).toInt();
    }
    timerState.durationSecs  = (uint32_t)h*3600 + m*60 + s;
    timerState.remainingSecs = timerState.durationSecs;
    timerState.running = false;
    timerState.expired = false;
  }
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /setalarm?idx=N&h=H&m=M&label=X&days=BBBBBBB&en=true|false
// days = 7-char string of 0/1 for Mon-Sun
void handleSetAlarm() {
  int idx = webServer.hasArg("idx") ? webServer.arg("idx").toInt() : -1;
  if (idx < 0 || idx >= ALARM_COUNT) { webServer.send(400, "text/plain", "bad idx"); return; }
  if (webServer.hasArg("h"))     alarms[idx].hour   = constrain(webServer.arg("h").toInt(), 0, 23);
  if (webServer.hasArg("m"))     alarms[idx].minute = constrain(webServer.arg("m").toInt(), 0, 59);
  if (webServer.hasArg("label")) {
    String lbl = webServer.arg("label");
    strncpy(alarms[idx].label, lbl.c_str(), sizeof(alarms[idx].label)-1);
    alarms[idx].label[sizeof(alarms[idx].label)-1] = '\0';
  }
  if (webServer.hasArg("days")) {
    String days = webServer.arg("days");
    if (days == "oneshot") {
      alarms[idx].dayMask = 0x00;
    } else {
      uint8_t mask = 0;
      for (int b = 0; b < 7 && b < (int)days.length(); b++) {
        if (days[b] == '1') mask |= (1 << b);
      }
      alarms[idx].dayMask = mask;
    }
  }
  if (webServer.hasArg("en")) alarms[idx].enabled = (webServer.arg("en") == "true");
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /timerstate — returns JSON for live web UI display
void handleTimerState() {
  unsigned long elMs = swElapsedMs();
  uint32_t tr = timerState.remainingSecs;
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"tmr_running\":%s,\"tmr_expired\":%s,\"tmr_rem\":%lu,"
    "\"sw_running\":%s,\"sw_elapsed\":%lu,"
    "\"alarm_firing\":%s,\"timer_firing\":%s}",
    timerState.running ? "true" : "false",
    timerState.expired ? "true" : "false",
    (unsigned long)tr,
    swState.running ? "true" : "false",
    (unsigned long)(elMs / 1000),
    alarmFiring ? "true" : "false",
    timerFiring ? "true" : "false"
  );
  webServer.send(200, "application/json", buf);
}

// GET /timers — Timers & Alarms tab
void handleTimers() {
  const char* dayNames[] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};

  // Build alarm cards
  String alarmCards = "";
  for (int i = 0; i < ALARM_COUNT; i++) {
    String firing = (alarmFiring && alarmFiringIdx == i) ?
      " style='border-color:#e74c3c;'" : "";
    alarmCards += "<div class='card'" + firing + ">";
    alarmCards += "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;'>";
    alarmCards += "<h2 style='margin:0;'>Alarm " + String(i+1) + (alarms[i].label[0] ? String(" &mdash; ") + String(alarms[i].label) : String("")) + "</h2>";
    alarmCards += "<label class='switch'><input type='checkbox' onchange=\"setAlarmEn(" + String(i) + ",this.checked)\"" +
                  (alarms[i].enabled ? " checked" : "") + "><span class='slider'></span></label>";
    alarmCards += "</div>";

    // Time inputs
    char hbuf[4], mbuf[4];
    snprintf(hbuf, sizeof(hbuf), "%d", alarms[i].hour);
    snprintf(mbuf, sizeof(mbuf), "%02d", alarms[i].minute);
    alarmCards += "<div style='display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:10px;'>";
    alarmCards += "<select id='al" + String(i) + "_h' style='background:#0b1421;color:var(--txt);border:1px solid #2c425f;border-radius:6px;padding:6px 8px;'>";
    alarmCards += buildHourOptions(alarms[i].hour);
    alarmCards += "</select>";
    alarmCards += "<span style='color:var(--muted);'>:</span>";
    alarmCards += "<select id='al" + String(i) + "_m' style='background:#0b1421;color:var(--txt);border:1px solid #2c425f;border-radius:6px;padding:6px 8px;'>";
    for (int m = 0; m < 60; m++) {
      char ob[32]; snprintf(ob, sizeof(ob), "<option value='%d'%s>%02d</option>", m, m==alarms[i].minute?" selected":"", m);
      alarmCards += String(ob);
    }
    alarmCards += "</select>";
    alarmCards += "<input type='text' id='al" + String(i) + "_lbl' maxlength='12' placeholder='Label (12 chars)' value='" +
                  String(alarms[i].label) + "' style='background:#0b1421;color:var(--txt);border:1px solid #2c425f;border-radius:6px;padding:6px 8px;width:130px;'>";
    alarmCards += "<button class='btn sec' onclick=\"saveAlarm(" + String(i) + ")\">Save</button>";
    alarmCards += "</div>";

    // Day mask
    alarmCards += "<div style='display:flex;gap:6px;flex-wrap:wrap;align-items:center;'>";
    alarmCards += "<span style='color:var(--muted);font-size:12px;'>Repeat:</span>";
    for (int d = 0; d < 7; d++) {
      bool active = (alarms[i].dayMask & (1 << d));
      alarmCards += "<button id='al" + String(i) + "_d" + String(d) + "' "
                    "onclick=\"toggleDay(" + String(i) + "," + String(d) + ")\" "
                    "style='padding:4px 8px;border-radius:6px;border:1px solid #2c425f;cursor:pointer;font-size:12px;"
                    "background:" + String(active ? "#274161" : "#0f1b2a") + ";color:var(--txt);'>" +
                    String(dayNames[d]) + "</button>";
    }
    bool oneShot = (alarms[i].dayMask == 0x00);
    alarmCards += "<button id='al" + String(i) + "_os' "
                  "onclick=\"toggleOneShot(" + String(i) + ")\" "
                  "style='padding:4px 8px;border-radius:6px;border:1px solid #2c425f;cursor:pointer;font-size:12px;"
                  "background:" + String(oneShot ? "#274161" : "#0f1b2a") + ";color:var(--txt);'>One-shot</button>";
    alarmCards += "</div>";
    alarmCards += "</div>";
  }

  // Timer duration formatted
  char durBuf[12];
  uint32_t td = timerState.durationSecs;
  snprintf(durBuf, sizeof(durBuf), "%02lu:%02lu:%02lu", (unsigned long)(td/3600), (unsigned long)((td%3600)/60), (unsigned long)(td%60));

  String html = "<!doctype html><html><head>"
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Timers &amp; Alarms</title>" + String(kCSS) + "</head><body><div class='w'>"

    "<div class='tabs'>"
    "<a class='tab' href='/'>Control</a>"
    "<a class='tab' href='/weather'>Indoor &amp; Weather</a>"
    "<a class='tab' href='/seasons'>Seasons</a>"
    ""
    "<a class='tab active' href='/timers'>Timers &amp; Alarms</a>"
    "</div>"

    "<div class='card'>"
    "<h2>Alarm Audio</h2>"
    "<div class='sw'><span class='swlbl'>Sound alarm and timer chimes</span>"
    "<label class='switch'><input type='checkbox' id='alarm_audio'" +
    String(alarmAudioEnabled ? " checked" : "") +
    " onchange='setAlarmAudio(this.checked)'><span class='slider'></span></label></div>"
    "<div class='k'>Disabling this keeps visual alarm and timer notifications active.</div>"
    "</div>"

    // ---- Countdown Timer card ----
    "<div class='card'>"
    "<h1>Countdown Timer</h1>"
    "<div style='display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:14px;'>"
    "<input type='text' id='tmr_name' maxlength='20' placeholder='Timer name' value='" + String(timerState.name) + "' "
    "style='background:#0b1421;color:var(--txt);border:1px solid #2c425f;border-radius:6px;padding:6px 8px;width:160px;'>"
    "<input type='text' id='tmr_dur' placeholder='HH:MM:SS' value='" + String(durBuf) + "' "
    "style='background:#0b1421;color:var(--txt);border:1px solid #2c425f;border-radius:6px;padding:6px 8px;width:110px;'>"
    "<button class='btn sec' onclick=\"setTimer()\">Set</button>"
    "</div>"
    "<div style='font-size:36px;font-weight:700;font-family:monospace;color:var(--acc);margin-bottom:14px;' id='tmr_display'>--:--:--</div>"
    "<div class='row'>"
    "<button class='btn' onclick=\"doT('/timerstart')\">&#9654; Start</button>"
    "<button class='btn sec' onclick=\"doT('/timerstop')\">&#9646;&#9646; Pause</button>"
    "<button class='btn sec' onclick=\"doT('/timerreset')\">&#8635; Reset</button>"
    "</div>"
    "</div>"

    // ---- Stopwatch card ----
    "<div class='card'>"
    "<h1>Stopwatch</h1>"
    "<div style='font-size:36px;font-weight:700;font-family:monospace;color:#4da6ff;margin-bottom:14px;' id='sw_display'>00:00</div>"
    "<div class='row'>"
    "<button class='btn' onclick=\"doT('/swstart')\">&#9654; Start</button>"
    "<button class='btn sec' onclick=\"doT('/swstop')\">&#9646;&#9646; Stop</button>"
    "<button class='btn sec' onclick=\"doT('/swreset')\">&#8635; Reset</button>"
    "</div>"
    "</div>";

  html += alarmCards;

  html +=
    "<script>"
    // Alarm day mask state — held in JS so toggles work without page reload
    "var dayMasks=[";
  for (int i = 0; i < ALARM_COUNT; i++) {
    html += String(alarms[i].dayMask) + (i < ALARM_COUNT-1 ? "," : "");
  }
  html += String("];\n");
  html +=
    "function doT(url){fetch(url).catch(()=>{});}\n"
    "function setAlarmAudio(v){fetch('/setalarmaudio?v='+v).catch(()=>{});}\n"
    "function setTimer(){"
    "  var n=document.getElementById('tmr_name').value.trim();"
    "  var d=document.getElementById('tmr_dur').value.trim();"
    "  fetch('/settimer?name='+encodeURIComponent(n)+'&dur='+encodeURIComponent(d)).catch(()=>{});"
    "}\n"
    "function saveAlarm(i){"
    "  var h=document.getElementById('al'+i+'_h').value;"
    "  var m=document.getElementById('al'+i+'_m').value;"
    "  var lbl=document.getElementById('al'+i+'_lbl').value.trim();"
    "  var dm=dayMasks[i];"
    "  var days=(dm===0)?'oneshot':dm.toString(2).padStart(7,'0').split('').reverse().join('');"
    "  fetch('/setalarm?idx='+i+'&h='+h+'&m='+m+'&label='+encodeURIComponent(lbl)+'&days='+days).catch(()=>{});"
    "}\n"
    "function setAlarmEn(i,v){"
    "  fetch('/setalarm?idx='+i+'&en='+v).catch(()=>{});"
    "}\n"
    "function toggleDay(i,d){"
    "  dayMasks[i]^=(1<<d);"
    "  var el=document.getElementById('al'+i+'_d'+d);"
    "  if(el)el.style.background=(dayMasks[i]&(1<<d))?'#274161':'#0f1b2a';"
    "  var os=document.getElementById('al'+i+'_os');"
    "  if(os)os.style.background=(dayMasks[i]===0)?'#274161':'#0f1b2a';"
    "}\n"
    "function toggleOneShot(i){"
    "  dayMasks[i]=0;"
    "  for(var d=0;d<7;d++){var el=document.getElementById('al'+i+'_d'+d);if(el)el.style.background='#0f1b2a';}"
    "  var os=document.getElementById('al'+i+'_os');if(os)os.style.background='#274161';"
    "}\n"
    "function fmtSecs(s){"
    "  var h=Math.floor(s/3600);s%=3600;"
    "  var m=Math.floor(s/60);s%=60;"
    "  if(h>0)return pad(h)+':'+pad(m)+':'+pad(s);"
    "  return pad(m)+':'+pad(s);"
    "}\n"
    "function pad(n){return n<10?'0'+n:String(n);}\n"
    "function pollTimers(){"
    "  fetch('/timerstate').then(r=>r.json()).then(d=>{"
    "    document.getElementById('tmr_display').textContent=fmtSecs(d.tmr_rem);"
    "    document.getElementById('sw_display').textContent=fmtSecs(d.sw_elapsed);"
    "  }).catch(()=>{});"
    "}\n"
    "setInterval(pollTimers,1000);"
    "pollTimers();"
    "</script>"
    "</div></body></html>";

  webServer.send(200, "text/html; charset=utf-8", html);
}

void startWebServer() {
  webServer.on("/",            handleRoot);
  webServer.on("/weather",     handleWeather);
  webServer.on("/seasons",     handleSeasons);
  webServer.on("/timers",      handleTimers);
  webServer.on("/state",       handleState);
  webServer.on("/screenshot",  handleScreenshot);
  webServer.on("/setpage",     handleSetPage);
  webServer.on("/setpageenabled", handleSetPageEnabled);
  webServer.on("/setinvert",   handleSetInvert);
  webServer.on("/setbeep",     handleSetBeep);
  webServer.on("/setalarmaudio", handleSetAlarmAudio);
  webServer.on("/setbeepvol",  handleSetBeepVol);
  webServer.on("/setcycle",    handleSetCycle);
  webServer.on("/setcyclesec", handleSetCycleSec);
  webServer.on("/setweatherlocation", handleSetWeatherLocation);
  webServer.on("/refresh",     handleRefresh);
  webServer.on("/syncntp",     handleSyncNTP);
  webServer.on("/setsleep",      handleSetSleep);
  webServer.on("/wakenow",       handleWakeNow);
  webServer.on("/timerstart",    handleTimerStart);
  webServer.on("/timerstop",     handleTimerStop);
  webServer.on("/timerreset",    handleTimerReset);
  webServer.on("/swstart",       handleSwStart);
  webServer.on("/swstop",        handleSwStop);
  webServer.on("/swreset",       handleSwReset);
  webServer.on("/settimer",      handleSetTimer);
  webServer.on("/setalarm",      handleSetAlarm);
  webServer.on("/timerstate",    handleTimerState);
  webServer.onNotFound([]() { webServer.sendHeader("Location","/"); webServer.send(303,"text/plain",""); });
  webServer.begin();
  Serial.print("[WEB] Serving at http://"); Serial.println(WiFi.localIP());
}

void handleButtons() {
  unsigned long now = millis();
  bool btnLNow = (digitalRead(BTN_LEFT)   == LOW);
  bool btnMNow = (digitalRead(BTN_MIDDLE) == LOW);

  // While display is sleeping, any button press wakes it for 10 seconds.
  // No page changes or other actions are triggered during sleep.
  if (isDisplaySleeping()) {
    if ((btnLNow && !btnLeftPrev) || (btnMNow && !btnMiddlePrev)) {
      sleepWakeUntil = millis() + 10000UL;
    }
    btnLeftPrev   = btnLNow;
    btnMiddlePrev = btnMNow;
    return;
  }

  // BTN_LEFT: short press = next page, long press = previous page
  // When alarm/timer firing: short press = snooze, long press = dismiss
  if (btnLNow && !btnLeftPrev)  { lastBtnLeftDown = now; btnLeftHeld = false; }
  if (btnLNow && btnLeftPrev && !btnLeftHeld && (now - lastBtnLeftDown >= LONG_PRESS_MS)) {
    btnLeftHeld = true;
    if (alarmFiring)       { dismissAlarm(); }
    else if (timerFiring)  { dismissTimer(); }
    else if (alarmSnoozeUntil > 0 && millis() < alarmSnoozeUntil) {
      // Long press during snooze = fully dismiss
      if (alarmSnoozeIdx >= 0 && alarms[alarmSnoozeIdx].dayMask == 0x00) {
        alarms[alarmSnoozeIdx].enabled = false;
        nvsSave();
      }
      alarmSnoozeUntil = 0;
      alarmSnoozeIdx   = -1;
    }
    else {
      currentPage = nextEnabledPage(currentPage, -1);
      beepPageChange();
    }
  }
  if (!btnLNow && btnLeftPrev) {
    if (!btnLeftHeld && (now - lastBtnLeftDown > 30) && (now - lastBtnLeftDown < LONG_PRESS_MS)) {
      if (alarmFiring)       { snoozeAlarm(); }
      else if (timerFiring)  { dismissTimer(); }
      else {
        currentPage = nextEnabledPage(currentPage, 1);
        beepPageChange();
      }
    }
    btnLeftHeld = false;
  }
  btnLeftPrev = btnLNow;

  // BTN_MIDDLE: short press refreshes online weather. Long press is reserved.
  if (btnMNow && !btnMiddlePrev) { lastBtnMiddleDown = now; btnMiddleHeld = false; }
  if (btnMNow && btnMiddlePrev && !btnMiddleHeld && (now - lastBtnMiddleDown >= LONG_PRESS_MS)) {
    btnMiddleHeld = true;
    // Reserved for a future local action.
  }
  if (!btnMNow && btnMiddlePrev) {
    if (!btnMiddleHeld && (now - lastBtnMiddleDown > 30) && (now - lastBtnMiddleDown < LONG_PRESS_MS)) {
      if (wifiConnected) fetchWeatherData();
    }
    btnMiddleHeld = false;
  }
  btnMiddlePrev = btnMNow;
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-S3 SEN66 WEATHER STATION ===");
  bootMillis = millis();

  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_MIDDLE, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  nvsLoad();

  Wire.begin(13, 14);
  initAudio();
  RlcdPort.RLCD_Init();
  rtc.begin();

  canvas.fillScreen(0);
  canvas.drawRect(0, 0, W, H, 1); canvas.drawRect(1, 1, W-2, H-2, 1);
  canvas.setTextColor(1);
  char locTzBuf[40];
  snprintf(locTzBuf, sizeof(locTzBuf), "%s %s", activeWeatherLocation, getTZLabel());
  printCentered(&FONT_MEDIUM, 36, "ESP32-S3 RLCD");
  printCentered(&FONT_MEDIUM, 60, "Indoor Weather Station");
  printCentered(&FONT_SMALL, 82, locTzBuf);
  pushCanvasToRLCD(displayInvert);

  int bootLine = 0;
  auto drawBoot = [&](const char* msg, int state) {
    int ly = 118 + bootLine * 24;
    canvas.fillRect(14, ly-14, 372, 20, 0);
    canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
    canvas.setCursor(16, ly); canvas.print(msg);
    canvas.setCursor(358, ly); canvas.print(state == 1 ? "OK" : state == -1 ? "--" : "");
    pushCanvasToRLCD(displayInvert);
    bootLine++;
  };

  indoor.valid = false;
  weatherData.valid = false; weatherData.lastUpdate = 0;
  hourlyData.valid = false;

  drawBoot("Starting SEN66...", 0);
  bool senReady = initSen66();
  bootLine--; drawBoot(senReady ? "SEN66 started" : "SEN66 not found", senReady ? 1 : -1);

  drawBoot("Connecting WiFi...", 0); connectWiFi();
  bootLine--; drawBoot(wifiConnected ? "WiFi connected" : "WiFi offline", wifiConnected ? 1 : -1);

  if (wifiConnected) {
    startWebServer();
    drawBoot("Syncing clock...", 0); syncRTCWithNTP();
    bootLine--; drawBoot("Clock sync complete", 1);
    drawBoot("Loading outdoor weather...", 0); fetchWeatherData();
    bootLine--; drawBoot(weatherData.valid ? "Outdoor weather loaded" : "Weather unavailable", weatherData.valid ? 1 : -1);
  }

  if (senReady) {
    delay(1100);
    if (readSen66()) sensorReadCount++; else sensorFailCount++;
  }
  batteryVoltage = readBatteryVoltage();
  lastBatteryReadMs = millis();
  lastSensorReadMs = millis();

  history.initialized=false; history.currentIndex=0; history.sampleCount=0; history.lastLogTime=millis();
  for (int i=0;i<HISTORY_SIZE;i++) {
    history.tempHistory[i]=cToF(temperature);
    history.humidityHistory[i]=humidity;
  }
  beepBootOk();
  Serial.println("Ready!");
  delay(500);
}
// ===== LOOP =====
void loop() {
  if (wifiConnected) webServer.handleClient();  // non-blocking — must be called every loop

  handleButtons();
  // Read all RTC values in one pass to minimise I2C transactions
  hour24    = rtc.getHour();
  minuteVal = rtc.getMinute();
  // secondVal needed for dashboard (page 0) and analogue clock (page 1) second hands
  if (currentPage == 0 || currentPage == 1) secondVal = rtc.getSecond();

  unsigned long now = millis();
  if (now - lastSensorReadMs >= 1000UL) {
    lastSensorReadMs = now;
    if (readSen66()) {
      sensorReadCount++;
      Serial.printf("T=%.1fC RH=%.1f%% CO2=%u VOC=%.1f NOx=%.1f PM1=%.1f PM2.5=%.1f PM4=%.1f PM10=%.1f AQI=%d",
        indoor.temperature, indoor.humidity, indoor.co2, indoor.vocIndex, indoor.noxIndex,
        indoor.pm1, indoor.pm25, indoor.pm4, indoor.pm10, indoor.particleAqi);
    } else {
      sensorFailCount++;
      Serial.print("SEN66 read failed");
    }
    if (wifiConnected) { wifiRSSI=WiFi.RSSI(); Serial.printf(" WiFi=%d",wifiRSSI); }
    Serial.println();
  }
  if (now - lastBatteryReadMs >= 10000UL) {
    lastBatteryReadMs=now;
    batteryVoltage=readBatteryVoltage();
  }
  // Low battery warning beep — fires when voltage drops below 3.50V, at most once per minute
  unsigned long nowMs = millis();
  if (audioReady && codecReady && batteryVoltage > 0.5f && batteryVoltage < 3.50f) {
    if (nowMs - lastBatteryBeepMs >= 60000UL) {
      lastBatteryBeepMs = nowMs;
      beepLowBattery();
    }
  }

  if (now - history.lastLogTime >= 900000UL) {
    history.tempHistory[history.currentIndex]     = cToF(temperature);
    history.humidityHistory[history.currentIndex] = humidity;
    history.currentIndex = (history.currentIndex + 1) % HISTORY_SIZE;
    history.lastLogTime  = now;
    if (history.sampleCount < HISTORY_SIZE) history.sampleCount++;
    history.initialized = (history.sampleCount >= HISTORY_SIZE);
  }

  if (wifiConnected && (weatherData.lastUpdate == 0 || (now - weatherData.lastUpdate) > 1800000UL))
    fetchWeatherData();

  // Re-sync RTC with NTP every 24 hours
  if (wifiConnected && (ntpLastSync == 0 || (now - ntpLastSync) > 86400000UL))
    syncRTCWithNTP();

  // Web UI triggered actions (set by handlers, actioned here on main thread)
  if (webRefreshWeather) { webRefreshWeather = false; if (wifiConnected) fetchWeatherData(); }
  if (webSyncNTP)        { webSyncNTP        = false; if (wifiConnected) syncRTCWithNTP(); }
  if (webPageBeep)       { webPageBeep       = false; beepPageChange(); }
  // Timer/stopwatch/alarm web flags
  if (webTimerStart) { webTimerStart = false; timerState.running = true; timerState.expired = false; timerState.lastTick = millis(); }
  if (webTimerStop)  { webTimerStop  = false; timerState.running = false; }
  if (webTimerReset) {
    webTimerReset = false;
    timerState.running = false; timerState.expired = false;
    timerState.remainingSecs = timerState.durationSecs;
    timerFiring = false; timerRepeatCount = 0;
  }
  if (webSwStart) { webSwStart = false; if (!swState.running) { swState.startMs = millis(); swState.running = true; } }
  if (webSwStop)  { webSwStop  = false; if (swState.running)  { swState.elapsed += millis() - swState.startMs; swState.running = false; } }
  if (webSwReset) { webSwReset = false; swState.running = false; swState.elapsed = 0; swState.startMs = 0; }

  // Alarm & timer tick
  checkAlarms();
  updateTimer();
  serviceAlarmChimes();

  // Auto-cycle pages — paused while display is sleeping
  if (!isDisplaySleeping() && autoCycleEnabled && (now - autoCycleLastMs >= (unsigned long)autoCycleSeconds * 1000UL)) {
    autoCycleLastMs = now;
    currentPage = nextEnabledPage(currentPage, 1);
    beepPageChange();
  }

  // Display sleep — skip all canvas draws when sleeping.
  // On first entry to sleep: show a brief "Going to sleep..." banner then stop.
  // Button press (handled above in handleButtons) sets sleepWakeUntil for 10 s.
  static bool wasSleeping = false;
  bool nowSleeping = isDisplaySleeping();

  if (!wasSleeping && nowSleeping) {
    // Just entered sleep window — show one-shot banner for 1 s then blank the display
    canvas.fillScreen(0);
    canvas.drawRect(0, 0, W, H, 1);
    canvas.drawRect(1, 1, W-2, H-2, 1);
    canvas.setFont(&FONT_MEDIUM);
    canvas.setTextColor(1);
    printCentered(&FONT_MEDIUM, 155, "Going to sleep...");
    pushCanvasToRLCD(displayInvert);
    delay(1000);
    // Blank the screen so nothing remains visible while sleeping
    canvas.fillScreen(0);
    pushCanvasToRLCD(false);
  }
  wasSleeping = nowSleeping;

  if (nowSleeping) {
    delay(100);   // idle — no SPI writes while sleeping
  } else {
    draw();
    delay(16);
  }
}
