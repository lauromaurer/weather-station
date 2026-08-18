/*
 * Weather Station — Full Sensor Suite + SD Logging + Low Power + SMS  v4.2
 * Board:  LilyGo TTGO T-SIM7000G (ESP32-WROVER-E)
 *
 * Sensors:
 *   BME280    → INSIDE the enclosure box  (temp, humidity, pressure)
 *   SHT20     → OUTSIDE (actual weather)  (temp, humidity)
 *   BH1750    → Light                     (lux)
 *   XS-WSDS01 → Wind speed + direction    (RS485 Modbus-RTU)
 *
 * Features:
 *   SD card CSV logging every measurement
 *   Battery percentage via ADC (GPIO 35)
 *   Daily SMS status via SIM7000G modem
 *
 * NOTE ON SMS:
 *   The SIM7000G sends SMS over 2G/GSM (AT+CNMP=2, auto mode).
 *   This works in Italy (2G active until ~2027+).
 *   It does NOT work in Switzerland (2G shut down 2020/2021).
 *
 * ── STATION_MODE ─────────────────────────────────────────────────────────────
 *   0 = TEST       reads every 3 s, no deep sleep, SMS every 10 readings
 *   1 = ECO        reads every 3 min with deep sleep between, SMS daily
 *   2 = PRODUCTION deep sleep SLEEP_MINUTES between reads, SMS daily
 *
 * ── Wiring ───────────────────────────────────────────────────────────────────
 *   I2C (BME280, SHT20, BH1750 shared):  SDA → 21 / SCL → 22
 *   TPS61023 boost EN:  GPIO 33  (+ 10 kΩ pull-down to GND)
 *   MAX485 EN:          GPIO 32
 *   MAX485 RXD:         GPIO 18  (ESP32 TX)
 *   MAX485 TXD:         GPIO 19  (ESP32 RX)
 *   MAX485 A/B:         Wind sensor A/B
 *   Wind sensor VCC:    TPS61023 5V out
 *   SD card:            Built-in slot (remove SD card before uploading code)
 *   SIM card:           Built-in nano-SIM slot
 *   Antenna:            LTE/SIM port (not GPS port)
 *   Battery ADC:        GPIO 35 (built-in voltage divider on board)
 *
 * ── Libraries ────────────────────────────────────────────────────────────────
 *   Adafruit BME280 Library  (+ Adafruit Unified Sensor)
 *   SHT2x                    by Rob Tillaart
 *   BH1750                   by Christopher Laws
 *   TinyGSM                  by vshymanskyy
 */

#define TINY_GSM_MODEM_SIM7000
#define TINY_GSM_RX_BUFFER 1024

#include <TinyGsmClient.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include "driver/gpio.h"
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <SHT2x.h>
#include <BH1750.h>

// ── Configuration ─────────────────────────────────────────────────────────────
// STATION_MODE: 0 = test | 1 = eco (3 min sleep) | 2 = production
#define STATION_MODE   2
#define SLEEP_MINUTES  5          // used in PRODUCTION mode only (fallback)
#define ECO_MINUTES    3          // fixed sleep for ECO mode (fallback)

// ── Location (for sunrise/sunset calculation) ─────────────────────────────────
#define STATION_LAT         42.76f
#define STATION_LON         10.29f

// ── Adaptive daylight schedule ────────────────────────────────────────────────
#define DAILY_MEASUREMENTS   4
#define SUNRISE_BUFFER_MIN  30
#define SUNSET_BUFFER_MIN   30

// ── Session start date & time ─────────────────────────────────────────────────
#define SESSION_START_YEAR  2026
#define SESSION_START_MONTH    7
#define SESSION_START_DAY      4
#define SESSION_START_HOUR     7
#define SESSION_START_MIN      0

// Your phone number in international format (e.g. +39...)
#define SMS_TARGET    "+41XXXXXXXXX"

// SIM PIN — set to "" if your SIM has no PIN
#define SIM_PIN  ""

// How often to send SMS (wakeups per day, or 10 in test mode)
#if   (STATION_MODE == 0)
  #define SMS_EVERY_N_WAKEUPS  10
#elif (STATION_MODE == 1)
  #define SMS_EVERY_N_WAKEUPS  (24 * 60 / ECO_MINUTES)
#else
  #define SMS_EVERY_N_WAKEUPS  (24 * 60 / SLEEP_MINUTES)
#endif

// ── Pins ──────────────────────────────────────────────────────────────────────
#define SDA_PIN       21
#define SCL_PIN       22
#define SD_CS         13
#define SD_MOSI       15
#define SD_MISO        2
#define SD_CLK        14
#define RS485_TX      18
#define RS485_RX      19
#define RS485_EN      32
#define BOOST_EN      33
#define BAT_PIN       35
#define MODEM_PWRKEY   4
#define MODEM_DTR     25
#define MODEM_TX      27
#define MODEM_RX      26
#define LED_BLUE      12   // active LOW: HIGH = off, LOW = on

// ── RTC memory ────────────────────────────────────────────────────────────────
RTC_DATA_ATTR int           wakeCount     = 0;
RTC_DATA_ATTR unsigned long accumulatedMs = 0;
RTC_DATA_ATTR bool          firstBoot     = true;
RTC_DATA_ATTR uint32_t      lastSleepMs   = 0;

unsigned long sessionMs() {
#if (STATION_MODE == 0)
  return millis();
#else
  return accumulatedMs;
#endif
}

// ── Objects ───────────────────────────────────────────────────────────────────
Adafruit_BME280 bme;
SHT2x           sht;
BH1750          lightMeter;
HardwareSerial  rs485(2);
HardwareSerial  modemSerial(1);
TinyGsm         modem(modemSerial);

// ── Wind data struct ──────────────────────────────────────────────────────────
struct WindData {
  float    speedMs;
  uint16_t beaufort;
  float    speedKmh;
  float    dirDeg;
  uint8_t  dirCode;
  bool     valid;
};

// ── Battery ───────────────────────────────────────────────────────────────────
float getBatteryVoltage() {
  int raw = analogRead(BAT_PIN);
  return (raw / 4095.0f) * 3.3f * 2.0f;
}

int getBatteryPercent() {
  float v = getBatteryVoltage();
  if (v >= 4.2f) return 100;
  if (v <= 3.3f) return 0;
  return (int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f);
}

// ── Modbus helpers ────────────────────────────────────────────────────────────
uint16_t modbusCRC(uint8_t *buf, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (uint8_t j = 0; j < 8; j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

const char* bearingName(uint8_t code) {
  static const char* names[16] = {
    "N","NNE","NE","ENE","E","ESE","SE","SSE",
    "S","SSW","SW","WSW","W","WNW","NW","NNW"
  };
  return (code < 16) ? names[code] : "?";
}

WindData readWind() {
  WindData w = {};
  w.valid = false;

  uint8_t req[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00};
  uint16_t crc = modbusCRC(req, 6);
  req[6] = crc & 0xFF;
  req[7] = crc >> 8;

  while (rs485.available()) rs485.read();
  digitalWrite(RS485_EN, HIGH);
  delayMicroseconds(200);
  rs485.write(req, 8);
  rs485.flush();
  digitalWrite(RS485_EN, LOW);

  uint32_t t0 = millis();
  while (rs485.available() < 15 && millis() - t0 < 500);
  if (rs485.available() < 15) return w;

  uint8_t resp[15];
  rs485.readBytes(resp, 15);
  uint16_t rxCRC = (uint16_t)resp[13] | ((uint16_t)resp[14] << 8);
  if (modbusCRC(resp, 13) != rxCRC) return w;

  w.speedMs  = ((resp[3]  << 8) | resp[4])  / 10.0f;
  w.beaufort =  (resp[5]  << 8) | resp[6];
  w.speedKmh = ((resp[7]  << 8) | resp[8])  / 100.0f;
  w.dirDeg   = ((resp[9]  << 8) | resp[10]) / 10.0f;
  w.dirCode  =  resp[12];
  w.valid    = true;
  return w;
}

// ── SD logging ────────────────────────────────────────────────────────────────
#define LOG_FILE "/weather.csv"
bool sdReady = false;

void sdInit() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  delay(100);
  SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  delay(100);

  if (!SD.begin(SD_CS, SPI, 4000000)) {
    SPI.end(); delay(100);
    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI, 1000000)) {
      Serial.println("[ERR] SD not found — no data logged this session");
      return;
    }
  }
  sdReady = true;
  Serial.printf("[OK]  SD card  %.0f MB\n", SD.cardSize() / 1048576.0);

  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (f && f.size() == 0) {
    f.println(
      "timestamp_ms,"
      "inside_temp_C,inside_hum_pct,pressure_hPa,"
      "outside_temp_C,outside_hum_pct,"
      "light_lux,"
      "wind_speed_ms,wind_speed_kmh,wind_beaufort,wind_dir_deg,wind_dir_bearing,"
      "battery_pct"
    );
  }
  if (f) f.close();
}

void sdLog(float insideT, float insideH, float pressure,
           float outsideT, float outsideH,
           float lux, WindData &w, int batPct) {
  if (!sdReady) return;
  File f = SD.open(LOG_FILE, FILE_APPEND);
  if (!f) { Serial.println("[WARN] SD write failed"); return; }

  f.print(sessionMs()); f.print(',');
  f.print(insideT,  1); f.print(',');
  f.print(insideH,  1); f.print(',');
  f.print(pressure, 1); f.print(',');
  f.print(outsideT, 1); f.print(',');
  f.print(outsideH, 1); f.print(',');
  f.print(lux,      0); f.print(',');

  if (w.valid) {
    f.print(w.speedMs,  1); f.print(',');
    f.print(w.speedKmh, 1); f.print(',');
    f.print(w.beaufort);    f.print(',');
    f.print(w.dirDeg,   1); f.print(',');
    f.print(bearingName(w.dirCode));
  } else {
    f.print(",,,,");
  }
  f.print(',');
  f.println(batPct);
  f.close();
}

// ── Modem / SMS ───────────────────────────────────────────────────────────────
void modemPowerOn() {
  Serial.println("[GSM] Powering on modem...");
  pinMode(MODEM_PWRKEY, OUTPUT);
  // T-SIM7000G: inverting NPN transistor on PWRKEY
  //   GPIO HIGH → PWRKEY pulled LOW (active) for 1 s → modem powers on
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(5000);   // wait for boot
}

void modemPowerOff() {
  modem.poweroff();
  delay(500);
  Serial.println("[GSM] Modem off");
}

bool sendSMS(const char* number, const String &msg) {
  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  modemPowerOn();

  Serial.println("[GSM] Initialising...");
  if (!modem.init()) {
    delay(3000);
    if (!modem.init()) {
      Serial.println("[GSM] Init failed");
      return false;
    }
  }
  Serial.println("[GSM] Modem: " + modem.getModemInfo());

  // Unlock SIM PIN if set
  if (strlen(SIM_PIN) > 0) {
    modem.simUnlock(SIM_PIN);
    delay(2000);
  }

  // Auto network mode — allows 2G (needed for SMS on SIM7000G)
  modem.sendAT("+CNMP=2");    // auto: 2G/3G/LTE
  modem.waitResponse(5000);
  Serial.println("[GSM] Network mode: auto (2G/3G/LTE)");

  Serial.println("[GSM] Waiting for network (up to 60 s)...");
  if (!modem.waitForNetwork(60000)) {
    Serial.printf("[GSM] No network. Signal: %d/31\n", modem.getSignalQuality());
    modemPowerOff();
    return false;
  }
  Serial.printf("[GSM] Registered. Signal: %d/31  Operator: %s\n",
                modem.getSignalQuality(), modem.getOperator().c_str());

  bool ok = modem.sendSMS(number, msg);
  Serial.println(ok ? "[GSM] SMS sent!" : "[GSM] SMS failed");

  modemPowerOff();
  return ok;
}

String buildSMS(float outsideT, float outsideH, float pressure,
                float lux, WindData &w, int batPct) {
  String msg = "=== Weather Station ===\n";
  msg += "Battery:  " + String(batPct) + "%\n";
  msg += "Temp:     " + String(outsideT, 1) + " C\n";
  msg += "Humidity: " + String(outsideH, 1) + " %\n";
  msg += "Pressure: " + String(pressure, 1) + " hPa\n";
  if (w.valid) {
    msg += "Wind:     " + String(w.speedMs, 1) + " m/s " +
           bearingName(w.dirCode) + "\n";
  } else {
    msg += "Wind:     no data\n";
  }
  msg += "Light:    " + String((int)lux) + " lux";
  return msg;
}

// ── Astronomical helpers ──────────────────────────────────────────────────────

// True if year/month/day falls inside European Summer Time (DST).
// DST starts: last Sunday of March at 02:00 local.
// DST ends:   last Sunday of October at 03:00 local.
bool isEuropeanDST(int year, int month, int day) {
  if (month < 3 || month > 10) return false;
  if (month > 3 && month < 10) return true;
  static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int lastDay = dim[month - 1];
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
    lastDay = 29;
  int y = year; int m = month;
  static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y--;
  int dow = (y + y/4 - y/100 + y/400 + t[m-1] + lastDay) % 7; // 0 = Sunday
  int lastSun = lastDay - dow;
  if (month == 3)  return day >= lastSun;
  if (month == 10) return day <  lastSun;
  return false;
}

// Returns day-of-year (1 = Jan 1).
int dayOfYear(int year, int month, int day) {
  static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int doy = day;
  for (int m = 0; m < month - 1; m++) doy += dim[m];
  if (month > 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
    doy++;
  return doy;
}

// Derives the current calendar date from the session start + accumulatedMs.
void currentDate(int &year, int &month, int &day) {
  static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint32_t daysElapsed = accumulatedMs / 86400000UL;
  year  = SESSION_START_YEAR;
  month = SESSION_START_MONTH;
  day   = SESSION_START_DAY + (int)daysElapsed;
  while (true) {
    int d = dim[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) d = 29;
    if (day <= d) break;
    day -= d;
    if (++month > 12) { month = 1; year++; }
  }
}

// Current minute of the day (0–1439) derived from session start + elapsed time.
int currentMinuteOfDay() {
  uint32_t startMin   = (uint32_t)SESSION_START_HOUR * 60 + SESSION_START_MIN;
  uint32_t elapsedMin = accumulatedMs / 60000UL;
  return (int)((startMin + elapsedMin) % 1440);
}

// Calculates sunrise and sunset as minutes from midnight in local time.
// Uses NOAA's simplified algorithm — accurate to within ~5 min for mid-latitudes.
void sunriseSunset(int doy, float lat, float lon, float tzHours,
                   int &sunriseMin, int &sunsetMin) {
  const float DEG = PI / 180.0f;
  float gamma = 2.0f * PI * (doy - 1) / 365.0f;

  float eq = 229.18f * (0.000075f
    + 0.001868f * cosf(gamma)    - 0.032077f * sinf(gamma)
    - 0.014615f * cosf(2*gamma)  - 0.040890f * sinf(2*gamma));

  float decl = 0.006918f
    - 0.399912f * cosf(gamma)   + 0.070257f * sinf(gamma)
    - 0.006758f * cosf(2*gamma) + 0.000907f * sinf(2*gamma)
    - 0.002697f * cosf(3*gamma) + 0.001480f * sinf(3*gamma);

  float cosHA = cosf(90.833f * DEG) / (cosf(lat * DEG) * cosf(decl))
              - tanf(lat * DEG) * tanf(decl);
  cosHA = constrain(cosHA, -1.0f, 1.0f);
  float ha = acosf(cosHA);

  sunriseMin = (int)roundf(720.0f - 4.0f * (lon + ha / DEG) - eq + tzHours * 60.0f);
  sunsetMin  = (int)roundf(720.0f - 4.0f * (lon - ha / DEG) - eq + tzHours * 60.0f);
}

// Builds today's measurement schedule: fills `slots[]` with minutes-from-midnight
// evenly distributed between (sunrise + buffer) and (sunset - buffer).
int buildDaySchedule(int *slots, int maxSlots) {
  int year, month, day;
  currentDate(year, month, day);
  float tz = isEuropeanDST(year, month, day) ? 2.0f : 1.0f;
  int doy  = dayOfYear(year, month, day);

  int sunriseMin, sunsetMin;
  sunriseSunset(doy, STATION_LAT, STATION_LON, tz, sunriseMin, sunsetMin);

  Serial.printf("[SCHED] Date %04d-%02d-%02d  DOY %d  TZ UTC+%.0f\n",
                year, month, day, doy, tz);
  Serial.printf("[SCHED] Sunrise %02d:%02d  Sunset %02d:%02d\n",
                sunriseMin / 60, sunriseMin % 60, sunsetMin / 60, sunsetMin % 60);

  int playStart = sunriseMin + SUNRISE_BUFFER_MIN;
  int playEnd   = sunsetMin  - SUNSET_BUFFER_MIN;

  if (playEnd <= playStart) {
    Serial.println("[SCHED] Daylight window too short — skipping day");
    return 0;
  }

  int n = min(maxSlots, DAILY_MEASUREMENTS);
  for (int i = 0; i < n; i++) {
    slots[i] = (n == 1) ? (playStart + playEnd) / 2
                        : playStart + i * (playEnd - playStart) / (n - 1);
    Serial.printf("[SCHED]   Slot %d → %02d:%02d\n",
                  i + 1, slots[i] / 60, slots[i] % 60);
  }
  return n;
}

// Returns minutes to sleep until the next scheduled measurement.
uint32_t minutesUntilNextWakeup() {
  int slots[8];
  int count = buildDaySchedule(slots, 8);
  int nowMin = currentMinuteOfDay();

  for (int i = 0; i < count; i++) {
    if (slots[i] > nowMin + 2) {
      uint32_t sleepMin = slots[i] - nowMin;
      Serial.printf("[SCHED] Next slot in %u min (at %02d:%02d)\n",
                    sleepMin, slots[i] / 60, slots[i] % 60);
      return sleepMin;
    }
  }

  // All today's slots done — compute tomorrow's first slot
  int year, month, day;
  currentDate(year, month, day);
  day++;
  static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int d = dim[month - 1];
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) d = 29;
  if (day > d) { day = 1; if (++month > 12) { month = 1; year++; } }

  float tz = isEuropeanDST(year, month, day) ? 2.0f : 1.0f;
  int sr, ss;
  sunriseSunset(dayOfYear(year, month, day), STATION_LAT, STATION_LON, tz, sr, ss);
  int tomorrowFirst = sr + SUNRISE_BUFFER_MIN;

  uint32_t sleepMin = (uint32_t)(1440 - nowMin) + tomorrowFirst;
  Serial.printf("[SCHED] All slots done. Sleeping %u min until tomorrow %02d:%02d\n",
                sleepMin, tomorrowFirst / 60, tomorrowFirst % 60);
  return sleepMin;
}

// ── Power helpers ─────────────────────────────────────────────────────────────
void boostOn() {
  gpio_hold_dis((gpio_num_t)BOOST_EN);
  digitalWrite(BOOST_EN, HIGH);
  delay(600);
}

void boostOff() {
  digitalWrite(BOOST_EN, LOW);
  gpio_hold_en((gpio_num_t)BOOST_EN);
}

void goToSleep() {
  uint32_t sleepMin = minutesUntilNextWakeup();
  if (sleepMin == 0 || sleepMin > 1440) {
#if (STATION_MODE == 1)
    sleepMin = ECO_MINUTES;
#else
    sleepMin = SLEEP_MINUTES;
#endif
    Serial.printf("[PWR] Schedule fallback — sleeping %u min\n", sleepMin);
  }

  lastSleepMs = sleepMin * 60UL * 1000UL;
  Serial.printf("[PWR] Deep sleep for %u min (%.1f h)...\n\n", sleepMin, sleepMin / 60.0f);
  Serial.flush();
  SD.end();
  boostOff();
  pinMode(LED_BLUE, OUTPUT); digitalWrite(LED_BLUE, HIGH); // blue off during sleep
  gpio_hold_en((gpio_num_t)LED_BLUE);
  gpio_hold_en((gpio_num_t)BOOST_EN);
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_timer_wakeup((uint64_t)sleepMin * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Weather Station v4.2 ===");
#if   (STATION_MODE == 0)
  Serial.println("    [ TEST MODE — 3 s reads, no sleep, SMS every 10 readings ]\n");
#elif (STATION_MODE == 1)
  Serial.printf( "    [ ECO MODE  — %d min deep sleep, SMS daily ]\n\n", ECO_MINUTES);
#else
  Serial.printf( "    [ PRODUCTION — %d min deep sleep, SMS daily ]\n\n", SLEEP_MINUTES);
#endif
  Serial.printf("    Wake #%d\n\n", wakeCount + 1);

  // ── Accumulate sleep time (ECO / PRODUCTION only) ──────────────────────────
#if (STATION_MODE != 0)
  if (firstBoot) {
    firstBoot     = false;
    accumulatedMs = 0;
    lastSleepMs   = 0;
    Serial.println("[TIME] First boot — session timer reset to 0");
  } else {
    accumulatedMs += lastSleepMs;
    Serial.printf("[TIME] Session elapsed: %lu ms (%.1f h)\n",
                  accumulatedMs, accumulatedMs / 3600000.0f);
  }
#endif

  // Release deep-sleep GPIO holds, turn blue LED off (active LOW → HIGH = off)
  gpio_hold_dis((gpio_num_t)LED_BLUE);
  gpio_hold_dis((gpio_num_t)BOOST_EN);
  gpio_deep_sleep_hold_dis();
  pinMode(LED_BLUE, OUTPUT); digitalWrite(LED_BLUE, HIGH);

  pinMode(MODEM_DTR, OUTPUT); digitalWrite(MODEM_DTR, LOW);
  pinMode(RS485_EN,  OUTPUT); digitalWrite(RS485_EN,  LOW);
  pinMode(BOOST_EN,  OUTPUT);

  boostOn();
  Serial.println("[PWR] Boost ON");

  Wire.begin(SDA_PIN, SCL_PIN);

  if (bme.begin(0x76, &Wire)) {
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::FILTER_OFF,
                    Adafruit_BME280::STANDBY_MS_0_5);
    Serial.println("[OK]  BME280  — inside sensor");
  } else {
    Serial.println("[ERR] BME280 not found");
  }

  sht.begin();
  Serial.println("[OK]  SHT20   — outside sensor");

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire))
    Serial.println("[OK]  BH1750  — light sensor");
  else
    Serial.println("[ERR] BH1750 not found");

  rs485.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  Serial.println("[OK]  RS485   — wind sensor");

  sdInit();
  Serial.println("\n─── Reading sensors ─────────────────────────\n");

#if (STATION_MODE == 0)
  // readings handled in loop()
#else
  takeMeasurement();
  boostOff();
  goToSleep();
#endif
}

// ── Measurement ───────────────────────────────────────────────────────────────
void takeMeasurement() {
  wakeCount++;

  // Double-blink blue LED — indicates active measurement cycle
  for (int i = 0; i < 2; i++) {
    digitalWrite(LED_BLUE, LOW);  delay(80);
    digitalWrite(LED_BLUE, HIGH); delay(120);
  }

  int   batPct   = getBatteryPercent();

  bme.takeForcedMeasurement();
  float insideT  = bme.readTemperature();
  float insideH  = bme.readHumidity();
  float pressure = bme.readPressure() / 100.0f;

  sht.read();
  float outsideT = sht.getTemperature();
  float outsideH = sht.getHumidity();

  float lux = lightMeter.readLightLevel();

  WindData w = readWind();

  Serial.println("─────────────────────────────────────────────────");
  Serial.printf("Battery                 : %d %%  (%.2f V)\n", batPct, getBatteryVoltage());
  Serial.printf("Inside  temp  (BME280)  : %.1f C\n",   insideT);
  Serial.printf("Outside temp  (SHT20)   : %.1f C\n",   outsideT);
  Serial.printf("Inside  hum   (BME280)  : %.1f %%\n",  insideH);
  Serial.printf("Outside hum   (SHT20)   : %.1f %%\n",  outsideH);
  Serial.printf("Pressure      (BME280)  : %.1f hPa\n", pressure);
  Serial.printf("Light         (BH1750)  : %.0f lux\n", lux);
  if (w.valid) {
    Serial.printf("Wind speed  (XS-WSDS01) : %.1f m/s  %.1f km/h  Bft %d\n",
                  w.speedMs, w.speedKmh, w.beaufort);
    Serial.printf("Wind dir    (XS-WSDS01) : %.1f deg  %s\n",
                  w.dirDeg, bearingName(w.dirCode));
  } else {
    Serial.println("Wind sensor             : NO RESPONSE");
  }

  sdLog(insideT, insideH, pressure, outsideT, outsideH, lux, w, batPct);
  if (sdReady) Serial.println("-> logged to /weather.csv");

  if (wakeCount >= SMS_EVERY_N_WAKEUPS) {
    Serial.println("\n[GSM] Sending daily SMS...");
    String msg = buildSMS(outsideT, outsideH, pressure, lux, w, batPct);
    Serial.println(msg);
    sendSMS(SMS_TARGET, msg);
    wakeCount = 0;
  } else {
    Serial.printf("    Next SMS in %d readings\n", SMS_EVERY_N_WAKEUPS - wakeCount);
  }

  Serial.println();
}

// ── Loop (test mode only — eco/production never reach here) ──────────────────
void loop() {
#if (STATION_MODE == 0)
  takeMeasurement();
  delay(3000);
#endif
}
