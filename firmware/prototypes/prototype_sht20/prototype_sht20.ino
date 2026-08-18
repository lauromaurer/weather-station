/*
 * BME280 + BH1750 + SHT20 test
 * Board: LilyGo TTGO T-SIM7000G
 * SDA → GPIO 21 / SCL → GPIO 22 (shared by all three)
 */

#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <BH1750.h>
#include <SHT2x.h>

#define SDA_PIN 21
#define SCL_PIN 22
#define MODEM_PWRKEY 4

Adafruit_BME280 bme;
BH1750          lightMeter;
SHT2x           sht;

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

  sht.begin();
  Serial.println("SHT20  OK");

  Serial.println();
}

void loop() {
  sht.read();

  Serial.println("--- Reading ---");
  Serial.printf("Temperature (BME280) : %.1f C\n",  bme.readTemperature());
  Serial.printf("Temperature (SHT20)  : %.1f C\n",  sht.getTemperature());
  Serial.printf("Humidity    (BME280) : %.1f %%\n", bme.readHumidity());
  Serial.printf("Humidity    (SHT20)  : %.1f %%\n", sht.getHumidity());
  Serial.printf("Pressure    (BME280) : %.1f hPa\n",bme.readPressure() / 100.0f);
  Serial.printf("Light       (BH1750) : %.0f lux\n\n", lightMeter.readLightLevel());

  delay(2000);
}
