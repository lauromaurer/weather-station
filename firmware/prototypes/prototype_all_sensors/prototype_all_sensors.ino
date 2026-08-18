/*
 * Full sensor test — BME280, SHT20, BH1750, XS-WSDS01 wind sensor
 * Board: LilyGo TTGO T-SIM7000G (ESP32-WROVER-E)
 *
 * Wiring recap:
 *   I2C (shared):   SDA → 21 / SCL → 22
 *   Boost EN:       GPIO 33  (+ 10k pull-down to GND)
 *   MAX485 EN:      GPIO 32
 *   MAX485 RXD:     GPIO 18  (ESP32 TX)
 *   MAX485 TXD:     GPIO 19  (ESP32 RX)
 *   MAX485 A/B:     Wind sensor A/B
 *   Wind sensor 5V: TPS61023 VOUT
 */

#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <BH1750.h>
#include <SHT2x.h>

// ── Pins ─────────────────────────────────────────────────────────────────────
#define SDA_PIN      21
#define SCL_PIN      22
#define MODEM_PWRKEY  4
#define BOOST_EN     33
#define RS485_EN     32
#define RS485_TX     18
#define RS485_RX     19

// ── Sensor objects ────────────────────────────────────────────────────────────
Adafruit_BME280 bme;
BH1750          lightMeter;
SHT2x           sht;
HardwareSerial  rs485(2);

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

void readWind() {
  // Query 5 registers from address 0x0000 (speed m/s, level, speed km/h, dir deg, dir bearing)
  uint8_t req[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00};
  uint16_t crc = modbusCRC(req, 6);
  req[6] = crc & 0xFF;
  req[7] = crc >> 8;

  while (rs485.available()) rs485.read();   // flush

  digitalWrite(RS485_EN, HIGH);             // transmit mode
  delayMicroseconds(200);
  rs485.write(req, 8);
  rs485.flush();
  digitalWrite(RS485_EN, LOW);              // receive mode

  // Wait up to 500 ms for 15 bytes
  uint32_t t0 = millis();
  while (rs485.available() < 15 && millis() - t0 < 500);

  if (rs485.available() < 15) {
    Serial.println("Wind sensor | NO RESPONSE (check wiring / power)");
    return;
  }

  uint8_t resp[15];
  rs485.readBytes(resp, 15);

  // Validate CRC
  uint16_t rxCRC = (uint16_t)resp[13] | ((uint16_t)resp[14] << 8);
  if (modbusCRC(resp, 13) != rxCRC) {
    Serial.println("Wind sensor | CRC ERROR (check A/B wiring)");
    return;
  }

  float    speedMs  = ((resp[3]  << 8) | resp[4])  / 10.0f;
  uint16_t level    =  (resp[5]  << 8) | resp[6];
  float    speedKmh = ((resp[7]  << 8) | resp[8])  / 100.0f;
  float    dirDeg   = ((resp[9]  << 8) | resp[10]) / 10.0f;
  uint8_t  dirCode  =  resp[12];

  Serial.printf("Wind speed  (XS-WSDS01) : %.1f m/s  (%.1f km/h)  Beaufort %d\n",
                speedMs, speedKmh, level);
  Serial.printf("Wind dir    (XS-WSDS01) : %.1f deg  %s\n",
                dirDeg, bearingName(dirCode));
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== Full Sensor Test ===\n");

  // Keep modem off
  pinMode(MODEM_PWRKEY, OUTPUT); digitalWrite(MODEM_PWRKEY, LOW);

  // RS485 direction — receive by default
  pinMode(RS485_EN, OUTPUT); digitalWrite(RS485_EN, LOW);

  // Enable 5V boost for wind sensor, wait for it to stabilise
  pinMode(BOOST_EN, OUTPUT);
  digitalWrite(BOOST_EN, HIGH);
  Serial.println("[PWR] Boost ON — waiting for wind sensor...");
  delay(600);

  // I2C
  Wire.begin(SDA_PIN, SCL_PIN);

  if (bme.begin(0x76, &Wire)) Serial.println("[OK]  BME280");
  else                         Serial.println("[ERR] BME280 not found");

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire))
    Serial.println("[OK]  BH1750");
  else
    Serial.println("[ERR] BH1750 not found");

  sht.begin();
  Serial.println("[OK]  SHT20");

  // RS485
  rs485.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);
  Serial.println("[OK]  RS485 UART2 (TX=18, RX=19)");

  Serial.println("\n─── Starting readings (every 3 s) ───\n");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  sht.read();

  Serial.println("─────────────────────────────────────────────────");
  Serial.printf("Temperature (BME280)  : %.1f C\n",   bme.readTemperature());
  Serial.printf("Temperature (SHT20)   : %.1f C\n",   sht.getTemperature());
  Serial.printf("Humidity    (BME280)  : %.1f %%\n",  bme.readHumidity());
  Serial.printf("Humidity    (SHT20)   : %.1f %%\n",  sht.getHumidity());
  Serial.printf("Pressure    (BME280)  : %.1f hPa\n", bme.readPressure() / 100.0f);
  Serial.printf("Light       (BH1750)  : %.0f lux\n", lightMeter.readLightLevel());
  readWind();
  Serial.println();

  delay(3000);
}
