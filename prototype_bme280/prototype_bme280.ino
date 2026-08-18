/*
 * Weather Station Prototype — BME280 on LilyGo T-SIM7000G
 *
 * Sensor:  BME280 (temperature, humidity, pressure) via I2C
 * Board:   LilyGo TTGO T-SIM7000G (ESP32-WROVER-E)
 * Wiring:
 *   BME280 VCC  → 3.3V
 *   BME280 GND  → GND
 *   BME280 SDA  → GPIO 21
 *   BME280 SCL  → GPIO 22
 *
 * Required library:
 *   Adafruit BME280 Library  (install via Arduino Library Manager)
 *   Adafruit Unified Sensor  (dependency, install together)
 *
 * Arduino IDE board settings:
 *   Board:            ESP32 Dev Module
 *   Flash Size:       4MB (32Mb)
 *   Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
 *   PSRAM:            Enabled
 *   Upload Speed:     921600
 */

#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>

// T-SIM7000G default I2C pins
#define SDA_PIN 21
#define SCL_PIN 22

// BME280 default I2C address (SDO pin low = 0x76, SDO pin high = 0x77)
#define BME280_ADDRESS 0x76

Adafruit_BME280 bme;

// ── Modem power pin — must stay LOW to avoid accidental modem startup ──
#define MODEM_PWRKEY 4
#define MODEM_DTR    25

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Weather Station Prototype ===");

  // Keep modem off — do not touch GPIO 4 or 25 otherwise
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, LOW);

  // Init I2C on the correct pins
  Wire.begin(SDA_PIN, SCL_PIN);

  // Try default address first, then 0x77
  if (!bme.begin(BME280_ADDRESS, &Wire)) {
    Serial.println("BME280 not found at 0x76, trying 0x77...");
    if (!bme.begin(0x77, &Wire)) {
      Serial.println("ERROR: No BME280 detected. Check wiring!");
      while (1) delay(1000);
    }
  }

  Serial.println("BME280 found! Starting readings...\n");

  // Optional: configure oversampling for better accuracy
  bme.setSampling(
    Adafruit_BME280::MODE_NORMAL,
    Adafruit_BME280::SAMPLING_X4,  // temperature
    Adafruit_BME280::SAMPLING_X4,  // pressure
    Adafruit_BME280::SAMPLING_X4,  // humidity
    Adafruit_BME280::FILTER_X4,
    Adafruit_BME280::STANDBY_MS_500
  );
}

void loop() {
  float temperature = bme.readTemperature();       // °C
  float humidity    = bme.readHumidity();          // %
  float pressure    = bme.readPressure() / 100.0F; // hPa

  Serial.println("--- Reading ---");
  Serial.print("Temperature: "); Serial.print(temperature, 1); Serial.println(" °C");
  Serial.print("Humidity:    "); Serial.print(humidity, 1);    Serial.println(" %");
  Serial.print("Pressure:    "); Serial.print(pressure, 1);    Serial.println(" hPa");
  Serial.println();

  delay(2000); // read every 2 seconds
}
