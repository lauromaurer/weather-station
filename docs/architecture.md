# Architecture

## Hardware

A LilyGo TTGO T-SIM7000G (ESP32-WROVER-E + SIM7000G LTE/GSM modem) is the
controller. It reads three I2C sensors and one RS485 Modbus sensor, logs to
an SD card, and runs off a 6V/3W solar panel through a TPS61023 boost
converter.

| Sensor | Placement | Measures | Interface |
|---|---|---|---|
| BME280 | Inside the enclosure | Temp, humidity, pressure | I2C |
| SHT20 | Outside the enclosure | Temp, humidity | I2C |
| BH1750 | Outside | Light (lux) | I2C |
| XS-WSDS01 | Outside, mast-mounted | Wind speed + direction | RS485 Modbus-RTU, via MAX485 |

Full wiring is documented in the header comment of
[`firmware/weather_station_full/weather_station_full.ino`](../firmware/weather_station_full/weather_station_full.ino).
See [`hardware/bill-of-materials.md`](../hardware/bill-of-materials.md) for
part sourcing.

## Adaptive measurement schedule

Rather than waking on a fixed interval, the station computes sunrise/sunset
for its configured lat/lon (via a standalone solar-position calculation, no
network dependency) and spreads a fixed number of daily measurements
(`DAILY_MEASUREMENTS`) evenly across daylight hours, with configurable
buffers before sunrise and after sunset. This concentrates readings on the
hours that matter for a solar-powered station and skips the dead of night.

[`tools/measurement_schedule.py`](../tools/measurement_schedule.py) is a
standalone port of the same algorithm, used to preview a day's wake times
from a laptop without touching the hardware.

## Power modes (`STATION_MODE`)

| Mode | Behavior |
|---|---|
| `0` TEST | Reads every 3s, no deep sleep, frequent SMS — for bench testing |
| `1` ECO | Deep sleep, wakes every 3 minutes, daily SMS |
| `2` PRODUCTION | Deep sleep, wakes on the adaptive daylight schedule, daily SMS |

Between wakes the ESP32 goes into deep sleep; RTC memory (`RTC_DATA_ATTR`)
carries wake count and accumulated session time across sleep cycles so the
session clock survives resets.

## Wind sensor read (Modbus RTU)

The XS-WSDS01 is polled over RS485 using a hand-rolled Modbus RTU request
(function code 0x03, CRC-16/MODBUS) rather than a library, since the sensor
returns speed, direction, and Beaufort scale in one register block. The
MAX485 driver-enable pin is toggled around the transmit/receive turnaround.

## Data logging

Every reading is appended as a CSV row to `/weather.csv` on the SD card,
timestamped in session-relative milliseconds. Battery percentage is derived
from a resistor-divided ADC read (`GPIO 35`) against a 3.3V–4.2V range
mapped to 0–100%.

## SMS status (SIM7000G, `weather_station_full` only)

The full firmware variant sends a daily status SMS over the SIM7000G modem
in 2G/GSM mode (`AT+CNMP=2`, auto). This is a hard regional constraint, not
a bug: SMS works where 2G is still active (Italy, as of this writing) and
will not work where 2G networks have been shut down (Switzerland, since
2020/2021). `weather_station_no_sms` is the same firmware with the modem
and SMS path removed entirely, for deployments where SMS isn't viable or
isn't wanted — it also skips the TinyGSM dependency.

## Dashboard

[`tools/weather_dashboard.html`](../tools/weather_dashboard.html) is a
static, dependency-free (besides Chart.js from a CDN) page that parses a
logged CSV client-side — drag a file onto it, no server or upload involved.
It also fetches an ERA5 historical baseline (via Open-Meteo) for the
station's coordinates to compare logged conditions against long-term
averages, and includes a configurable "playability" pass (wind/temperature
thresholds) originally built to answer a simple question: is it decent
weather to play tennis today.
