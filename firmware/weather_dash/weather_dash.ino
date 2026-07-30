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
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <time.h>
#include <math.h>
#include <Wire.h>
#include <FS.h>
#include <FFat.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
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

// Resolved from the WeatherAPI location object for activeWeatherLocation.
// A fixed current offset is refreshed with weather data, including after a
// location change. This avoids a compiled-in geographic timezone.
char activeTimezoneId[48] = "UTC";
char activePosixTZ[24] = "UTC0";
long gmtOffset_sec = 0;

const int BTN_LEFT    = 0;
const int BTN_MIDDLE  = 18;
const int BAT_ADC_PIN = 4;
static const uint8_t SEN66_ADDR = 0x6B;
static const uint8_t SHTC3_ADDR = 0x70;

// Schematic R21/R23 divider: 200 kOhm / 100 kOhm, therefore VBAT = VADC * 3.
// This unit reads correctly at 1.020 after comparison with a DMM.
static constexpr float BATTERY_DIVIDER_RATIO = 3.0f;
static constexpr float BATTERY_CALIBRATION = 1.020f;
static constexpr unsigned long USB_STATUS_SAMPLE_MS = 250UL;
static constexpr unsigned long USB_CONNECT_CONFIRM_MS = 250UL;
static constexpr unsigned long USB_DISCONNECT_CONFIRM_MS = 2000UL;
static constexpr unsigned long DISPLAY_UPDATE_MS = 2000UL;  // 0.5 Hz
static constexpr unsigned long DISPLAY_INTERACTIVE_UPDATE_MS = 250UL;
static constexpr unsigned long DISPLAY_INTERACTIVE_WINDOW_MS = 60000UL;
static constexpr unsigned long SHTC3_UPDATE_MS = 15000UL;
static constexpr unsigned long HISTORY_UPDATE_MS = 900000UL;
static constexpr uint32_t HISTORY_WINDOW_SECONDS = 21600UL;
static constexpr uint32_t SENSOR_COMPARE_QUALIFY_SECONDS = 43200UL;
static constexpr uint32_t SENSOR_COMPARE_MIN_SAMPLES = 100UL;
static constexpr unsigned long WIFI_NORMAL_MS = 1800000UL;
static constexpr unsigned long WIFI_RAIN_MS = 600000UL;
static constexpr unsigned long WIFI_MANUAL_WINDOW_MS = 300000UL;

#define FONT_SMALL   FreeSansBold9pt7b
#define FONT_MEDIUM  FreeSansBold12pt7b
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
float batterySoc     = 0.0f;
bool  batteryStateInitialized = false;
bool  usbHostConnected = false;
bool  externalPowerLikely = false;
bool  lowPowerMode   = true;
bool  displayHighPower = true;
unsigned long displayInteractiveUntilMs = 0;
unsigned long lastUsbCheckMs = 0;
bool  usbCandidateConnected = false;
unsigned long usbCandidateChangedMs = 0;
int   wifiRSSI       = 0;
int   hour24         = 0;
int   minuteVal      = 0;
int   secondVal      = 0;
bool  wifiConnected  = false;
unsigned long lastSensorReadMs = 0;
unsigned long lastBatteryReadMs = 0;
unsigned long lastDisplayUpdateMs = 0;
unsigned long ntpLastSync = 0;
int   sensorReadCount     = 0;
int   sensorFailCount     = 0;

static inline float cToF(float celsius) {
  return celsius * 9.0f / 5.0f + 32.0f;
}

int  currentPage  = 0;
const int totalPages = 14;
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
const unsigned long LONG_PRESS_MS = 1000;
unsigned long lastBtnLeftDown   = 0;
unsigned long lastBtnMiddleDown = 0;
bool btnLeftPrev    = true;
bool btnMiddlePrev  = true;
bool btnLeftHeld    = false;
bool btnMiddleHeld  = false;

// Runtime network state. Credentials from secrets.h are defaults; a provisioning
// session can replace them in NVS without changing or publishing secrets.h.
char activeSsid[33] = "";
char activePassword[65] = "";
bool webServerStarted = false;
bool provisioningActive = false;
unsigned long provisioningUntilMs = 0;
unsigned long wifiWindowUntilMs = 0;
unsigned long lastOnlineUpdateMs = 0;
unsigned long lastOnlineAttemptMs = 0;
DNSServer dnsServer;

// SEN66 low-power duty-cycle and VOC baseline retention.
bool sen66MeasurementRunning = false;
unsigned long sen66StartedMs = 0;
unsigned long lastVocStateSaveMs = 0;
static constexpr uint16_t VOC_STATE_SIZE = 8;
uint8_t savedVocState[VOC_STATE_SIZE] = {0};
bool savedVocStateValid = false;

// OpenWeather One Call 4.0 minute timeline and air-pollution results.
struct OwmMinuteData {
  float precipitation[60];
  time_t epoch[60];
  int count;
  float maxPrecipitation;
  bool rainNext30;
  float outsideTempF;
  int rainChance;
  bool outsideValid;
  bool severeWeather;
  uint32_t alertHash;
  bool valid;
  unsigned long lastUpdate;
} owmMinute;
unsigned long rainBoostUntilMs = 0;
bool rainEventArmed = true;
uint32_t acknowledgedAlertHash = 0;
uint16_t owmCallsToday = 0;
int owmCallDay = -1;

// Voltage-derived state-of-charge trend. This is an estimate, not coulomb counting.
struct BatteryTrendPoint {
  float soc;
  unsigned long atMs;
};
BatteryTrendPoint batteryTrend[48];
int batteryTrendCount = 0;
int batteryTrendHead = 0;
unsigned long lastBatteryTrendMs = 0;

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
  float  pressureHpa;
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

enum Co2CalibrationState : uint8_t {
  CO2_CAL_IDLE,
  CO2_CAL_STABILIZING,
  CO2_CAL_EXECUTING,
  CO2_CAL_SUCCESS,
  CO2_CAL_FAILED
};

static const uint16_t CO2_CAL_TARGET_PPM = 400;
static const uint16_t CO2_CAL_MIN_PPM = 350;
static const uint16_t CO2_CAL_MAX_PPM = 450;
static const unsigned long CO2_CAL_STABILIZE_MS = 300000UL;

struct Co2CalibrationStatus {
  Co2CalibrationState state;
  unsigned long qualifyingSince;
  float pressureHpa;
  uint16_t correctionRaw;
  char message[128];
} co2Calibration = {
  CO2_CAL_IDLE, 0, NAN, 0,
  "Ready. Move the sensor outdoors before starting."
};

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
  uint32_t epoch[HISTORY_SIZE];
  int   currentIndex;
  unsigned long lastLogTime;
  bool  initialized;
  int   sampleCount;
} history;

struct SensorComparisonState {
  bool shtc3Present;
  bool shtc3Valid;
  bool qualified;
  float shtc3Temperature;
  float shtc3Humidity;
  uint32_t firstPairEpoch;
  uint32_t lastPairEpoch;
  uint32_t pairCount;
  double meanTempDeltaC;
  double meanHumidityDelta;
  double m2TempDelta;
  double m2HumidityDelta;
  double tempVariance;
  double humidityVariance;
  unsigned long lastShtc3ReadMs;
  unsigned long lastPairedShtc3ReadMs;
  unsigned long lastPersistMs;
} sensorComparison = {};

bool storageReady = false;

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
unsigned long sleepWakeUntil  = 0;       // millis() deadline for temporary display wake

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
  digitalWrite(SPK_EN_PIN, HIGH);  // enabled only during codec initialization
  delay(5);
  i2sAudio.setPins(I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DOUT_PIN, -1, I2S_MCLK_PIN);
  audioReady = i2sAudio.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (!audioReady) { Serial.println("[AUDIO] I2S init failed"); return false; }
  codecReady = es8311InitPlayback();
  if (!codecReady) { Serial.println("[AUDIO] ES8311 init failed"); return false; }
  digitalWrite(SPK_EN_PIN, LOW);   // NS4150B shutdown between audible events
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
  digitalWrite(SPK_EN_PIN, HIGH);
  delay(2);
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
  digitalWrite(SPK_EN_PIN, LOW);
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
      currentPage = 13;   // jump to timers page
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
    currentPage = 13;  // jump to timers/alarms page
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
  currentPage      = 13;  // stay on timers page to show snooze countdown
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
int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 +
                       day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + (int64_t)doe - 719468LL;
}

void configureFixedOffsetTimezone(long offsetSeconds) {
  long totalMinutes = offsetSeconds / 60;
  long magnitude = labs(totalMinutes);
  long hours = magnitude / 60;
  long minutes = magnitude % 60;
  // POSIX TZ signs are reversed: UTC5 means UTC-05:00.
  if (minutes == 0) {
    snprintf(activePosixTZ, sizeof(activePosixTZ), "UTC%s%ld",
             totalMinutes > 0 ? "-" : "", hours);
  } else {
    snprintf(activePosixTZ, sizeof(activePosixTZ), "UTC%s%ld:%02ld",
             totalMinutes > 0 ? "-" : "", hours, minutes);
  }
  setenv("TZ", activePosixTZ, 1);
  tzset();
}

bool applyWeatherLocationTime(const String& timezoneId,
                              const String& localTimeText,
                              time_t localEpoch) {
  int year, month, day, hour, minute;
  if (timezoneId.length() == 0 || localEpoch < 1000000000 ||
      sscanf(localTimeText.c_str(), "%d-%d-%d %d:%d",
             &year, &month, &day, &hour, &minute) != 5) {
    return false;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return false;
  }

  int64_t localAsUtc = daysFromCivil(year, (unsigned)month, (unsigned)day) *
                       86400LL + hour * 3600LL + minute * 60LL +
                       (localEpoch % 60);
  int64_t offset = localAsUtc - (int64_t)localEpoch;
  // Round to a whole minute and allow every civil offset from UTC-14 to UTC+14.
  offset = offset >= 0 ? ((offset + 30) / 60) * 60 :
                         ((offset - 30) / 60) * 60;
  if (offset < -14 * 3600LL || offset > 14 * 3600LL) return false;

  bool changed = gmtOffset_sec != (long)offset ||
                 timezoneId != String(activeTimezoneId);
  gmtOffset_sec = (long)offset;
  timezoneId.toCharArray(activeTimezoneId, sizeof(activeTimezoneId));
  configureFixedOffsetTimezone(gmtOffset_sec);

  int weekday = (int)((daysFromCivil(year, (unsigned)month, (unsigned)day) + 4) % 7);
  if (weekday < 0) weekday += 7;
  rtc.setTime(hour, minute, (int)(localEpoch % 60));
  rtc.setDate(weekday, day, month, year);

  if (changed) {
    prefs.begin("dash", false);
    prefs.putString("tz_id", activeTimezoneId);
    prefs.putLong("tz_off", gmtOffset_sec);
    prefs.end();
  }
  Serial.printf("[TIME] %s, UTC%+ld:%02ld, local %s\n",
                activeTimezoneId, gmtOffset_sec / 3600,
                labs((gmtOffset_sec / 60) % 60), localTimeText.c_str());
  return true;
}

const char* getTZLabel() {
  static char label[12];
  long totalMinutes = gmtOffset_sec / 60;
  snprintf(label, sizeof(label), "UTC%+ld:%02ld",
           totalMinutes / 60, labs(totalMinutes % 60));
  return label;
}

uint32_t currentEpoch() {
  time_t systemNow = time(nullptr);
  if (systemNow >= 1700000000) return (uint32_t)systemNow;

  int year = rtc.getYear();
  int month = rtc.getMonth();
  int day = rtc.getDay();
  if (year < 2024 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
  int64_t localSeconds =
    daysFromCivil(year, (unsigned)month, (unsigned)day) * 86400LL +
    rtc.getHour() * 3600LL + rtc.getMinute() * 60LL + rtc.getSecond();
  int64_t utcSeconds = localSeconds - gmtOffset_sec;
  return utcSeconds > 0 ? (uint32_t)utcSeconds : 0;
}

bool shtc3SendCommand(uint16_t command) {
  Wire.beginTransmission(SHTC3_ADDR);
  Wire.write((uint8_t)(command >> 8));
  Wire.write((uint8_t)(command & 0xFF));
  return Wire.endTransmission() == 0;
}

uint8_t sensirionCrc8(const uint8_t* data, size_t length) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

const char* expectedI2cDevice(uint8_t address) {
  switch (address) {
    case 0x18: return "ES8311 audio codec";
    case 0x40: return "ES7210 microphone ADC";
    case 0x51: return "PCF85063A RTC";
    case 0x6B: return "SEN66 external air-quality sensor";
    case 0x70: return "SHTC3 onboard temperature/humidity sensor";
    default: return nullptr;
  }
}

void scanI2cBusAtStartup() {
  // A retained SHTC3 sleep state can survive an ESP32-only reset.
  shtc3SendCommand(0x3517);
  delay(2);

  static const uint8_t expected[] = {0x18, 0x40, 0x51, 0x6B, 0x70};
  bool foundExpected[sizeof(expected)] = {};
  int foundCount = 0;
  int unknownCount = 0;

  Serial.println("[I2C] Startup scan on SDA=GPIO13, SCL=GPIO14");
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    if (error == 0) {
      foundCount++;
      const char* name = expectedI2cDevice(address);
      if (name) {
        Serial.printf("[I2C] 0x%02X %s - EXPECTED (board schematic or configured external sensor)\n",
                      address, name);
        for (size_t i = 0; i < sizeof(expected); i++)
          if (expected[i] == address) foundExpected[i] = true;
      } else {
        unknownCount++;
        Serial.printf("[I2C] 0x%02X UNKNOWN - not listed for this firmware or board\n",
                      address);
      }
    } else if (error == 4) {
      Serial.printf("[I2C] 0x%02X BUS ERROR during address probe\n", address);
    }
  }

  for (size_t i = 0; i < sizeof(expected); i++) {
    if (!foundExpected[i]) {
      Serial.printf("[I2C] 0x%02X MISSING - expected %s\n",
                    expected[i], expectedI2cDevice(expected[i]));
    }
  }
  Serial.printf("[I2C] Scan complete: %d responder(s), %d unknown\n",
                foundCount, unknownCount);
  shtc3SendCommand(0xB098);
}

bool readShtc3(float& temperatureC, float& relativeHumidity) {
  if (!shtc3SendCommand(0x3517)) return false;
  delay(1);
  if (!shtc3SendCommand(0x7866)) return false;
  delay(15);

  uint8_t data[6] = {};
  size_t received = Wire.requestFrom((int)SHTC3_ADDR, 6);
  if (received != 6) {
    shtc3SendCommand(0xB098);
    return false;
  }
  for (int i = 0; i < 6; i++) data[i] = Wire.read();
  shtc3SendCommand(0xB098);
  if (sensirionCrc8(data, 2) != data[2] ||
      sensirionCrc8(data + 3, 2) != data[5]) return false;

  uint16_t rawTemperature = ((uint16_t)data[0] << 8) | data[1];
  uint16_t rawHumidity = ((uint16_t)data[3] << 8) | data[4];
  temperatureC = -45.0f + 175.0f * rawTemperature / 65535.0f;
  relativeHumidity = constrain(100.0f * rawHumidity / 65535.0f, 0.0f, 100.0f);
  return isfinite(temperatureC) && isfinite(relativeHumidity);
}

struct PersistedComparisonState {
  uint32_t magic;
  uint16_t version;
  uint8_t qualified;
  uint8_t reserved;
  uint32_t firstPairEpoch;
  uint32_t lastPairEpoch;
  uint32_t pairCount;
  double meanTempDeltaC;
  double meanHumidityDelta;
  double m2TempDelta;
  double m2HumidityDelta;
  double tempVariance;
  double humidityVariance;
};

void persistSensorComparisonState(bool forceWrite = false) {
  if (!forceWrite && millis() - sensorComparison.lastPersistMs < 3600000UL) return;
  PersistedComparisonState saved = {
    0x53433636UL, 1, (uint8_t)sensorComparison.qualified, 0,
    sensorComparison.firstPairEpoch, sensorComparison.lastPairEpoch,
    sensorComparison.pairCount, sensorComparison.meanTempDeltaC,
    sensorComparison.meanHumidityDelta, sensorComparison.m2TempDelta,
    sensorComparison.m2HumidityDelta, sensorComparison.tempVariance,
    sensorComparison.humidityVariance
  };
  prefs.begin("dash", false);
  prefs.putBytes("sensor_cmp", &saved, sizeof(saved));
  prefs.end();
  sensorComparison.lastPersistMs = millis();
}

void loadSensorComparisonState() {
  PersistedComparisonState saved = {};
  prefs.begin("dash", true);
  size_t storedLength = prefs.getBytesLength("sensor_cmp");
  if (storedLength == sizeof(saved)) prefs.getBytes("sensor_cmp", &saved, sizeof(saved));
  prefs.end();
  if (storedLength != sizeof(saved) || saved.magic != 0x53433636UL ||
      saved.version != 1) return;
  sensorComparison.qualified = saved.qualified != 0;
  sensorComparison.firstPairEpoch = saved.firstPairEpoch;
  sensorComparison.lastPairEpoch = saved.lastPairEpoch;
  sensorComparison.pairCount = saved.pairCount;
  sensorComparison.meanTempDeltaC = saved.meanTempDeltaC;
  sensorComparison.meanHumidityDelta = saved.meanHumidityDelta;
  sensorComparison.m2TempDelta = saved.m2TempDelta;
  sensorComparison.m2HumidityDelta = saved.m2HumidityDelta;
  sensorComparison.tempVariance = saved.tempVariance;
  sensorComparison.humidityVariance = saved.humidityVariance;
}

bool mountStorage() {
  storageReady = FFat.begin(false);
  if (!storageReady) {
    Serial.println("[FFAT] Mount failed. Formatting the configured FATFS partition.");
    storageReady = FFat.begin(true);
  }
  if (storageReady) {
    Serial.printf("[FFAT] Mounted: %llu bytes total, %llu bytes used\n",
                  (unsigned long long)FFat.totalBytes(),
                  (unsigned long long)FFat.usedBytes());
  } else {
    Serial.println("[FFAT] Storage unavailable. Comparison and history files are disabled.");
  }
  return storageReady;
}

void appendSensorComparisonLog(uint32_t epoch, float senTemperature,
                               float senHumidity, float shtTemperature,
                               float shtHumidity) {
  if (!storageReady) return;
  const char* path = "/sensor_compare.csv";
  File existing = FFat.open(path, FILE_READ);
  size_t currentSize = existing ? existing.size() : 0;
  existing.close();
  if (currentSize > 4UL * 1024UL * 1024UL) {
    FFat.remove("/sensor_compare.old.csv");
    FFat.rename(path, "/sensor_compare.old.csv");
    currentSize = 0;
  }
  File file = FFat.open(path, FILE_APPEND);
  if (!file) {
    Serial.println("[FFAT] Could not append /sensor_compare.csv");
    return;
  }
  if (currentSize == 0)
    file.println("epoch,sen66_temp_c,sen66_rh,shtc3_temp_c,shtc3_rh,temp_delta_c,rh_delta");
  file.printf("%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
              (unsigned long)epoch, senTemperature, senHumidity,
              shtTemperature, shtHumidity, senTemperature - shtTemperature,
              senHumidity - shtHumidity);
  file.close();
}

void recordSensorComparison() {
  if (!sensorComparison.shtc3Valid ||
      sensorComparison.lastShtc3ReadMs == sensorComparison.lastPairedShtc3ReadMs ||
      !indoor.valid) return;
  sensorComparison.lastPairedShtc3ReadMs = sensorComparison.lastShtc3ReadMs;
  uint32_t epoch = currentEpoch();
  float rawTempDelta = indoor.temperature - sensorComparison.shtc3Temperature;
  float rawHumidityDelta = indoor.humidity - sensorComparison.shtc3Humidity;
  float tempDelta = constrain(rawTempDelta, -10.0f, 10.0f);
  float humidityDelta = constrain(rawHumidityDelta, -30.0f, 30.0f);
  appendSensorComparisonLog(epoch, indoor.temperature, indoor.humidity,
                            sensorComparison.shtc3Temperature,
                            sensorComparison.shtc3Humidity);

  if (!sensorComparison.qualified) {
    if (sensorComparison.firstPairEpoch == 0 && epoch != 0)
      sensorComparison.firstPairEpoch = epoch;
    sensorComparison.lastPairEpoch = epoch;
    sensorComparison.pairCount++;
    double tempDifference = tempDelta - sensorComparison.meanTempDeltaC;
    sensorComparison.meanTempDeltaC += tempDifference / sensorComparison.pairCount;
    sensorComparison.m2TempDelta +=
      tempDifference * (tempDelta - sensorComparison.meanTempDeltaC);
    double humidityDifference =
      humidityDelta - sensorComparison.meanHumidityDelta;
    sensorComparison.meanHumidityDelta +=
      humidityDifference / sensorComparison.pairCount;
    sensorComparison.m2HumidityDelta +=
      humidityDifference * (humidityDelta - sensorComparison.meanHumidityDelta);

    bool elapsed = epoch != 0 && sensorComparison.firstPairEpoch != 0 &&
      epoch >= sensorComparison.firstPairEpoch &&
      epoch - sensorComparison.firstPairEpoch >= SENSOR_COMPARE_QUALIFY_SECONDS;
    if (elapsed && sensorComparison.pairCount >= SENSOR_COMPARE_MIN_SAMPLES) {
      sensorComparison.tempVariance =
        sensorComparison.m2TempDelta / max(1UL, sensorComparison.pairCount - 1);
      sensorComparison.humidityVariance =
        sensorComparison.m2HumidityDelta / max(1UL, sensorComparison.pairCount - 1);
      sensorComparison.qualified = true;
      Serial.printf("[SENSOR COMPARE] Qualified after %lu pairs. Delta T=%+.3fC, RH=%+.3f%%\n",
                    (unsigned long)sensorComparison.pairCount,
                    sensorComparison.meanTempDeltaC,
                    sensorComparison.meanHumidityDelta);
      persistSensorComparisonState(true);
    } else {
      persistSensorComparisonState(sensorComparison.pairCount == 1);
    }
    return;
  }

  const double alpha = 0.02;
  double tempSigma = max(0.10, sqrt(max(0.0, sensorComparison.tempVariance)));
  double humiditySigma =
    max(1.00, sqrt(max(0.0, sensorComparison.humidityVariance)));
  if (fabs(tempDelta - sensorComparison.meanTempDeltaC) <= tempSigma) {
    double difference = tempDelta - sensorComparison.meanTempDeltaC;
    sensorComparison.meanTempDeltaC += alpha * difference;
    sensorComparison.tempVariance =
      (1.0 - alpha) * (sensorComparison.tempVariance + alpha * difference * difference);
  }
  if (fabs(humidityDelta - sensorComparison.meanHumidityDelta) <= humiditySigma) {
    double difference = humidityDelta - sensorComparison.meanHumidityDelta;
    sensorComparison.meanHumidityDelta += alpha * difference;
    sensorComparison.humidityVariance =
      (1.0 - alpha) *
      (sensorComparison.humidityVariance + alpha * difference * difference);
  }
  sensorComparison.pairCount++;
  sensorComparison.lastPairEpoch = epoch;
  persistSensorComparisonState(false);
}

void serviceShtc3(bool forceRead = false) {
  unsigned long now = millis();
  if (!forceRead && sensorComparison.lastShtc3ReadMs != 0 &&
      now - sensorComparison.lastShtc3ReadMs < SHTC3_UPDATE_MS) return;
  float shtTemperature = NAN;
  float shtHumidity = NAN;
  if (!readShtc3(shtTemperature, shtHumidity)) {
    sensorComparison.shtc3Valid = false;
    Serial.printf("[SHTC3] Read failed. VBAT=%.3fV\n", batteryVoltage);
    return;
  }
  sensorComparison.shtc3Present = true;
  sensorComparison.shtc3Valid = true;
  sensorComparison.shtc3Temperature = shtTemperature;
  sensorComparison.shtc3Humidity = shtHumidity;
  sensorComparison.lastShtc3ReadMs = now;

  if (!sen66MeasurementRunning) {
    temperature = shtTemperature +
      (sensorComparison.qualified ? sensorComparison.meanTempDeltaC : 0.0);
    humidity = constrain(
      shtHumidity +
      (sensorComparison.qualified ? sensorComparison.meanHumidityDelta : 0.0),
      0.0, 100.0);
  }
  Serial.printf("[SHTC3] T=%.2fC RH=%.2f%% source=%s VBAT=%.3fV\n",
                shtTemperature, shtHumidity,
                sensorComparison.qualified ? "corrected fallback ready" : "raw",
                batteryVoltage);
}

int historyStartIndex() {
  return history.sampleCount >= HISTORY_SIZE ? history.currentIndex : 0;
}

void persistHistory() {
  if (!storageReady) return;
  FFat.remove("/history.tmp");
  File file = FFat.open("/history.tmp", FILE_WRITE);
  if (!file) {
    Serial.println("[FFAT] Could not write /history.tmp");
    return;
  }
  file.println("epoch,temperature_f,humidity_percent");
  uint32_t nowEpoch = currentEpoch();
  int start = historyStartIndex();
  for (int i = 0; i < history.sampleCount; i++) {
    int index = (start + i) % HISTORY_SIZE;
    uint32_t pointEpoch = history.epoch[index];
    if (nowEpoch != 0 && pointEpoch != 0 && nowEpoch >= pointEpoch &&
        nowEpoch - pointEpoch > HISTORY_WINDOW_SECONDS) continue;
    file.printf("%lu,%.3f,%.3f\n", (unsigned long)pointEpoch,
                history.tempHistory[index], history.humidityHistory[index]);
  }
  file.close();
  FFat.remove("/history.csv");
  if (!FFat.rename("/history.tmp", "/history.csv"))
    Serial.println("[FFAT] Could not replace /history.csv");
}

void appendHistoryPoint(float temperatureF, float relativeHumidity) {
  history.tempHistory[history.currentIndex] = temperatureF;
  history.humidityHistory[history.currentIndex] = relativeHumidity;
  history.epoch[history.currentIndex] = currentEpoch();
  history.currentIndex = (history.currentIndex + 1) % HISTORY_SIZE;
  if (history.sampleCount < HISTORY_SIZE) history.sampleCount++;
  history.initialized = history.sampleCount >= HISTORY_SIZE;
  history.lastLogTime = millis();
  persistHistory();
}

float historyTemperatureC() {
  if (sen66MeasurementRunning && indoor.valid) return indoor.temperature;
  if (sensorComparison.qualified && sensorComparison.shtc3Valid) {
    return sensorComparison.shtc3Temperature +
           sensorComparison.meanTempDeltaC;
  }
  if (indoor.valid) return indoor.temperature;
  if (sensorComparison.shtc3Valid) return sensorComparison.shtc3Temperature;
  return temperature;
}

float historyRelativeHumidity() {
  if (sen66MeasurementRunning && indoor.valid) return indoor.humidity;
  if (sensorComparison.qualified && sensorComparison.shtc3Valid) {
    return constrain(
      sensorComparison.shtc3Humidity +
      sensorComparison.meanHumidityDelta,
      0.0, 100.0);
  }
  if (indoor.valid) return indoor.humidity;
  if (sensorComparison.shtc3Valid) return sensorComparison.shtc3Humidity;
  return humidity;
}

void loadHistory() {
  history = {};
  history.lastLogTime = millis();
  if (!storageReady) return;
  File file = FFat.open("/history.csv", FILE_READ);
  if (!file) {
    Serial.println("[HISTORY] No stored six-hour history.");
    return;
  }
  uint32_t nowEpoch = currentEpoch();
  while (file.available()) {
    String line = file.readStringUntil('\n');
    unsigned long epochValue = 0;
    float temperatureF = 0.0f;
    float relativeHumidity = 0.0f;
    if (sscanf(line.c_str(), "%lu,%f,%f", &epochValue,
               &temperatureF, &relativeHumidity) != 3) continue;
    if (nowEpoch != 0 && epochValue != 0 && nowEpoch >= epochValue &&
        nowEpoch - (uint32_t)epochValue > HISTORY_WINDOW_SECONDS) continue;
    history.tempHistory[history.currentIndex] = temperatureF;
    history.humidityHistory[history.currentIndex] = relativeHumidity;
    history.epoch[history.currentIndex] = (uint32_t)epochValue;
    history.currentIndex = (history.currentIndex + 1) % HISTORY_SIZE;
    if (history.sampleCount < HISTORY_SIZE) history.sampleCount++;
  }
  file.close();
  history.initialized = history.sampleCount >= HISTORY_SIZE;
  Serial.printf("[HISTORY] Restored %d temperature/humidity point(s).\n",
                history.sampleCount);
  persistHistory();
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

  if (savedVocStateValid) {
    error = sen66.setVocAlgorithmState(savedVocState, VOC_STATE_SIZE);
    Serial.printf("[SEN66] VOC state restore: %s (%d)\n",
                  error == NO_ERROR ? "OK" : "failed", error);
  }

  error = sen66.startContinuousMeasurement();
  if (error != NO_ERROR) {
    Serial.printf("[SEN66] Start failed: %d\n", error);
    return false;
  }
  sen66MeasurementRunning = true;
  sen66StartedMs = millis();
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
  recordSensorComparison();
  return true;
}

bool saveVocAlgorithmState(bool forceWrite = false) {
  if (!sen66MeasurementRunning && !savedVocStateValid) return false;
  uint8_t state[VOC_STATE_SIZE] = {0};
  int16_t error = sen66.getVocAlgorithmState(state, VOC_STATE_SIZE);
  if (error != NO_ERROR) {
    Serial.printf("[SEN66] VOC state read failed: %d\n", error);
    return false;
  }
  memcpy(savedVocState, state, VOC_STATE_SIZE);
  savedVocStateValid = true;

  // Limit flash wear to at most one persisted baseline per hour. Stop/start
  // retains the state inside a powered SEN66, so more frequent writes add no value.
  if (forceWrite || millis() - lastVocStateSaveMs >= 3600000UL) {
    prefs.begin("dash", false);
    prefs.putBytes("voc_state", savedVocState, VOC_STATE_SIZE);
    prefs.end();
    lastVocStateSaveMs = millis();
    Serial.println("[SEN66] VOC learning state persisted (8 bytes).");
  }
  return true;
}

bool startSen66Measurement() {
  if (sen66MeasurementRunning) return true;
  int16_t error = sen66.startContinuousMeasurement();
  if (error != NO_ERROR) {
    Serial.printf("[SEN66] Duty-cycle start failed: %d\n", error);
    return false;
  }
  sen66MeasurementRunning = true;
  sen66StartedMs = millis();
  Serial.println("[SEN66] Measurement started.");
  return true;
}

bool stopSen66Measurement() {
  if (!sen66MeasurementRunning) return true;
  saveVocAlgorithmState(false);
  int16_t error = sen66.stopMeasurement();
  if (error != NO_ERROR) {
    Serial.printf("[SEN66] Duty-cycle stop failed: %d\n", error);
    return false;
  }
  sen66MeasurementRunning = false;
  Serial.println("[SEN66] Measurement stopped; VOC state remains in the powered sensor.");
  return true;
}

bool sen66ShouldRunNow() {
  if (!lowPowerMode || co2Calibration.state == CO2_CAL_STABILIZING ||
      co2Calibration.state == CO2_CAL_EXECUTING) return true;

  // Full conditioning window from xx:50:00 through the top-of-hour update.
  if (minuteVal >= 50) return true;

  // Otherwise run for the first 90 seconds of each ten-minute slot.
  int slotSeconds = (minuteVal % 10) * 60 + secondVal;
  return slotSeconds < 90;
}

void serviceSen66Power() {
  bool shouldRun = sen66ShouldRunNow();
  if (shouldRun && !sen66MeasurementRunning) startSen66Measurement();
  else if (!shouldRun && sen66MeasurementRunning) stopSen66Measurement();
}

const char* co2CalibrationStateName() {
  switch (co2Calibration.state) {
    case CO2_CAL_STABILIZING: return "stabilizing";
    case CO2_CAL_EXECUTING:   return "calibrating";
    case CO2_CAL_SUCCESS:     return "success";
    case CO2_CAL_FAILED:      return "failed";
    default:                  return "idle";
  }
}

bool co2InCalibrationBand() {
  return indoor.valid && indoor.co2 != 0xFFFF &&
         indoor.co2 >= CO2_CAL_MIN_PPM && indoor.co2 <= CO2_CAL_MAX_PPM;
}

void failCo2Calibration(const char* message) {
  co2Calibration.state = CO2_CAL_FAILED;
  co2Calibration.qualifyingSince = 0;
  strncpy(co2Calibration.message, message, sizeof(co2Calibration.message) - 1);
  co2Calibration.message[sizeof(co2Calibration.message) - 1] = '\0';
  Serial.printf("[SEN66 FRC] %s\n", co2Calibration.message);
}

void executeCo2Calibration() {
  co2Calibration.state = CO2_CAL_EXECUTING;
  strncpy(co2Calibration.message, "Applying forced CO2 recalibration...",
          sizeof(co2Calibration.message) - 1);

  int16_t stopError = sen66.stopMeasurement();
  if (stopError != NO_ERROR) {
    char message[96];
    snprintf(message, sizeof(message),
             "Could not stop SEN66 measurement (error %d).", stopError);
    failCo2Calibration(message);
    return;
  }
  sen66MeasurementRunning = false;

  // SEN6x datasheet requires at least 1400 ms in idle mode before FRC.
  delay(1500);
  uint16_t correction = 0;
  int16_t frcError =
    sen66.performForcedCo2Recalibration(CO2_CAL_TARGET_PPM, correction);
  int16_t startError = sen66.startContinuousMeasurement();
  sen66MeasurementRunning = (startError == NO_ERROR);
  if (sen66MeasurementRunning) sen66StartedMs = millis();
  indoor.valid = false;
  lastSensorReadMs = millis();

  if (frcError != NO_ERROR || correction == 0xFFFF) {
    char message[112];
    snprintf(message, sizeof(message),
             "Forced calibration failed (error %d, result 0x%04X). Measurement restart: %s.",
             frcError, correction, startError == NO_ERROR ? "OK" : "FAILED");
    failCo2Calibration(message);
    return;
  }
  if (startError != NO_ERROR) {
    char message[96];
    snprintf(message, sizeof(message),
             "Calibration was stored, but measurement restart failed (error %d).", startError);
    failCo2Calibration(message);
    return;
  }

  co2Calibration.correctionRaw = correction;
  co2Calibration.state = CO2_CAL_SUCCESS;
  co2Calibration.qualifyingSince = 0;
  int32_t correctionPpm = (int32_t)correction - 0x8000;
  snprintf(co2Calibration.message, sizeof(co2Calibration.message),
           "Calibration complete at %u ppm. Applied correction: %ld ppm.",
           CO2_CAL_TARGET_PPM, (long)correctionPpm);
  Serial.printf("[SEN66 FRC] %s Pressure %.0f hPa.\n",
                co2Calibration.message, co2Calibration.pressureHpa);
}

void serviceCo2Calibration() {
  if (co2Calibration.state != CO2_CAL_STABILIZING) return;

  if (!co2InCalibrationBand()) {
    co2Calibration.qualifyingSince = 0;
    snprintf(co2Calibration.message, sizeof(co2Calibration.message),
             "Waiting for a continuous %u-%u ppm outdoor reading; current CO2 is %s.",
             CO2_CAL_MIN_PPM, CO2_CAL_MAX_PPM,
             indoor.valid && indoor.co2 != 0xFFFF ?
             String(indoor.co2).c_str() : "unavailable");
    return;
  }

  unsigned long now = millis();
  if (co2Calibration.qualifyingSince == 0) {
    co2Calibration.qualifyingSince = now;
  }
  unsigned long elapsed = now - co2Calibration.qualifyingSince;
  unsigned long remaining =
    elapsed >= CO2_CAL_STABILIZE_MS ? 0 :
    (CO2_CAL_STABILIZE_MS - elapsed + 999) / 1000;
  snprintf(co2Calibration.message, sizeof(co2Calibration.message),
           "Outdoor reference is in range. Keep the sensor undisturbed for %lu more seconds.",
           remaining);
  if (elapsed >= CO2_CAL_STABILIZE_MS) executeCo2Calibration();
}

// ===== BATTERY =====
float readBatteryVoltage() {
  // analogReadMilliVolts() uses the ESP32 ADC calibration data. Average 32
  // readings and discard the extrema to suppress Wi-Fi/charger switching noise.
  uint32_t sum = 0;
  uint32_t lo = UINT32_MAX;
  uint32_t hi = 0;
  for (int i = 0; i < 32; i++) {
    uint32_t mv = analogReadMilliVolts(BAT_ADC_PIN);
    sum += mv;
    lo = min(lo, mv);
    hi = max(hi, mv);
    delayMicroseconds(250);
  }
  float adcMv = (sum - lo - hi) / 30.0f;
  return adcMv * BATTERY_DIVIDER_RATIO * BATTERY_CALIBRATION / 1000.0f;
}

float batteryVoltageToSoc(float volts) {
  // Resting-voltage interpolation for a conventional 4.20 V Li-ion 18650.
  // Values under load are intentionally smoothed elsewhere.
  static const float v[] = {3.20f,3.40f,3.55f,3.65f,3.72f,3.78f,3.84f,3.90f,3.96f,4.05f,4.20f};
  static const float p[] = {0,5,10,20,30,40,50,60,70,85,100};
  if (volts <= v[0]) return 0.0f;
  if (volts >= v[10]) return 100.0f;
  for (int i = 1; i < 11; i++) {
    if (volts <= v[i]) {
      float f = (volts - v[i-1]) / (v[i] - v[i-1]);
      return p[i-1] + f * (p[i] - p[i-1]);
    }
  }
  return 0.0f;
}

void updateBatteryState(float measuredVoltage) {
  batteryVoltage = measuredVoltage;
  float instantSoc = batteryVoltageToSoc(measuredVoltage);
  batterySoc = batteryStateInitialized ?
               batterySoc * 0.90f + instantSoc * 0.10f : instantSoc;
  batteryStateInitialized = true;
}

void serviceUsbHostState(bool forceCheck = false) {
  unsigned long now = millis();
  if (!forceCheck && now - lastUsbCheckMs < USB_STATUS_SAMPLE_MS) return;
  lastUsbCheckMs = now;
  bool plugged = false;
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  plugged = Serial.isPlugged();
#endif

  if (forceCheck) {
    bool previousState = usbHostConnected;
    usbCandidateConnected = plugged;
    usbCandidateChangedMs = now;
    usbHostConnected = plugged;
    if (usbHostConnected != previousState) {
      Serial.printf("[POWER] USB host %s\n",
                    usbHostConnected ? "connected" : "disconnected");
    }
  } else {
    if (plugged != usbCandidateConnected) {
      usbCandidateConnected = plugged;
      usbCandidateChangedMs = now;
    }
    unsigned long confirmationMs = usbCandidateConnected ?
      USB_CONNECT_CONFIRM_MS : USB_DISCONNECT_CONFIRM_MS;
    if (usbCandidateConnected != usbHostConnected &&
        now - usbCandidateChangedMs >= confirmationMs) {
      usbHostConnected = usbCandidateConnected;
      Serial.printf("[POWER] USB host %s\n",
                    usbHostConnected ? "connected" : "disconnected");
    }
  }

  // Cell voltage cannot distinguish a full battery from charge-only USB power.
  // Use the hardware USB host signal as the only automatic external-power input.
  externalPowerLikely = usbHostConnected;
  lowPowerMode = !externalPowerLikely;
}

bool displayInteractive() {
  return (long)(displayInteractiveUntilMs - millis()) > 0;
}

void activateDisplayInteractiveWindow() {
  displayInteractiveUntilMs = millis() + DISPLAY_INTERACTIVE_WINDOW_MS;
  sleepWakeUntil = max(sleepWakeUntil, displayInteractiveUntilMs);
  if (!displayHighPower) {
    RlcdPort.RLCD_SetPowerMode(true);
    displayHighPower = true;
  }
  lastDisplayUpdateMs = 0;
}

void serviceDisplayPowerMode() {
  bool requestedHighPower = externalPowerLikely || displayInteractive();
  if (requestedHighPower == displayHighPower) return;
  RlcdPort.RLCD_SetPowerMode(requestedHighPower);
  displayHighPower = requestedHighPower;
  Serial.printf("[DISPLAY] %s mode\n",
                requestedHighPower ? "high-power interactive" : "low-power 0.5 Hz");
}

void logBatteryTrend() {
  if (lastBatteryTrendMs != 0 && millis() - lastBatteryTrendMs < 1800000UL) return;
  lastBatteryTrendMs = millis();
  batteryTrend[batteryTrendHead] = {batterySoc, millis()};
  batteryTrendHead = (batteryTrendHead + 1) % 48;
  if (batteryTrendCount < 48) batteryTrendCount++;
}

float estimatedHoursTo20Percent() {
  if (externalPowerLikely || batterySoc <= 20.0f || batteryTrendCount < 5) return NAN;
  int oldest = (batteryTrendHead - batteryTrendCount + 48) % 48;
  const BatteryTrendPoint& first = batteryTrend[oldest];
  const BatteryTrendPoint& last =
    batteryTrend[(batteryTrendHead - 1 + 48) % 48];
  float hours = (last.atMs - first.atMs) / 3600000.0f;
  if (hours < 2.0f) return NAN;
  float dropPerHour = (first.soc - last.soc) / hours;
  if (dropPerHour <= 0.05f) return NAN;
  return (batterySoc - 20.0f) / dropPerHour;
}

// Returns true if the display should currently be sleeping.
// Handles midnight-spanning windows (e.g. 23:00 -> 06:00).
// Overridden to false during the 60-second button interaction window.
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

  int locPos = payload.indexOf("\"location\":");
  String resolvedTimezone = pstr("\"tz_id\":\"", locPos);
  String resolvedLocalTime = pstr("\"localtime\":\"", locPos);
  time_t resolvedLocalEpoch =
    (time_t)pint("\"localtime_epoch\":", locPos, curPos);
  if (!applyWeatherLocationTime(resolvedTimezone, resolvedLocalTime,
                                resolvedLocalEpoch)) {
    Serial.println("[TIME] Weather location did not provide a valid timezone/local time.");
  }

  weatherData.currentTemp = pfloat("\"temp_c\":",      curPos, fcastPos);
  weatherData.feelsLike   = pfloat("\"feelslike_c\":", curPos, fcastPos);
  weatherData.humidity    = pint  ("\"humidity\":",    curPos, fcastPos);
  weatherData.windSpeed   = pfloat("\"wind_kph\":",    curPos, fcastPos);
  weatherData.windDir     = pstr  ("\"wind_dir\":\"",  curPos);
  weatherData.condition   = pstr  ("\"text\":\"",      curPos);
  weatherData.precipMM    = pfloat("\"precip_mm\":",   curPos, fcastPos);
  weatherData.pressureHpa = pfloat("\"pressure_mb\":", curPos, fcastPos);

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

bool activeCoordinates(float& latitude, float& longitude) {
  String value(activeWeatherLocation);
  int comma = value.indexOf(',');
  if (comma <= 0) return false;
  char* latEnd = nullptr;
  char* lonEnd = nullptr;
  latitude = strtof(value.substring(0, comma).c_str(), &latEnd);
  longitude = strtof(value.substring(comma + 1).c_str(), &lonEnd);
  return latEnd && *latEnd == '\0' && lonEnd && *lonEnd == '\0' &&
         latitude >= -90.0f && latitude <= 90.0f &&
         longitude >= -180.0f && longitude <= 180.0f;
}

bool consumeOwmCallBudget() {
  int today = rtc.getDay();
  if (today != owmCallDay) {
    owmCallDay = today;
    owmCallsToday = 0;
  }
  // Leave a deliberate 10% safety margin below the user's 1,000-call ceiling.
  if (owmCallsToday >= 900) return false;
  owmCallsToday++;
  return true;
}

uint32_t hashOwmAlertId(uint32_t hash, const String& value) {
  if (hash == 0) hash = 2166136261UL;
  for (size_t i = 0; i < value.length(); i++) {
    hash ^= (uint8_t)value[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool fetchOpenWeatherMapData() {
  if (!wifiConnected || strlen(OpenWeatherMapApiKey) < 8) return false;
  float lat = 0.0f, lon = 0.0f;
  if (!activeCoordinates(lat, lon)) {
    Serial.println("[OWM] Location must be decimal latitude,longitude.");
    return false;
  }

  bool minuteOk = false;
  if (consumeOwmCallBudget()) {
    HTTPClient http;
    String url = "https://api.openweathermap.org/data/4.0/onecall/timeline/1min?lat=" +
                 String(lat, 6) + "&lon=" + String(lon, 6) +
                 "&appid=" + String(OpenWeatherMapApiKey);
    http.begin(url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code == 200) {
      JsonDocument filter;
      filter["data"][0]["dt"] = true;
      filter["data"][0]["precipitation"] = true;
      JsonDocument doc;
      DeserializationError error =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      if (!error) {
        owmMinute.count = 0;
        owmMinute.maxPrecipitation = 0.0f;
        owmMinute.rainNext30 = false;
        for (JsonObject item : doc["data"].as<JsonArray>()) {
          if (owmMinute.count >= 60) break;
          int i = owmMinute.count++;
          owmMinute.epoch[i] = item["dt"] | 0;
          owmMinute.precipitation[i] = item["precipitation"] | 0.0f;
          owmMinute.maxPrecipitation =
            max(owmMinute.maxPrecipitation, owmMinute.precipitation[i]);
          if (i < 30 && owmMinute.precipitation[i] > 0.01f)
            owmMinute.rainNext30 = true;
        }
        minuteOk = owmMinute.count > 0;
        owmMinute.valid = minuteOk;
        if (minuteOk) owmMinute.lastUpdate = millis();
      } else {
        Serial.printf("[OWM] Minute JSON error: %s\n", error.c_str());
      }
    } else {
      Serial.printf("[OWM] Minute request HTTP %d\n", code);
    }
    http.end();
  }

  bool quarterHourOk = false;
  if (consumeOwmCallBudget()) {
    HTTPClient http;
    String url = "https://api.openweathermap.org/data/4.0/onecall/timeline/15min?lat=" +
                 String(lat, 6) + "&lon=" + String(lon, 6) +
                 "&units=imperial&appid=" + String(OpenWeatherMapApiKey);
    http.begin(url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code == 200) {
      JsonDocument filter;
      filter["data"][0]["temp"] = true;
      filter["data"][0]["pop"] = true;
      filter["data"][0]["weather"][0]["id"] = true;
      filter["data"][0]["alerts"][0] = true;
      JsonDocument doc;
      DeserializationError error =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      if (!error && !doc["data"][0].isNull()) {
        owmMinute.outsideTempF = doc["data"][0]["temp"] | NAN;
        float probability = doc["data"][0]["pop"] | 0.0f;
        owmMinute.rainChance =
          constrain((int)lroundf(probability * 100.0f), 0, 100);
        owmMinute.outsideValid = isfinite(owmMinute.outsideTempF);
        uint32_t alertHash = 0;
        for (JsonVariant alert : doc["data"][0]["alerts"].as<JsonArray>()) {
          String identifier = alert.as<String>();
          alertHash = hashOwmAlertId(alertHash, identifier);
        }
        owmMinute.alertHash = alertHash;
        owmMinute.severeWeather = alertHash != 0;
        quarterHourOk = owmMinute.outsideValid;
      } else {
        Serial.printf("[OWM] 15-minute JSON error: %s\n", error.c_str());
      }
    } else {
      Serial.printf("[OWM] 15-minute request HTTP %d\n", code);
    }
    http.end();
  }

  bool airOk = false;
  if (consumeOwmCallBudget()) {
    HTTPClient http;
    String url = "https://api.openweathermap.org/data/2.5/air_pollution?lat=" +
                 String(lat, 6) + "&lon=" + String(lon, 6) +
                 "&appid=" + String(OpenWeatherMapApiKey);
    http.begin(url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code == 200) {
      JsonDocument filter;
      filter["list"][0]["main"]["aqi"] = true;
      filter["list"][0]["components"]["pm2_5"] = true;
      filter["list"][0]["components"]["pm10"] = true;
      JsonDocument doc;
      DeserializationError error =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      if (!error && !doc["list"][0].isNull()) {
        float pm25 = doc["list"][0]["components"]["pm2_5"] | 0.0f;
        float pm10 = doc["list"][0]["components"]["pm10"] | 0.0f;
        weatherData.pm25 = pm25;
        weatherData.pmAqi = max(pm25ToAqi(pm25), pm10ToAqi(pm10));
        weatherData.airQualityIndex = doc["list"][0]["main"]["aqi"] | 0;
        airOk = true;
      } else {
        Serial.printf("[OWM] Air JSON error: %s\n", error.c_str());
      }
    } else {
      Serial.printf("[OWM] Air request HTTP %d\n", code);
    }
    http.end();
  }

  bool rainEventNow = (minuteOk && owmMinute.rainNext30) ||
                      (quarterHourOk && owmMinute.rainChance > 0);
  bool newRainEvent = rainEventNow && rainEventArmed;
  bool newSevereEvent = owmMinute.severeWeather &&
                        owmMinute.alertHash != acknowledgedAlertHash;
  if (minuteOk || quarterHourOk) {
    if (rainEventNow && rainEventArmed) {
      rainBoostUntilMs = millis() + 7200000UL;
      rainEventArmed = false;
      Serial.println("[OWM] Rain in next 30 minutes; 10-minute updates enabled for two hours.");
    } else if (!rainEventNow) {
      rainEventArmed = true;
      rainBoostUntilMs = 0;
    }
  }
  if (owmMinute.severeWeather)
    acknowledgedAlertHash = owmMinute.alertHash;
  else
    acknowledgedAlertHash = 0;

  if ((newRainEvent || newSevereEvent) && pageIsEnabled(5)) {
    currentPage = 5;
    lastDisplayUpdateMs = 0;
    Serial.printf("[OWM] New %s event. Showing the next-60-minutes page once.\n",
                  newSevereEvent ? "severe-weather" : "rain");
  }
  Serial.printf("[OWM] Minute=%s 15-minute=%s air=%s calls today=%u\n",
                minuteOk ? "OK" : "failed",
                quarterHourOk ? "OK" : "failed",
                airOk ? "OK" : "failed", owmCallsToday);
  return minuteOk || quarterHourOk || airOk;
}

bool fetchAllOnlineData() {
  bool weatherOk = fetchWeatherData();
  bool owmOk = fetchOpenWeatherMapData();
  if (weatherOk || owmOk) lastOnlineUpdateMs = millis();
  return weatherOk || owmOk;
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

static constexpr int UI_HEADER_X = 8;
static constexpr int UI_HEADER_Y = 8;
static constexpr int UI_HEADER_W = 384;
static constexpr int UI_HEADER_H = 26;

void drawCenteredTextInRect(const GFXfont* textFont, int x, int width,
                            int baseline, const char* text) {
  canvas.setFont(textFont);
  int16_t x1, y1;
  uint16_t textWidth, textHeight;
  canvas.getTextBounds(text, 0, baseline, &x1, &y1, &textWidth, &textHeight);
  canvas.setCursor(x + (width - (int)textWidth) / 2 - x1, baseline);
  canvas.print(text);
}

void beginDisplayPage(const char* title) {
  canvas.fillScreen(0);
  canvas.setTextWrap(false);
  canvas.drawRect(0, 0, W, H, 1);
  canvas.drawRect(1, 1, W - 2, H - 2, 1);
  canvas.fillRect(UI_HEADER_X, UI_HEADER_Y, UI_HEADER_W, UI_HEADER_H, 1);

  canvas.setTextColor(0);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(UI_HEADER_X + 7, UI_HEADER_Y + 19);
  canvas.print(title);

  char clockText[6];
  snprintf(clockText, sizeof(clockText), "%02d:%02d", hour24, minuteVal);
  int16_t x1, y1;
  uint16_t textWidth, textHeight;
  canvas.getTextBounds(clockText, 0, UI_HEADER_Y + 19,
                       &x1, &y1, &textWidth, &textHeight);
  canvas.setCursor(UI_HEADER_X + UI_HEADER_W - 7 - textWidth - x1,
                   UI_HEADER_Y + 19);
  canvas.print(clockText);
  canvas.setTextColor(1);
}

void drawMetricBox(int x, int y, int width, int height,
                   const char* label, const String& value,
                   const GFXfont* valueFont) {
  canvas.drawRect(x, y, width, height, 1);
  canvas.setTextColor(1);
  drawCenteredTextInRect(&FONT_SMALL, x, width, y + 18, label);
  drawCenteredTextInRect(valueFont, x, width, y + height - 11, value.c_str());
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
  beginDisplayPage("INDOOR AIR");

  // Three primary measurements.
  const int topY = 42, topH = 82, topW = 122;
  canvas.drawRect(8, topY, topW, topH, 1);
  canvas.drawRect(139, topY, topW, topH, 1);
  canvas.drawRect(270, topY, topW, topH, 1);
  canvas.setFont(&FONT_SMALL);
  canvas.setCursor(15, 61); canvas.print("TEMP (F)");
  canvas.setCursor(146, 61); canvas.print("RH (%)");
  canvas.setCursor(277, 61); canvas.print("CO2 (ppm)");

  canvas.setFont(&FONT_LARGE);
  canvas.setCursor(15, 101);
  if (indoor.valid) {
    canvas.print(cToF(indoor.temperature), 1);
  } else canvas.print("--");
  canvas.setCursor(146, 101);
  if (indoor.valid) canvas.print((int)indoor.humidity);
  else canvas.print("--");
  canvas.setCursor(277, 101);
  if (indoor.valid && indoor.co2 != 0xFFFF) canvas.print(indoor.co2);
  else canvas.print("--");

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
    canvas.print("SEN66: WAITING");
  } else if (isnan(indoor.vocIndex) || isnan(indoor.noxIndex) || indoor.co2 == 0xFFFF) {
    canvas.print("SEN66: PREHEATING");
  } else {
    canvas.print("SEN66: READY");
  }
  canvas.setCursor(244, 280);
  canvas.print(batteryVoltage, 2); canvas.print("V ");
  canvas.print((int)lroundf(batterySoc)); canvas.print("%");
  drawWiFiIcon(350, 265, wifiRSSI);

  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 1: ANALOGUE CLOCK =====
void drawAnalogClockPage() {
  beginDisplayPage("ANALOG CLOCK");

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
  char infoBuf[28];
  snprintf(infoBuf, sizeof(infoBuf), "%.1fF  %d%%  %.2fV",
    cToF(temperature), (int)humidity, batteryVoltage);
  int16_t ix1, iy1; uint16_t itw, ith;
  canvas.getTextBounds(infoBuf, 0, 290, &ix1, &iy1, &itw, &ith);
  canvas.setCursor(W - 14 - itw - ix1, 290); canvas.print(infoBuf);

  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 2: NORTH AMERICAN TIME ZONES =====
void drawTimeZonePage() {
  beginDisplayPage("NORTH AMERICAN TIME");

  time_t utcNow = time(nullptr);
  struct tm utcInfo;
  gmtime_r(&utcNow, &utcInfo);
  int year = utcInfo.tm_year + 1900;
  auto firstSunday = [&](int month) {
    int weekday = (int)((daysFromCivil(year, (unsigned)month, 1) + 4) % 7);
    if (weekday < 0) weekday += 7;
    return 1 + ((7 - weekday) % 7);
  };
  int secondSundayMarch = firstSunday(3) + 7;
  int firstSundayNovember = firstSunday(11);
  int month = utcInfo.tm_mon + 1;
  bool dst = month > 3 && month < 11;
  if (month == 3) dst = utcInfo.tm_mday >= secondSundayMarch;
  if (month == 11) dst = utcInfo.tm_mday < firstSundayNovember;
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
  beginDisplayPage("CURRENT CONDITIONS");

  if (!weatherData.valid) {
    canvas.setFont(&FONT_LARGE); canvas.setTextColor(1);
    canvas.setCursor(60, 130); canvas.print("NO WEATHER DATA");
    canvas.setFont(&FONT_SMALL); canvas.setCursor(110, 165); canvas.print("Press KEY button");
    pushCanvasToRLCD(displayInvert); return;
  }

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

// ===== PAGE 6: 3-DAY FORECAST =====
void drawForecastPage() {
  beginDisplayPage("3-DAY FORECAST");

  if (!weatherData.valid) {
    canvas.setFont(&FONT_LARGE); canvas.setTextColor(1);
    canvas.setCursor(60, 130); canvas.print("NO WEATHER DATA");
    canvas.setFont(&FONT_SMALL); canvas.setCursor(110, 165); canvas.print("Press KEY button");
    pushCanvasToRLCD(displayInvert); return;
  }

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
    const int conditionCharsPerLine = 11;
    if (fcond.length() > conditionCharsPerLine) {
      int sp = fcond.lastIndexOf(' ', conditionCharsPerLine);
      if (sp > 0) {
        canvas.setCursor(cx+6, y+28); canvas.print(fcond.substring(0, sp));
        canvas.setCursor(cx+6, y+42);
        canvas.print(fcond.substring(sp + 1, min((int)fcond.length(), sp + conditionCharsPerLine + 1)));
      } else {
        canvas.setCursor(cx+6, y+28); canvas.print(fcond.substring(0, conditionCharsPerLine));
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

// ===== PAGE 11: SYSTEM INFO =====
void drawSystemPage() {
  beginDisplayPage("SYSTEM INFORMATION");

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
  String ssidStr = wifiConnected ? String(activeSsid) : "--";
  if (ssidStr.length() > 20) ssidStr = ssidStr.substring(0, 20); // FONT_SMALL safe up to ~26 chars
  String chanStr  = wifiConnected ? String(WiFi.channel()) : "--";
  String tempDelta = sensorComparison.pairCount > 0 ?
    String(sensorComparison.meanTempDeltaC * 1.8, 2) + " F" : "No pairs";
  String humidityDelta = sensorComparison.pairCount > 0 ?
    String(sensorComparison.meanHumidityDelta, 2) + " %" : "No pairs";

  int gy = gy0;
  drawDetail(lx, gy, "WIFI",         wifiVal);
  drawDetail(rx, gy, "IP ADDRESS",   ipStr);       gy += rowH;
  drawDetail(lx, gy, "WEATHER",      weatherData.valid ? "Fresh" : "No Data");
  drawDetail(rx, gy, "WX REFRESH",   wxVal);        gy += rowH;
  drawDetail(lx, gy, "UPTIME",       String(uptimeStr));
  drawDetail(rx, gy, "NTP SYNC",     ntpStr);       gy += rowH;
  drawDetail(lx, gy, "NETWORK",      ssidStr, true);
  drawDetail(rx, gy, "CHANNEL",      chanStr);      gy += rowH;
  drawDetail(lx, gy, "SEN66-SHTC3 TEMP", tempDelta, true);
  drawDetail(rx, gy, sensorComparison.qualified ? "RH DELTA (QUALIFIED)" : "RH DELTA (LEARNING)",
             humidityDelta, true);

  canvas.fillRect(8, 264, 384, 2, 1);
  canvas.setFont(&FONT_SMALL); canvas.setCursor(12, 287);
  canvas.print("BATTERY ");
  canvas.print(batteryVoltage, 3); canvas.print(" V  ");
  canvas.print((int)lroundf(batterySoc)); canvas.print("%  ");
  canvas.print(lowPowerMode ? "LOW POWER" : "EXTERNAL POWER");
  pushCanvasToRLCD(displayInvert);
}

// ===== PAGE 12: COMPLETE SEN66 OUTPUT =====
void drawSen66DetailsPage() {
  beginDisplayPage("SEN66 SENSOR OUTPUT");

  const int lx = 8, rx = 200, cellW = 192, rowH = 44, gy0 = 40;
  canvas.fillRect(198, gy0, 2, rowH * 5, 1);
  for (int r = 1; r < 5; r++) canvas.drawFastHLine(8, gy0 + r * rowH, 384, 1);

  auto reading = [&](int x, int y, const char* label, const String& value) {
    drawCenteredTextInRect(&FONT_SMALL, x, cellW, y + 14, label);
    drawCenteredTextInRect(&FONT_MEDIUM, x, cellW, y + 36, value.c_str());
  };
  String unavailable = "--";
  reading(lx, gy0, "TEMPERATURE (F)", indoor.valid ? String(cToF(indoor.temperature), 1) : unavailable);
  reading(rx, gy0, "HUMIDITY (%)", indoor.valid ? String(indoor.humidity, 1) : unavailable);
  reading(lx, gy0 + rowH, "CO2 (ppm)", indoor.valid && indoor.co2 != 0xFFFF ? String(indoor.co2) : unavailable);
  reading(rx, gy0 + rowH, "VOC INDEX", indoor.valid ? String(indoor.vocIndex, 1) : unavailable);
  reading(lx, gy0 + rowH * 2, "NOx INDEX", indoor.valid ? String(indoor.noxIndex, 1) : unavailable);
  reading(rx, gy0 + rowH * 2, "INDOOR AQI", indoor.valid ? String(indoor.particleAqi) : unavailable);
  reading(lx, gy0 + rowH * 3, "PM1.0 (ug/m3)", indoor.valid ? String(indoor.pm1, 1) : unavailable);
  reading(rx, gy0 + rowH * 3, "PM2.5 (ug/m3)", indoor.valid ? String(indoor.pm25, 1) : unavailable);
  reading(lx, gy0 + rowH * 4, "PM4.0 (ug/m3)", indoor.valid ? String(indoor.pm4, 1) : unavailable);
  reading(rx, gy0 + rowH * 4, "PM10 (ug/m3)", indoor.valid ? String(indoor.pm10, 1) : unavailable);

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
  beginDisplayPage("NEXT 6 HOURS");

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

// ===== PAGE 5: 60-MINUTE PRECIPITATION =====
void drawMinuteCastPage() {
  beginDisplayPage("NEXT 60 MINUTES");

  const int boxY = 42, boxH = 58, boxW = 91;
  const char* labels[] = {"OUT (F)", "IN (F)", "RH (%)", "CO2 (ppm)"};
  for (int i = 0; i < 4; i++) {
    int x = 8 + i * 97;
    String value = "--";
    if (i == 0) {
      if (owmMinute.outsideValid) value = String(owmMinute.outsideTempF, 1);
    } else if (i == 1) {
      value = String(cToF(temperature), 1);
    } else if (i == 2) {
      value = String(humidity, 0);
    } else if (indoor.valid && indoor.co2 != 0xFFFF) {
      value = String(indoor.co2);
    }
    drawMetricBox(x, boxY, boxW, boxH, labels[i], value, &FONT_MEDIUM);
  }

  const int gx = 24, gy = 122, gw = 352, gh = 112;
  canvas.drawRect(gx, gy, gw, gh, 1);
  canvas.setFont(&FONT_SMALL);
  if (!owmMinute.valid || owmMinute.count == 0) {
    canvas.setCursor(84, 190); canvas.print("NO MINUTE FORECAST DATA");
  } else {
    float scaleMax = max(0.10f, owmMinute.maxPrecipitation);
    for (int i = 0; i < owmMinute.count; i++) {
      int x = gx + 2 + (i * (gw - 4)) / 60;
      int nextX = gx + 2 + ((i + 1) * (gw - 4)) / 60;
      int bw = max(1, nextX - x);
      int bh = (int)lroundf((owmMinute.precipitation[i] / scaleMax) * (gh - 20));
      if (bh > 0) canvas.fillRect(x, gy + gh - 2 - bh, bw, bh, 1);
    }
    canvas.setCursor(gx, gy - 6); canvas.print(scaleMax, 1); canvas.print(" mm/h");
    for (int m = 0; m <= 60; m += 10) {
      int x = gx + (m * gw) / 60;
      canvas.drawLine(x, gy + gh, x, gy + gh + 4, 1);
      canvas.setCursor(constrain(x - 7, 4, 370), gy + gh + 18);
      canvas.print(m);
    }
  }

  canvas.setCursor(12, 284);
  if (owmMinute.valid) {
    canvas.print(owmMinute.rainNext30 ? "RAIN <30M" : "DRY <30M");
  } else canvas.print("OWM OFFLINE");

  const int batteryX = 326, batteryY = 270, batteryW = 59, batteryH = 20;
  canvas.drawRect(batteryX, batteryY, batteryW, batteryH, 1);
  canvas.fillRect(batteryX + batteryW, batteryY + 6, 4, 8, 1);
  char socText[8];
  snprintf(socText, sizeof(socText), "%d%%", (int)lroundf(batterySoc));
  canvas.setFont(&FONT_SMALL);
  int16_t x1, y1; uint16_t textW, textH;
  canvas.getTextBounds(socText, 0, 0, &x1, &y1, &textW, &textH);
  canvas.setCursor(batteryX + (batteryW - textW) / 2, batteryY + 15);
  canvas.print(socText);
  pushCanvasToRLCD(displayInvert);
}

// ===== GRAPH HELPERS =====
GraphBounds calcGraphBounds(float* data, int startIndex, int count,
                            float minRange, float pad) {
  GraphBounds b = { 999.0f, -999.0f, 0.0f };
  for (int i = 0; i < count; i++) {
    float value = data[(startIndex + i) % HISTORY_SIZE];
    if (value < b.mn) b.mn = value;
    if (value > b.mx) b.mx = value;
  }
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

// ===== PAGE 9: TEMP GRAPH =====
void drawTempGraphPage() {
  beginDisplayPage("INDOOR TEMPERATURE");
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(14, 60); canvas.print("CURRENT");
  // Current value
  canvas.setFont(&FONT_LARGE); canvas.setCursor(96, 64); canvas.print(cToF(temperature), 1);
  canvas.setFont(&FONT_SMALL); canvas.print(" F");
  int startIndex = historyStartIndex();
  int tTrend = calcTrend(history.tempHistory, startIndex, history.sampleCount);
  drawTrendArrow(375, 52, tTrend);

  if (history.sampleCount < 2) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(85, 155); canvas.print("COLLECTING DATA");
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(95, 178); canvas.print("Graph available in 15 mins");
    pushCanvasToRLCD(displayInvert); return;
  }

  int gX = 44, gY = 76, gW = 336, gH = 144;
  GraphBounds b = calcGraphBounds(history.tempHistory, startIndex,
                                  history.sampleCount, 5.0f, 1.0f);
  drawEnhancedGraph(history.tempHistory, startIndex, history.sampleCount,
                    gX, gY, gW, gH, b, "F");

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

// ===== PAGE 10: HUMIDITY GRAPH =====
void drawHumidityGraphPage() {
  beginDisplayPage("INDOOR HUMIDITY");
  canvas.setFont(&FONT_SMALL); canvas.setTextColor(1);
  canvas.setCursor(14, 60); canvas.print("CURRENT");
  // Current value
  canvas.setFont(&FONT_LARGE); canvas.setCursor(96, 64); canvas.print((int)humidity);
  canvas.setFont(&FONT_SMALL); canvas.print(" %");
  int startIndex = historyStartIndex();
  int hTrend = calcTrend(history.humidityHistory, startIndex, history.sampleCount);
  drawTrendArrow(375, 52, hTrend);

  if (history.sampleCount < 2) {
    canvas.setFont(&FONT_MEDIUM); canvas.setCursor(85, 155); canvas.print("COLLECTING DATA");
    canvas.setFont(&FONT_SMALL);  canvas.setCursor(95, 178); canvas.print("Graph available in 15 mins");
    pushCanvasToRLCD(displayInvert); return;
  }

  int gX = 44, gY = 76, gW = 336, gH = 144;
  GraphBounds b = calcGraphBounds(history.humidityHistory, startIndex,
                                  history.sampleCount, 10.0f, 2.0f);
  if (b.mn < 0.0f) b.mn = 0.0f; if (b.mx > 100.0f) b.mx = 100.0f;
  b.rng = b.mx - b.mn; if (b.rng < 1.0f) b.rng = 1.0f;
  drawEnhancedGraph(history.humidityHistory, startIndex, history.sampleCount,
                    gX, gY, gW, gH, b, "%");

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

// ===== PAGE 7: EARTH & SEASONS =====
void drawSeasonsPage() {
  beginDisplayPage("SEASONS");

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
// ===== PAGE 8: SEASONS ORBIT DIAGRAM =====
void drawSeasonsOrbitPage() {
  beginDisplayPage("SEASON ORBIT");

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

// ===== PAGE 13: TIMERS / STOPWATCH / ALARMS =====
void drawTimersPage() {
  beginDisplayPage("TIMERS & ALARMS");

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
    case 5:  drawMinuteCastPage();       break;
    case 6:  drawForecastPage();         break;
    case 7:  drawSeasonsPage();          break;
    case 8:  drawSeasonsOrbitPage();     break;
    case 9:  drawTempGraphPage();        break;
    case 10: drawHumidityGraphPage();    break;
    case 11: drawSystemPage();           break;
    case 12: drawSen66DetailsPage();     break;
    case 13: drawTimersPage();           break;
  }
}

// ===== WIFI & NTP =====
void connectWiFi() {
  if (strlen(activeSsid) == 0) {
    wifiConnected = false;
    return;
  }
  Serial.print("Connecting: "); Serial.println(activeSsid);
  WiFi.mode(provisioningActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(activeSsid, activePassword);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(500); Serial.print("."); attempts++; }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK"); Serial.println(WiFi.localIP());
    wifiConnected = true; wifiRSSI = WiFi.RSSI();
  } else { Serial.println("\nWiFi failed"); wifiConnected = false; }
}

void syncRTCWithNTP() {
  if (!wifiConnected) return;
  // NTP supplies UTC. The current coordinate-derived fixed offset converts it
  // to location-local time before the RTC is updated.
  configTime(0, 0, ntpServer);
  setenv("TZ", activePosixTZ, 1);
  tzset();

  struct tm timeinfo; int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 10) { delay(500); retries++; }
  if (getLocalTime(&timeinfo)) {
    // Push current location-local time to the hardware RTC.
    rtc.setTime(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    rtc.setDate(timeinfo.tm_wday, timeinfo.tm_mday, timeinfo.tm_mon+1, timeinfo.tm_year+1900);
    ntpLastSync = millis();

    Serial.printf("RTC synced for %s (%s)\n",
                  activeTimezoneId, getTZLabel());
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
  prefs.putUChar("page_schema", 2);
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
  uint16_t storedMask = prefs.getUShort("page_mask", ALL_PAGE_MASK);
  uint8_t pageSchema = prefs.getUChar("page_schema", 1);
  if (pageSchema < 2) {
    // Schema 2 inserts the minute forecast at page 5.
    pageEnabledMask = (storedMask & 0x001FU) |
                      ((storedMask & 0x1FE0U) << 1) | (1U << 5);
  } else {
    pageEnabledMask = storedMask;
  }
  pageEnabledMask &= ALL_PAGE_MASK;
  if (pageEnabledMask == 0) pageEnabledMask = ALL_PAGE_MASK;
  sleepEnabled  = prefs.getBool("sleep_en",   false);
  sleepFromHour = prefs.getInt ("sleep_from", 23);
  sleepToHour   = prefs.getInt ("sleep_to",   6);
  String savedWeatherLocation = prefs.getString("weather_loc", weatherLocation);
  savedWeatherLocation.toCharArray(activeWeatherLocation, sizeof(activeWeatherLocation));
  String savedTimezoneId = prefs.getString("tz_id", "UTC");
  savedTimezoneId.toCharArray(activeTimezoneId, sizeof(activeTimezoneId));
  gmtOffset_sec = prefs.getLong("tz_off", 0);
  String savedSsid = prefs.getString("wifi_ssid", ssid);
  String savedPassword = prefs.getString("wifi_pass", password);
  savedSsid.toCharArray(activeSsid, sizeof(activeSsid));
  savedPassword.toCharArray(activePassword, sizeof(activePassword));
  if (prefs.getBytesLength("voc_state") == VOC_STATE_SIZE) {
    prefs.getBytes("voc_state", savedVocState, VOC_STATE_SIZE);
    savedVocStateValid = true;
  }
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
  configureFixedOffsetTimezone(gmtOffset_sec);
  if (pageSchema < 2) nvsSave();
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
    "Hourly Forecast","60-Minute Rain","3-Day Forecast","Seasons","Season Orbit",
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
         "<button class='btn' onclick=\"doAction('/wakenow','Waking...','Awake for 60 s')\">Wake Now</button>"
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
  float hoursTo20 = estimatedHoursTo20Percent();
  String runtimeEstimate = isnan(hoursTo20) ? "Learning" :
    (hoursTo20 < 48.0f ? String(hoursTo20, 1) + " hours" :
                         String(hoursTo20 / 24.0f, 1) + " days");
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
    "<div class='card'><h2>Battery Estimate</h2><div class='row'>" +
    metric("Battery voltage", String(batteryVoltage, 2)+" V") +
    metric("Estimated charge", String((int)lroundf(batterySoc))+"%") +
    metric("Power mode", lowPowerMode ? "Low power" : "External power") +
    metric("Estimated time to 20%", runtimeEstimate) +
    "</div><div class='k'>The runtime value uses the measured voltage trend. "
    "The board has no current monitor, so this value is not a coulomb-counted result.</div></div>"
    "<div class='card'><h2>Particle detail</h2><div class='row'>" +
    metric("Indoor PM2.5", indoor.valid ? String(indoor.pm25,1)+" ug/m3" : "--") +
    metric("Indoor PM10", indoor.valid ? String(indoor.pm10,1)+" ug/m3" : "--") +
    metric("Indoor particle AQI", indoor.valid ? String(indoor.particleAqi) : "--") +
    metric("Outdoor PM2.5 AQI", weatherData.valid ? String(weatherData.pmAqi) : "--") +
    "</div></div>"
    "<div class='card'><h2>Actions</h2><div class='row'>"
    "<button class='btn' onclick=\"fetch('/refresh').then(()=>location.reload())\">Refresh Weather and Air</button>"
    "<button class='btn sec' onclick=\"fetch('/syncntp').then(()=>location.reload())\">Sync NTP</button>"
    "<a class='btn sec' href='/timers'>Timers</a></div></div>"
    "<div class='card'><h2>Outdoor SEN66 CO2 Calibration</h2>"
    "<div class='k'>Persistent forced recalibration to a 400 ppm outdoor reference. The station first downloads "
    "current local pressure, applies pressure compensation, and then requires five uninterrupted minutes with "
    "the SEN66 reading in the 350-450 ppm reference band. Leaving the band restarts the timer.</div>"
    "<div class='row' style='margin-top:10px'>" +
    metric("Current CO2", indoor.valid && indoor.co2!=0xFFFF ?
           String(indoor.co2)+" ppm" : "--") +
    metric("Downloaded pressure", weatherData.valid && weatherData.pressureHpa>0 ?
           String(weatherData.pressureHpa,0)+" hPa" : "--") +
    "</div>"
    "<div class='sw' style='margin-top:10px'><span class='swlbl'>I confirm the complete SEN66 unit is outdoors "
    "in open, well-mixed air and away from people, vehicles, vents, and combustion sources.</span>"
    "<label class='switch'><input type='checkbox' id='cal_outdoor'><span class='slider'></span></label></div>"
    "<div class='row' style='margin-top:10px'>"
    "<button class='btn' id='cal_start' onclick='startCo2Calibration()'>Start 5-Min Calibration</button>"
    "<button class='btn sec' id='cal_cancel' onclick='cancelCo2Calibration()'>Cancel</button></div>"
    "<div id='cal_status' class='k' style='margin-top:10px'>Loading calibration status...</div></div>"
    "<div class='card'><h2>Weather Location</h2>"
    "<div class='k'>Enter decimal latitude and longitude, for example 40.7128,-74.0060.</div>"
    "<div class='row' style='margin-top:10px'>"
    "<input type='text' id='weather_location' maxlength='63' value='" + String(activeWeatherLocation) + "'>"
    "<button class='btn' onclick='setWeatherLocation()'>Update Location</button></div>"
    "<div class='row' style='margin-top:10px'>" +
    metric("Resolved timezone", String(activeTimezoneId)) +
    metric("Current UTC offset", String(getTZLabel())) +
    "</div><div id='weather_location_status' class='k'></div></div>"
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
    "async function startCo2Calibration(){let s=document.getElementById('cal_status');"
    "if(!document.getElementById('cal_outdoor').checked){s.textContent='Confirm that the complete sensor is outdoors first.';return;}"
    "if(!confirm('This writes a persistent forced CO2 calibration to the SEN66. Keep it outdoors and undisturbed for five minutes. Continue?'))return;"
    "s.textContent='Downloading current pressure...';"
    "try{let r=await fetch('/co2cal/start?confirmed=true',{method:'POST'});let t=await r.text();"
    "if(!r.ok)throw new Error(t);s.textContent=t;pollCo2Calibration()}catch(e){s.textContent=e.message}}"
    "async function cancelCo2Calibration(){let r=await fetch('/co2cal/cancel',{method:'POST'});"
    "document.getElementById('cal_status').textContent=await r.text();pollCo2Calibration()}"
    "async function pollCo2Calibration(){try{let r=await fetch('/co2cal/status');let d=await r.json();"
    "let extra=d.state==='stabilizing'?' Remaining: '+d.remaining_sec+' s.':'';"
    "document.getElementById('cal_status').textContent=d.message+extra+' CO2: '+"
    "(d.co2===null?'--':d.co2)+' ppm; pressure: '+(d.pressure_hpa===null?'--':d.pressure_hpa)+' hPa.';"
    "let busy=d.state==='stabilizing'||d.state==='calibrating';"
    "document.getElementById('cal_start').disabled=busy;"
    "document.getElementById('cal_cancel').disabled=!busy||d.state==='calibrating';}catch(e){}}"
    "setInterval(pollCo2Calibration,1000);pollCo2Calibration();"
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
      lastDisplayUpdateMs = 0;
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
  owmMinute.valid = false;
  lastOnlineAttemptMs = 0;
  webRefreshWeather = true;
  nvsSave();
  webServer.send(200, "text/plain", "ok");
}

// GET /refresh — trigger weather refresh on next loop()
void handleRefresh() {
  webRefreshWeather = true;
  webServer.send(200, "text/plain", "ok");
}

void handleCo2CalibrationStart() {
  if (!webServer.hasArg("confirmed") || webServer.arg("confirmed") != "true") {
    webServer.send(400, "text/plain", "Outdoor placement confirmation is required.");
    return;
  }
  if (co2Calibration.state == CO2_CAL_STABILIZING ||
      co2Calibration.state == CO2_CAL_EXECUTING) {
    webServer.send(409, "text/plain", "A calibration workflow is already running.");
    return;
  }
  if (!wifiConnected || WiFi.status() != WL_CONNECTED) {
    webServer.send(503, "text/plain", "Wi-Fi is required to download local pressure.");
    return;
  }
  if (!indoor.valid || indoor.co2 == 0xFFFF) {
    webServer.send(503, "text/plain", "The SEN66 does not have a valid CO2 reading.");
    return;
  }

  // Deliberately fetch now: calibration may not use cached pressure.
  if (!fetchWeatherData() || weatherData.pressureHpa < 700.0f ||
      weatherData.pressureHpa > 1200.0f) {
    webServer.send(502, "text/plain",
                   "Could not retrieve a valid 700-1200 hPa local pressure from WeatherAPI.");
    return;
  }

  uint16_t pressureHpa = (uint16_t)lroundf(weatherData.pressureHpa);
  int16_t pressureError = sen66.setAmbientPressure(pressureHpa);
  if (pressureError != NO_ERROR) {
    char message[96];
    snprintf(message, sizeof(message),
             "SEN66 rejected pressure compensation (error %d).", pressureError);
    webServer.send(500, "text/plain", message);
    return;
  }

  co2Calibration.state = CO2_CAL_STABILIZING;
  co2Calibration.pressureHpa = pressureHpa;
  co2Calibration.correctionRaw = 0;
  co2Calibration.qualifyingSince = co2InCalibrationBand() ? millis() : 0;
  snprintf(co2Calibration.message, sizeof(co2Calibration.message),
           "Pressure %.0f hPa applied. Waiting for five continuous minutes in the %u-%u ppm band.",
           co2Calibration.pressureHpa, CO2_CAL_MIN_PPM, CO2_CAL_MAX_PPM);
  Serial.printf("[SEN66 FRC] Outdoor workflow started at %.0f hPa.\n",
                co2Calibration.pressureHpa);
  webServer.send(202, "text/plain", co2Calibration.message);
}

void handleCo2CalibrationCancel() {
  if (co2Calibration.state == CO2_CAL_EXECUTING) {
    webServer.send(409, "text/plain", "Calibration is being written and cannot be cancelled.");
    return;
  }
  co2Calibration.state = CO2_CAL_IDLE;
  co2Calibration.qualifyingSince = 0;
  strncpy(co2Calibration.message, "Calibration cancelled.",
          sizeof(co2Calibration.message) - 1);
  co2Calibration.message[sizeof(co2Calibration.message) - 1] = '\0';
  webServer.send(200, "text/plain", co2Calibration.message);
}

void handleCo2CalibrationStatus() {
  unsigned long remaining = CO2_CAL_STABILIZE_MS / 1000;
  if (co2Calibration.state == CO2_CAL_STABILIZING &&
      co2Calibration.qualifyingSince != 0) {
    unsigned long elapsed = millis() - co2Calibration.qualifyingSince;
    remaining = elapsed >= CO2_CAL_STABILIZE_MS ?
                0 : (CO2_CAL_STABILIZE_MS - elapsed + 999) / 1000;
  }
  String co2Value = indoor.valid && indoor.co2 != 0xFFFF ?
                    String(indoor.co2) : String("null");
  String pressureValue = isnan(co2Calibration.pressureHpa) ?
                         String("null") : String(co2Calibration.pressureHpa, 0);
  String json = "{\"state\":\"" + String(co2CalibrationStateName()) +
                "\",\"remaining_sec\":" + String(remaining) +
                ",\"co2\":" + co2Value +
                ",\"pressure_hpa\":" + pressureValue +
                ",\"in_reference_band\":" +
                String(co2InCalibrationBand() ? "true" : "false") +
                ",\"message\":\"" + String(co2Calibration.message) + "\"}";
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", json);
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

// GET /wakenow — temporary 60-second interactive wake from the web UI
void handleWakeNow() {
  activateDisplayInteractiveWindow();
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

void handleProvisioningPage() {
  String html = "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>SEN66 Setup</title>" + String(kCSS) + "</head><body><div class='w'>"
    "<div class='card'><h1>SEN66 Network Setup</h1>"
    "<div class='k'>This temporary setup network closes after ten minutes. "
    "Enter Wi-Fi credentials and decimal coordinates. Browser location requires "
    "permission and may be blocked on this local HTTP page; manual coordinates always work.</div>"
    "<form method='post' action='/savewifi'>"
    "<div class='row' style='margin-top:14px'><label class='kv'>"
    "<div class='k'>Wi-Fi SSID</div><input type='text' name='ssid' maxlength='32' required></label></div>"
    "<div class='row'><label class='kv'><div class='k'>Wi-Fi password</div>"
    "<input type='password' name='pass' maxlength='64'></label></div>"
    "<div class='row'><label class='kv'><div class='k'>Latitude</div>"
    "<input type='text' id='lat' name='lat' maxlength='16' required></label>"
    "<label class='kv'><div class='k'>Longitude</div>"
    "<input type='text' id='lon' name='lon' maxlength='16' required></label></div>"
    "<div class='row'><button type='button' class='btn sec' onclick='locate()'>Use Phone Location</button>"
    "<button type='submit' class='btn'>Save and Restart</button></div>"
    "<div id='status' class='k'></div></form></div>"
    "<script>function locate(){let s=document.getElementById('status');"
    "if(!navigator.geolocation){s.textContent='Geolocation is unavailable; enter coordinates manually.';return;}"
    "s.textContent='Requesting browser location permission...';"
    "navigator.geolocation.getCurrentPosition(p=>{lat.value=p.coords.latitude.toFixed(6);"
    "lon.value=p.coords.longitude.toFixed(6);s.textContent='Location filled in.'},"
    "e=>s.textContent='Location unavailable: '+e.message+'. Enter coordinates manually.',"
    "{enableHighAccuracy:true,timeout:15000})}</script></div></body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
}

void handleSaveWifi() {
  String newSsid = webServer.arg("ssid");
  String newPass = webServer.arg("pass");
  String latText = webServer.arg("lat");
  String lonText = webServer.arg("lon");
  newSsid.trim(); latText.trim(); lonText.trim();
  char* latEnd = nullptr;
  char* lonEnd = nullptr;
  float lat = strtof(latText.c_str(), &latEnd);
  float lon = strtof(lonText.c_str(), &lonEnd);
  if (newSsid.length() == 0 || newSsid.length() > 32 ||
      newPass.length() > 64 || !latEnd || *latEnd != '\0' ||
      !lonEnd || *lonEnd != '\0' || lat < -90.0f || lat > 90.0f ||
      lon < -180.0f || lon > 180.0f) {
    webServer.send(400, "text/plain", "Invalid SSID, password, or coordinates.");
    return;
  }
  String location = String(lat, 6) + "," + String(lon, 6);
  prefs.begin("dash", false);
  prefs.putString("wifi_ssid", newSsid);
  prefs.putString("wifi_pass", newPass);
  prefs.putString("weather_loc", location);
  prefs.end();
  webServer.send(200, "text/html; charset=utf-8",
                 "<html><body><h2>Saved. The station is restarting.</h2></body></html>");
  delay(500);
  ESP.restart();
}

void startProvisioningPortal() {
  provisioningActive = true;
  provisioningUntilMs = millis() + 600000UL;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("SEN66-Setup", "sen66-setup");
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.print("[WIFI] Setup portal: http://");
  Serial.println(WiFi.softAPIP());
  startWebServer();
}

void startWebServer() {
  if (webServerStarted) {
    webServer.begin();
    return;
  }
  webServer.on("/", []() {
    if (provisioningActive) handleProvisioningPage();
    else handleRoot();
  });
  webServer.on("/setup",        handleProvisioningPage);
  webServer.on("/savewifi", HTTP_POST, handleSaveWifi);
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
  webServer.on("/co2cal/start", HTTP_POST, handleCo2CalibrationStart);
  webServer.on("/co2cal/cancel", HTTP_POST, handleCo2CalibrationCancel);
  webServer.on("/co2cal/status", HTTP_GET, handleCo2CalibrationStatus);
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
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", provisioningActive ? "/setup" : "/");
    webServer.send(303,"text/plain","");
  });
  webServer.begin();
  webServerStarted = true;
  Serial.print("[WEB] Serving at http://");
  Serial.println(provisioningActive ? WiFi.softAPIP() : WiFi.localIP());
}

void requestWifiWindow(unsigned long durationMs = WIFI_MANUAL_WINDOW_MS) {
  wifiWindowUntilMs = max(wifiWindowUntilMs, millis() + durationMs);
  if (!wifiConnected) connectWiFi();
  if (wifiConnected) {
    startWebServer();
    Serial.printf("[WIFI] On-demand window open for %lu seconds at http://%s\n",
                  durationMs / 1000UL, WiFi.localIP().toString().c_str());
  }
}

void stopStationWifi() {
  if (!wifiConnected || provisioningActive) return;
  webServer.stop();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  wifiConnected = false;
  wifiRSSI = 0;
  Serial.println("[WIFI] Radio disabled for low-power interval.");
}

void serviceConnectivity() {
  unsigned long now = millis();
  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (provisioningActive) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    if ((long)(now - provisioningUntilMs) >= 0) {
      dnsServer.stop();
      WiFi.softAPdisconnect(true);
      provisioningActive = false;
      if (!wifiConnected) WiFi.mode(WIFI_OFF);
      Serial.println("[WIFI] Ten-minute setup window closed.");
    }
    return;
  }

  unsigned long interval =
    ((long)(rainBoostUntilMs - now) > 0) ? WIFI_RAIN_MS : WIFI_NORMAL_MS;
  bool onlineUpdateDue =
    lastOnlineAttemptMs == 0 || now - lastOnlineAttemptMs >= interval;

  if (onlineUpdateDue) {
    lastOnlineAttemptMs = now;
    if (!wifiConnected) connectWiFi();
    if (wifiConnected) {
      startWebServer();
      fetchAllOnlineData();
      if (ntpLastSync == 0 || now - ntpLastSync > 86400000UL) syncRTCWithNTP();
      if (lowPowerMode) wifiWindowUntilMs = max(wifiWindowUntilMs, millis() + 30000UL);
    }
  }

  if (wifiConnected) {
    wifiRSSI = WiFi.RSSI();
    webServer.handleClient();
  }
  if (lowPowerMode && wifiConnected && (long)(now - wifiWindowUntilMs) >= 0)
    stopStationWifi();
}

void handleButtons() {
  unsigned long now = millis();
  bool btnLNow = (digitalRead(BTN_LEFT)   == LOW);
  bool btnMNow = (digitalRead(BTN_MIDDLE) == LOW);
  bool newButtonPress =
    (btnLNow && !btnLeftPrev) || (btnMNow && !btnMiddlePrev);
  if (newButtonPress) activateDisplayInteractiveWindow();

  // While display is sleeping, any button press wakes it for 60 seconds.
  // No page changes or other actions are triggered during sleep.
  if (isDisplaySleeping()) {
    btnLeftPrev   = btnLNow;
    btnMiddlePrev = btnMNow;
    return;
  }

  // GPIO0/BOOT: short press = previous page; hold >=1 s = five-minute Wi-Fi window.
  // During an alarm/timer, the established snooze/dismiss actions take priority.
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
      requestWifiWindow();
    }
  }
  if (!btnLNow && btnLeftPrev) {
    if (!btnLeftHeld && (now - lastBtnLeftDown > 30) && (now - lastBtnLeftDown < LONG_PRESS_MS)) {
      if (alarmFiring)       { snoozeAlarm(); }
      else if (timerFiring)  { dismissTimer(); }
      else {
        currentPage = nextEnabledPage(currentPage, -1);
        beepPageChange();
        lastDisplayUpdateMs = 0;
      }
    }
    btnLeftHeld = false;
  }
  btnLeftPrev = btnLNow;

  // GPIO18/KEY: short press = next page.
  if (btnMNow && !btnMiddlePrev) { lastBtnMiddleDown = now; btnMiddleHeld = false; }
  if (btnMNow && btnMiddlePrev && !btnMiddleHeld && (now - lastBtnMiddleDown >= LONG_PRESS_MS)) {
    btnMiddleHeld = true;
    if (alarmFiring) dismissAlarm();
    else if (timerFiring) dismissTimer();
  }
  if (!btnMNow && btnMiddlePrev) {
    if (!btnMiddleHeld && (now - lastBtnMiddleDown > 30) && (now - lastBtnMiddleDown < LONG_PRESS_MS)) {
      if (alarmFiring) snoozeAlarm();
      else if (timerFiring) dismissTimer();
      else {
        currentPage = nextEnabledPage(currentPage, 1);
        beepPageChange();
        lastDisplayUpdateMs = 0;
      }
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
  loadSensorComparisonState();
  updateBatteryState(readBatteryVoltage());
  serviceUsbHostState(true);
  lastBatteryReadMs = millis();
  gpio_wakeup_enable((gpio_num_t)BTN_LEFT, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)BTN_MIDDLE, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  Wire.begin(13, 14);
  scanI2cBusAtStartup();
  mountStorage();
  initAudio();
  RlcdPort.RLCD_Init();
  displayHighPower = true;
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
    drawBoot("Loading outdoor weather...", 0); fetchAllOnlineData();
    lastOnlineAttemptMs = millis();
    bootLine--; drawBoot(weatherData.valid ? "Outdoor weather loaded" : "Weather unavailable", weatherData.valid ? 1 : -1);
    drawBoot("Syncing location time...", 0); syncRTCWithNTP();
    bootLine--; drawBoot("Location clock synced", 1);
    if (lowPowerMode) wifiWindowUntilMs = millis() + 30000UL;
  } else {
    drawBoot("Starting phone setup...", 0);
    startProvisioningPortal();
    bootLine--; drawBoot("Join SEN66-Setup", 1);
  }

  serviceShtc3(true);
  if (senReady) {
    delay(1100);
    if (readSen66()) sensorReadCount++; else sensorFailCount++;
  }
  lastSensorReadMs = millis();

  loadHistory();
  if (history.sampleCount == 0 && (indoor.valid || sensorComparison.shtc3Valid))
    appendHistoryPoint(cToF(historyTemperatureC()), historyRelativeHumidity());
  serviceDisplayPowerMode();
  beepBootOk();
  Serial.println("Ready!");
  delay(500);
}
// ===== LOOP =====
void loop() {
  if (wifiConnected) webServer.handleClient();  // non-blocking — must be called every loop

  serviceUsbHostState();
  handleButtons();
  serviceDisplayPowerMode();
  // Read all RTC values in one pass to minimise I2C transactions
  hour24    = rtc.getHour();
  minuteVal = rtc.getMinute();
  // secondVal needed for dashboard (page 0) and analogue clock (page 1) second hands
  secondVal = rtc.getSecond();

  unsigned long now = millis();
  serviceConnectivity();
  serviceSen66Power();
  serviceShtc3();
  if (sen66MeasurementRunning && now - sen66StartedMs >= 1100UL &&
      now - lastSensorReadMs >= 1000UL) {
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
    Serial.printf(" VBAT=%.3fV", batteryVoltage);
    Serial.println();
    serviceCo2Calibration();
  }
  if (now - lastBatteryReadMs >= 10000UL) {
    lastBatteryReadMs=now;
    updateBatteryState(readBatteryVoltage());
    logBatteryTrend();
  }
  // Low battery warning beep — fires when voltage drops below 3.50V, at most once per minute
  unsigned long nowMs = millis();
  if (audioReady && codecReady && batteryVoltage > 0.5f && batteryVoltage < 3.50f) {
    if (nowMs - lastBatteryBeepMs >= 60000UL) {
      lastBatteryBeepMs = nowMs;
      beepLowBattery();
    }
  }

  if (now - history.lastLogTime >= HISTORY_UPDATE_MS)
    appendHistoryPoint(cToF(historyTemperatureC()), historyRelativeHumidity());

  // Web UI triggered actions (set by handlers, actioned here on main thread)
  if (webRefreshWeather) { webRefreshWeather = false; if (wifiConnected) fetchAllOnlineData(); }
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
    lastDisplayUpdateMs = 0;
  }

  // Display sleep — skip all canvas draws when sleeping.
  // On first entry to sleep: show a brief "Going to sleep..." banner then stop.
  // Button press (handled above in handleButtons) sets a 60-second wake window.
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
    RlcdPort.RLCD_Sleep();
    displayHighPower = true;
  }
  if (wasSleeping && !nowSleeping) {
    RlcdPort.RLCD_Wake();
    displayHighPower = true;
    serviceDisplayPowerMode();
    lastDisplayUpdateMs = 0;
  }
  wasSleeping = nowSleeping;

  if (nowSleeping) {
    delay(100);   // idle — no SPI writes while sleeping
  } else {
    unsigned long displayInterval =
      displayHighPower ? DISPLAY_INTERACTIVE_UPDATE_MS : DISPLAY_UPDATE_MS;
    if (lastDisplayUpdateMs == 0 ||
        now - lastDisplayUpdateMs >= displayInterval) {
      draw();
      lastDisplayUpdateMs = millis();
    }
  }

  // Timer and either active-low button wake light sleep. Wi-Fi, audible alerts,
  // and the guarded outdoor calibration keep the CPU awake.
  bool canLightSleep = lowPowerMode && !wifiConnected && !provisioningActive &&
    !alarmFiring && !timerFiring &&
    co2Calibration.state != CO2_CAL_STABILIZING &&
    co2Calibration.state != CO2_CAL_EXECUTING &&
    !displayInteractive() &&
    digitalRead(BTN_LEFT) == HIGH && digitalRead(BTN_MIDDLE) == HIGH;
  if (canLightSleep) {
    esp_sleep_enable_timer_wakeup(200000ULL);
    esp_light_sleep_start();
  } else if (!nowSleeping) {
    delay(20);
  }
}
