# ESP32 OLED display powered by platform.io and Arduino

## What?

An ESP32 dev module from China running Espressif Arduino framework, powered by Platform.io. This device connects to HomeAssistant REST API to get sensor readings from my home.

Specs:

| Item  | Value  |
|----------------|
| CPU   | 240Mhz |
| RAM   | 320KB  |
| Flash | 4Mb    |

## Why?

An ideal storm was brewing. I was mildly sick, bored and had a couple of long-forgotten ESP32 microcontrollers in the drawer. Taking a career shift into management inspired me to write code for fun!

## Features

 - Fully open-source
 - 3D printed case (SOON!)
 - ESP deep sleep support
 - ESP touch support
 - Non-blocking loop/Timer based
 - SSD1306 0.96" OLED display
 - Supports calling multiple REST API endpoints using the `khoih-prog/AsyncHTTPSRequest_Generic` library
 - Low-memory footprint JSON handling using the `bblanchon/ArduinoJson` library
 - NTP time synchronization

 ## UI Features
  - Displays time, date and date of week 
  - Displays inside sensor reading and outside temperature attribute from HomeAssistant
  - Displays animated icon when connecting to WiFi
  - Uses animated WiFi icon when displaying WiFi RSSI
  - Multiple separate pages of UI - Setup/Connecting to WiFi, normal operation and entering sleep
  - Internal webserver for configuration
  - Displays stock ticker current price (SOON!)

## Setup

Clone this project and open it as a Platform.io project. Create a `srcsecrets.h` file under `src/` directory and set your own values for SSID, WiFi password, HomeAssistant API URL(SSL is supported), HTTP Auth header and HomeAssistant Entity ID's:

```
  static const PROGMEM char SSID[] = "ssid";
  static const PROGMEM char PASSWORD[] = "password";
  static const PROGMEM char API_URL[] =
 "http://homeassistant.ip:8123/api/states/";

  static const PROGMEM char AUTH_HEADER[] = " Bearer MyToken";

  static const PROGMEM char IN_SENSOR_ID[] = "sensor.id";
  static const PROGMEM char OUT_SENSOR_ID[] = "sensor.id";
```
