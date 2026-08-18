/*
 * BME280 + BH1750 test
 * Board: LilyGo TTGO T-SIM7000G
 * SDA → GPIO 21 / SCL → GPIO 22 (shared)
 */

#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <BH1750.h>

#define SDA_PIN 21
#define SCL_PIN 22
#define MODEM_PWRKEY 4

Adafruit_BME280 bme;
BH1750 lightMeter;

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!bme.begin(0x76, &Wire))
    Serial.println("BME280 ERROR");
  else
    Serial.println("BME280 OK");

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire))
    Serial.println("BH1750 ERROR");
  else
    Serial.println("BH1750 OK");

  Serial.println();
}

void loop() {
  Serial.printf("Temp: %.1f C  Hum: %.1f %%  Pres: %.1f hPa  Light: %.0f lux\n",
    bme.readTemperature(),
    bme.readHumidity(),
    bme.readPressure() / 100.0f,
    lightMeter.readLightLevel()
  );
  delay(2000);
}
