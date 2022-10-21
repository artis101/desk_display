#include "NTPClient.h"
#include <Arduino.h>
#define ARDUINOJSON_USE_DOUBLE 0
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
  static const PROGMEM char API_URL[] =
 "http://homeassistant.ip:8123/api/states/";

  static const PROGMEM char AUTH_HEADER[] = " Bearer MyToken";

  static const PROGMEM char IN_SENSOR_ID[] = "sensor.id";
  static const PROGMEM char OUT_SENSOR_ID[] = "sensor.id";
*/

RTC_DATA_ATTR int bootCount = 0;

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

#define _ASYNC_HTTP_LOGLEVEL_ 0
AsyncHTTPRequest inTempRequest;
AsyncHTTPRequest outTempRequest;
AsyncHTTPRequest stockPriceRequest;

StaticJsonDocument<48> sensorDoc;
StaticJsonDocument<16> sensorValueFilter;

StaticJsonDocument<96> outSensorDoc;
StaticJsonDocument<32> outSensorValueFilter;

static const PROGMEM char SENSOR_UNAVAILABLE_STR[] = "unavailable";
static const PROGMEM char SENSOR_NO_VALUE_STR[] = "-.-";

// Variables to save date and time
String formattedDateTime;
String formattedTime;
String formattedDate;
String timeStamp;
// JSON request variables
#define SENSOR_RESPONSE_BUFFER_SIZE 4096
uint8_t sensorResponseBuffer[SENSOR_RESPONSE_BUFFER_SIZE];
RTC_DATA_ATTR char insideTempSensorReading[5] = "-.-";
RTC_DATA_ATTR char outsideTempSensorReading[5] = "-.-";

#define HTTP_REQUEST_INTERVAL 60
// poll every 100ms for user interaction
#define UI_LOOP_INTERVAL 0.1
#define MAIN_EVENT_LOOP_INTERVAL 0.5
// API calls every 60 seconds that update the data
Ticker inTempRequestTicker;
Ticker outTempRequestTicker;
Ticker stockPriceRequestTicker;

#define TOUCH_PIN T0
#define TOUCH_TRESHOLD 100 // touch is below 100

bool showActivityIndicator = false;

// counter from 0 to 3 that resets itself
uint8_t currentStep = 0;
#define MAX_STEPS 3
// tracks how long touches are
unsigned long lastTouchStart = 0;
// hold for about 6 seconds to sleep
#define TOUCH_INVERT_SRC_THRESHOLD 5900

// UI elements
static const uint8_t WIFI_ICON_DOT_X = 12;
static const uint8_t WIFI_ICON_DOT_Y = 41;

// draws on display and updates clock every 0.5s
Ticker mainEventLoop;
// updates every 100ms grab user interaction events
Ticker uiLoop;

void displayWiFiTimeout(void) {
  display.clear();
  display.drawString(64, 32, F("WiFi setup failed, restarting in 5s!"));
  display.display();
}

void updateCurrentStep(void) {
  // important update step every time
  currentStep =
      (currentStep > 0 && currentStep % MAX_STEPS == 0) ? 0 : currentStep + 1;
}

void displayWiFiIcon(boolean animate = false) {
  long rssi = WiFi.RSSI();

  if ((!animate && rssi >= -67) || (animate && currentStep) >= 3) {
    display.drawLine(WIFI_ICON_DOT_X - 6, WIFI_ICON_DOT_Y - 8,
                     WIFI_ICON_DOT_X - 5,
                     WIFI_ICON_DOT_Y - 8); // dot top left line
    display.drawLine(WIFI_ICON_DOT_X - 4, WIFI_ICON_DOT_Y - 9,
                     WIFI_ICON_DOT_X + 5,
                     WIFI_ICON_DOT_Y - 9); // dot top middle line
    display.drawLine(WIFI_ICON_DOT_X + 6, WIFI_ICON_DOT_Y - 8,
                     WIFI_ICON_DOT_X + 7,
                     WIFI_ICON_DOT_Y - 8); // dot top right line
  }

  if ((!animate && rssi >= -70) || (animate && currentStep) >= 2) {
    display.drawLine(WIFI_ICON_DOT_X - 4, WIFI_ICON_DOT_Y - 5,
                     WIFI_ICON_DOT_X - 3,
                     WIFI_ICON_DOT_Y - 5); // dot mid left line
    display.drawLine(WIFI_ICON_DOT_X - 2, WIFI_ICON_DOT_Y - 6,
                     WIFI_ICON_DOT_X + 3,
                     WIFI_ICON_DOT_Y - 6); // dot mid middle line
    display.drawLine(WIFI_ICON_DOT_X + 4, WIFI_ICON_DOT_Y - 5,
                     WIFI_ICON_DOT_X + 5,
                     WIFI_ICON_DOT_Y - 5); // dot mid right line
  }

  if ((!animate && rssi >= -80) || (animate && currentStep) >= 1) {
    display.drawLine(WIFI_ICON_DOT_X - 3, WIFI_ICON_DOT_Y - 2,
                     WIFI_ICON_DOT_X - 2,
                     WIFI_ICON_DOT_Y - 2); // dot lower left line
    display.drawLine(WIFI_ICON_DOT_X - 1, WIFI_ICON_DOT_Y - 3,
                     WIFI_ICON_DOT_X + 2,
                     WIFI_ICON_DOT_Y - 3); // dot lower middle line
    display.drawLine(WIFI_ICON_DOT_X + 3, WIFI_ICON_DOT_Y - 2,
                     WIFI_ICON_DOT_X + 4,
                     WIFI_ICON_DOT_Y - 2); // dot lower right line
  }

  display.fillRect(WIFI_ICON_DOT_X, WIFI_ICON_DOT_Y, 2, 2); // the dot
}

void setupWiFi(const char *ssid, const char *password,
               unsigned long rebootTimeoutMillis, boolean quiet = false) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startMillis = millis();
  while (true) {
    Serial.print('.');

    if (!quiet) {
      display.clear();
      displayWiFiIcon(true);
      display.drawString(64, 32, F("WiFi setup..."));
      display.display();
    }

    unsigned long nowMillis = millis();
    if ((unsigned long)(nowMillis - startMillis) >= rebootTimeoutMillis) {
      Serial.println(F(""));
      Serial.println(F("WiFi setup failed, restarting in 5s!"));
      displayWiFiTimeout();
      delay(5000);
      ESP.restart();
    }

    if (WiFi.status() == WL_CONNECTED) {
      if (!quiet) {
        display.clear();
        display.drawString(64, 32, F("WiFi connected!"));
        display.display();
      }
      break;
    } else {
      updateCurrentStep();
      delay(500);
    }
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
  sendApiRequest(&inTempRequest, IN_SENSOR_ID);
}

void sendOutTempSensorApiRequest(void) {
  sendApiRequest(&outTempRequest, OUT_SENSOR_ID);
}

void handleInSensorOkResponse(void *cbVoidPtr, AsyncHTTPRequest *request) {
  char *readingStringPtr = (char *)cbVoidPtr;

  request->responseRead(sensorResponseBuffer, SENSOR_RESPONSE_BUFFER_SIZE);

  DeserializationError error =
      deserializeJson(sensorDoc, sensorResponseBuffer,
                      DeserializationOption::Filter(sensorValueFilter));

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  auto state = sensorDoc["state"].as<const char *>();

  if (state == SENSOR_UNAVAILABLE_STR) {
    strncpy(readingStringPtr, SENSOR_NO_VALUE_STR, strlen(SENSOR_NO_VALUE_STR));
  } else {
    strncpy(readingStringPtr, state, strlen(state));
  }
}

void handleOutSensorOkResponse(void *cbVoidPtr, AsyncHTTPRequest *request) {
  char *readingStringPtr = (char *)cbVoidPtr;

  request->responseRead(sensorResponseBuffer, SENSOR_RESPONSE_BUFFER_SIZE);

  DeserializationError error =
      deserializeJson(outSensorDoc, sensorResponseBuffer,
                      DeserializationOption::Filter(outSensorValueFilter));

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  auto tempFloat = outSensorDoc["attributes"]["temperature"].as<float>();
  char buffer[5]; // xx.y + \0
  sprintf(buffer, "%1g", tempFloat);
  strncpy(readingStringPtr, buffer, strlen(buffer));
}

void apiSensorReadReqCb(void *cbVoidPtr, AsyncHTTPRequest *request,
                        int readyState) {
  if (readyState == readyStateDone) {
    showActivityIndicator = false;

    if (request->responseHTTPcode() == 200) {
      if (request == &inTempRequest) {
        handleInSensorOkResponse(cbVoidPtr, request);
      } else if (request == &outTempRequest) {
        handleOutSensorOkResponse(cbVoidPtr, request);
      }
    }
  } else {
    showActivityIndicator = true;
  }
}

void displayClockRow(boolean draw = false) {
  formattedTime = timeClient.getFormattedTime();
  display.setFont(ArialMT_Plain_24);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 0, formattedTime);
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

void animateSideLines(void) {
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
}

void displaySensorRow(boolean draw = false) {
  String sensorOutputFirstRow = String(insideTempSensorReading) + String("°C") +
                                String(" | ") +
                                String(outsideTempSensorReading) + String("°C");
  String sensorOutputSecondRow = String("WDAY $xxx.yy");

  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  // draw top line
  display.drawLine(25, 25, 103, 25);
  display.drawString(64, 26, sensorOutputFirstRow);
  display.drawString(64, 38, sensorOutputSecondRow);
  // draw bottom line
  display.drawLine(25, 51, 103, 51);
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
    return F("UNK");
  }
}

void displayDateRow(boolean draw = false) {
  formattedDateTime = timeClient.getFormattedDate();
  int splitT = formattedDateTime.indexOf("T");
  formattedDate = formattedDateTime.substring(0, splitT);

  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 51, String(formattedDate + F("   ") + getDow()));
  // draw separator line
  display.drawLine(85, 51, 85, 64);
}

void processLongTouch(void) {
  if (touchRead(TOUCH_PIN) < TOUCH_TRESHOLD) {
    showActivityIndicator = true;

    if (lastTouchStart == 0) { // register touch
      lastTouchStart = millis();
      currentStep = 0;
    }

    unsigned long currMillis = millis();
    unsigned long touchDuration = currMillis - lastTouchStart;

    if (touchDuration > TOUCH_INVERT_SRC_THRESHOLD) {
      display.clear();
      display.setFont(ArialMT_Plain_10);
      display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
      display.drawString(64, 32, F("Turning off..."));
      display.display();
      delay(2000);
      display.displayOff();
      Serial.println(F("Going to sleep now"));
      esp_deep_sleep_start();
    }
  } else {
    showActivityIndicator = false;
    lastTouchStart = 0;
  }
}

void processInteractions(void) { processLongTouch(); }

void updateMainLoop(void) {
  while (!timeClient.update()) {
    timeClient.forceUpdate();
  }

  display.clear();

  displayClockRow();
  displaySensorRow();
  displayDateRow();

  if (showActivityIndicator)
    animateSideLines();

  updateCurrentStep();

  if (!WiFi.isConnected()) {
    displayWiFiIcon(true);
    setupWiFi(SSID, PASSWORD, WIFI_TIMEOUT_MILLIS, true);
  } else if (WiFi.isConnected() && !showActivityIndicator) {
    displayWiFiIcon(false);
  }

  display.display();
}

void touchInterruptCb(void) {}

void setup(void) {
  Serial.begin(115200);
  // Increment boot number and print it every reboot
  ++bootCount;
  Serial.println(F("Hello Hacker!"));
  Serial.println("Boot number: " + String(bootCount));

  if (esp_sleep_enable_touchpad_wakeup() == ESP_OK) {
    touchAttachInterrupt(TOUCH_PIN, touchInterruptCb, TOUCH_TRESHOLD);
  } else {
    Serial.print(F("Cannot enable deep sleep from touch!"));
  }

  Serial.print(F("Initializing display..."));
  display.init();
  // lower brightness is better
  display.setBrightness(32);
  display.clear();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
  Serial.println(F("\tOK!"));

  boolean touchWakeup = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TOUCHPAD;

  Serial.print(F("Connecting to WiFi"));

  if (touchWakeup) {
    display.drawString(64, 32, F("Waking up..."));
    displayWiFiIcon(true);
    display.display();
    setupWiFi(SSID, PASSWORD, WIFI_TIMEOUT_MILLIS, true);
  } else {
    setupWiFi(SSID, PASSWORD, WIFI_TIMEOUT_MILLIS, false);
    delay(2000);
  }

  Serial.println(F("\tOK!"));

  // set up time client and adjust GMT offset
  timeClient.begin();
  // GMT +3 = 3600 * 3
  timeClient.setTimeOffset(3600 * 3);

  uiLoop.attach(UI_LOOP_INTERVAL, processInteractions);
  mainEventLoop.attach(MAIN_EVENT_LOOP_INTERVAL, updateMainLoop);

  Serial.print(F("Fetching data..."));
  // set up requestJSON filters and request ticker
  sensorValueFilter["state"] = true;
  outSensorValueFilter["attributes"]["temperature"] = true;
  // set up the requests we will be making
  inTempRequest.onReadyStateChange(apiSensorReadReqCb,
                                   &insideTempSensorReading);
  inTempRequestTicker.attach(HTTP_REQUEST_INTERVAL, sendInTempSensorApiRequest);

  outTempRequest.onReadyStateChange(apiSensorReadReqCb,
                                    &outsideTempSensorReading);
  outTempRequestTicker.attach(HTTP_REQUEST_INTERVAL,
                              sendOutTempSensorApiRequest);
  sendInTempSensorApiRequest();
  sendOutTempSensorApiRequest();
  Serial.println(F("\tOK!"));
}

void loop(void) {}