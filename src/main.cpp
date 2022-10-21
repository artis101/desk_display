#include "NTPClient.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncHTTPRequest_Generic.h>
#include <FS.h>
#include <SPI.h>
#include <SSD1306Wire.h>
#include <Ticker.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>

#include "srcsecrets.h"
/**
 * Define in srcsecrets.h
 *
  static const PROGMEM char SSID[] = "ssid";
  static const PROGMEM char PASSWORD[] = "password";
  static const PROGMEM char API_URL[] = "scheme://api.url/api";
  static const PROGMEM char AUTH_HEADER[] = " Bearer YOURTOKEN";
*/

static const unsigned long WIFI_TIMEOUT_MILLIS = 15000;

#define NTP_OFFSET 19800 // In seconds

#define NTP_INTERVAL 60 * 1000 // In miliseconds

#define NTP_ADDRESS "lv.pool.ntp.org"

WiFiUDP ntpUDP;

NTPClient timeClient(ntpUDP, NTP_ADDRESS, NTP_OFFSET, NTP_INTERVAL);

#define OLED_ROTATION 0x3c
#define OLED_SCL 22
#define OLED_SDA 21

SSD1306Wire display(OLED_ROTATION, OLED_SDA, OLED_SCL); // ADDRESS, SDA, SCL

#define HTTP_REQUEST_INTERVAL 60 // 300
#define _ASYNC_HTTP_LOGLEVEL_ 0
AsyncHTTPRequest inTempRequest;
Ticker inTempRequestTicker;
AsyncHTTPRequest outTempRequest;
Ticker outTempRequestTicker;
AsyncHTTPRequest stockPriceRequest;
Ticker stockPriceRequestTicker;

// counter from 0 to 3 that resets itself
uint8_t currentStep = 0;
// for millis() based updates
unsigned long lastUpdate = 0;

StaticJsonDocument<48> sensorDoc;
StaticJsonDocument<16> sensorValueFilter;

static const PROGMEM char SEPERATOR_HYPHEN[] = " - ";
static const PROGMEM char SEPERATOR_PIPE[] = " | ";
static const PROGMEM char SENSOR_UNAVAILABLE_STR[] = "unavailable";
static const PROGMEM char SENSOR_NO_VALUE_STR[] = "-.-";

// Variables to save date and time
String formattedDateTime;
String formattedTime;
String formattedDate;
String timeStamp;
// JSON request variables
#define SENSOR_RESPONSE_BUFFER_SIZE 512
uint8_t sensorResponseBuffer[SENSOR_RESPONSE_BUFFER_SIZE];
char insideTempSensorReading[5] = "-.-";
char outsideTempSensorReading[5] = "-.-";

void setupWiFi(const char *ssid, const char *password,
               unsigned long rebootTimeoutMillis) {
  Serial.print(F("Connecting to WiFi"));
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startMillis = millis();
  while (true) {
    Serial.print('.');

    display.clear();
    display.drawString(64, 32, F("WiFi setup..."));
    display.display();

    if (WiFi.status() == WL_CONNECTED) {
      display.clear();
      display.drawString(64, 32, F("WiFi connected!"));
      display.display();
      Serial.println(
          F("WiFi setup successful! Booting device into loop in 2000ms"));
      delay(2000);
      break;
    }

    unsigned long nowMillis = millis();
    if ((unsigned long)(nowMillis - startMillis) >= rebootTimeoutMillis) {
      display.clear();
      display.drawString(64, 32, F("WiFi setup FAIL, reboot in 5s"));
      display.display();
      Serial.println(
          F("WiFi connection failed... Restart the device and try again"));
      delay(5000);
      ESP.restart();
    }

    delay(500);
  }
}

void sendApiRequest(AsyncHTTPRequest *request, String sensorId) {
  static bool requestOpenResult;

  if (request->readyState() == readyStateUnsent ||
      request->readyState() == readyStateDone) {
    String apiUrlStr = String(API_URL + sensorId);

    requestOpenResult = request->open("GET", apiUrlStr.c_str());
    request->setReqHeader("Accept", "application/json");
    request->setReqHeader("Authorization", AUTH_HEADER);

    if (requestOpenResult) {
      request->send();
    } else {
      Serial.println(F("Can't open Request"));
    }
  } else {
    Serial.println(F("Can't send Request"));
  }
}

void sendInTempSensorApiRequest(void) {
  sendApiRequest(&inTempRequest,
                 F("sensor.temperature_humidity_sensor_51b9_temperature"));
}

void sendOutTempSensorApiRequest(void) {
  sendApiRequest(&outTempRequest,
                 F("sensor.temperature_humidity_sensor_394e_temperature"));
}

void apiSensorReadRequestCb(void *cbVoidPtr, AsyncHTTPRequest *request,
                            int readyState) {
  char *readingStringPtr = (char *)cbVoidPtr;

  if (readyState == readyStateDone) {
    if (request->responseHTTPcode() == 200) {
      request->responseRead(sensorResponseBuffer, SENSOR_RESPONSE_BUFFER_SIZE);

      DeserializationError error =
          deserializeJson(sensorDoc, sensorResponseBuffer,
                          DeserializationOption::Filter(sensorValueFilter));

      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
      } else {
        auto state = sensorDoc["state"].as<const char *>();

        if (state == SENSOR_UNAVAILABLE_STR) {
          strncpy(readingStringPtr, SENSOR_NO_VALUE_STR,
                  strlen(SENSOR_NO_VALUE_STR));
        } else {
          strncpy(readingStringPtr, state, strlen(state));
        }
      }
    }
  }
}

void setup(void) {
  Serial.begin(115200);
  Serial.println(F("Starting..."));
  Serial.println();

  display.init();
  // clear the display
  display.clear();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
  Serial.println(F("Display initialized, connecting to WiFi"));

  setupWiFi(SSID, PASSWORD, WIFI_TIMEOUT_MILLIS);

  // set up time client and adjust GMT offset
  timeClient.begin();
  // GMT +3 = 3600 * 3
  timeClient.setTimeOffset(3600 * 3);

  // set up requestJSON filters and request ticker
  sensorValueFilter["state"] = true;
  // set up the requests we will be making
  inTempRequest.onReadyStateChange(apiSensorReadRequestCb,
                                   &insideTempSensorReading);
  inTempRequestTicker.attach(HTTP_REQUEST_INTERVAL, sendInTempSensorApiRequest);
  sendInTempSensorApiRequest();

  outTempRequest.onReadyStateChange(apiSensorReadRequestCb,
                                   &outsideTempSensorReading);
  outTempRequestTicker.attach(HTTP_REQUEST_INTERVAL, sendOutTempSensorApiRequest);
  sendOutTempSensorApiRequest();
}

void displayClockRow(boolean draw = false) {
  formattedTime = timeClient.getFormattedTime();
  display.setFont(ArialMT_Plain_24);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, formattedTime);

  if (draw)
    display.display();
}

void drawl2o4(void) {
  display.drawLine(10, 36, 10, 40);
  display.drawLine(118, 36, 118, 40);
}

void drawl3o4(void) {
  display.drawLine(15, 32, 15, 44);
  display.drawLine(113, 32, 113, 44);
}

void drawl4o4(void) {
  display.drawLine(20, 28, 20, 48);
  display.drawLine(108, 28, 108, 48);
}

void displaySensorRow(boolean draw = false) {
  String sensorOutputFirstRow = String(insideTempSensorReading) + String("°C") +
                                String(" | ") +
                                String(outsideTempSensorReading) + String("°C");
  String sensorOutputSecondRow = String("WDAY $xxx.yy");

  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawLine(25, 25, 103, 25);
  display.drawString(64, 26, sensorOutputFirstRow);
  display.drawString(64, 38, sensorOutputSecondRow);
  display.drawLine(25, 53, 103, 53);

  switch (currentStep) {
  case 0:
  default:
    break;
  case 1:
    drawl4o4();
    break;
  case 2:
    drawl3o4();
    drawl4o4();
    break;
  case 3:
    drawl2o4();
    drawl3o4();
    drawl4o4();
    break;
  }

  if (draw)
    display.display();
}

String getDow(void) {
  switch (timeClient.getDay()) {
  case 0:
    return F("Sun");
  case 1:
    return F("Mon");
  case 2:
    return F("Tue");
  case 3:
    return F("Wed");
  case 4:
    return F("Thu");
  case 5:
    return F("Fri");
  case 6:
    return F("Sat");
  default:
    return F("UNKNOWN");
  }
}

void displayDateRow(boolean draw = false) {
  formattedDateTime = timeClient.getFormattedDate();
  int splitT = formattedDateTime.indexOf("T");
  formattedDate = formattedDateTime.substring(0, splitT);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 54,
                     String(formattedDate + String(SEPERATOR_PIPE) + getDow()));

  if (draw)
    display.display();
}

void loop(void) {
  while (!timeClient.update()) {
    timeClient.forceUpdate();
  }

  display.clear();

  displayClockRow();
  displaySensorRow();
  displayDateRow();

  display.display();

  currentStep = (currentStep > 0 && currentStep % 3 == 0) ? 0 : currentStep + 1;

  delay(500);
}