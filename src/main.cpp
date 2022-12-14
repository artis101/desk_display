#include "NTPClient.h"
#include <Arduino.h>
#define ARDUINOJSON_USE_DOUBLE 0
#include "SPIFFS.h"
#include <ArduinoJson.h>
#include <AsyncHTTPRequest_Generic.h>
#include <AsyncTCP.h>
#include <EEPROM.h>
#include <ESPAsyncWebServer.h>
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

  static const PROGMEM char AUTH_HEADER_TOKEN[] = " Bearer MyToken";

  static const PROGMEM char IN_SENSOR_ID[] = "sensor.id";
  static const PROGMEM char OUT_SENSOR_ID[] = "sensor.id";
*/

RTC_DATA_ATTR int bootCount = 0;

static const unsigned long WIFI_TIMEOUT_MILLIS = 15000;

#define CONFIG_TEXT_MAX_LENGTH 256
#define CONFIG_FILE_NAME "/.config"
static const char *defaultHttpRequestInterval = "60";
static const char *defaultSleepTouchThreshold = "long";
static const char *defaultScreenBrightness = "dim";

struct DeviceSettings {
  // internal flags
  bool isSetup;
  // wifi settings
  char wifiSsid[CONFIG_TEXT_MAX_LENGTH];
  char wifiPassword[CONFIG_TEXT_MAX_LENGTH];
  // home assistant rest api settings
  char apiUrl[CONFIG_TEXT_MAX_LENGTH];
  char authToken[CONFIG_TEXT_MAX_LENGTH];
  char inSensorId[CONFIG_TEXT_MAX_LENGTH];
  char outSensorId[CONFIG_TEXT_MAX_LENGTH];
  // device settings
  bool displayWifiIndicator;
  char httpRequestInterval[CONFIG_TEXT_MAX_LENGTH];
  char sleepTouchThreshold[CONFIG_TEXT_MAX_LENGTH];
  char screenBrightness[CONFIG_TEXT_MAX_LENGTH];
  bool invertScreen;
  bool debugMode;
};

DeviceSettings deviceSettings;

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

AsyncWebServer server(80);

#define TOUCH_PIN T0
#define TOUCH_TRESHOLD 100 // touch is below 100

bool showActivityIndicator = false;

// counter from 0 to 3 that resets itself
uint8_t currentStep = 0;
#define MAX_STEPS 3
// tracks how long touches are
unsigned long lastTouchStart = 0;
// touch interactivity thresholds
#define SLEEP_TOUCH_THRESHOLD_LONG 5900
#define SLEEP_TOUCH_THRESHOLD_MEDIUM 4400
#define SLEEP_TOUCH_THRESHOLD_SHORT 1400

// UI elements
static const uint8_t WIFI_ICON_DOT_X = 12;
static const uint8_t WIFI_ICON_DOT_Y = 41;

// draws on display and updates clock every 0.5s
Ticker mainEventLoop;
// updates every 100ms grab user interaction events
Ticker uiLoop;

void updateCurrentStep(void) {
  // important update step every time
  currentStep =
      (currentStep > 0 && currentStep % MAX_STEPS == 0) ? 0 : currentStep + 1;
}

DeviceSettings getDefaultSettings(void) {
  DeviceSettings defaultSettings;

  defaultSettings.isSetup = false;
  strcpy(defaultSettings.wifiSsid, SSID);
  strcpy(defaultSettings.wifiPassword, PASSWORD);
  strcpy(defaultSettings.apiUrl, API_URL);
  strcpy(defaultSettings.authToken, AUTH_HEADER_TOKEN);
  strcpy(defaultSettings.inSensorId, IN_SENSOR_ID);
  strcpy(defaultSettings.outSensorId, OUT_SENSOR_ID);
  defaultSettings.displayWifiIndicator = true;
  strcpy(defaultSettings.httpRequestInterval, defaultHttpRequestInterval);
  strcpy(defaultSettings.sleepTouchThreshold, defaultSleepTouchThreshold);
  strcpy(defaultSettings.screenBrightness, defaultScreenBrightness);
  defaultSettings.invertScreen = false;
  defaultSettings.debugMode = false;

  return defaultSettings;
}

void saveSettings(void) {
  Serial.print("Saving configuration...");

  File file = SPIFFS.open(CONFIG_FILE_NAME, "wb");
  file.write((byte *)&deviceSettings, sizeof(deviceSettings));

  Serial.println(F("\tOK!"));
}

void displayWiFiTimeout(void) {
  display.clear();
  display.drawString(64, 32, F("WiFi setup FAILED!"));
  display.display();
}

void displayWiFiIcon(bool animate = false, uint8_t x = WIFI_ICON_DOT_X,
                     uint8_t y = WIFI_ICON_DOT_Y) {
  if (!animate && !deviceSettings.displayWifiIndicator) {
    return;
  }

  long rssi = WiFi.RSSI();

  if ((!animate && rssi >= -67) || (animate && currentStep >= 3)) {
    display.drawLine(x - 6, y - 8, x - 5,
                     y - 8); // dot top left line
    display.drawLine(x - 4, y - 9, x + 5,
                     y - 9); // dot top middle line
    display.drawLine(x + 6, y - 8, x + 7,
                     y - 8); // dot top right line
  }

  if ((!animate && rssi >= -70) || (animate && currentStep >= 2)) {
    display.drawLine(x - 4, y - 5, x - 3,
                     y - 5); // dot mid left line
    display.drawLine(x - 2, y - 6, x + 3,
                     y - 6); // dot mid middle line
    display.drawLine(x + 4, y - 5, x + 5,
                     y - 5); // dot mid right line
  }

  if ((!animate && rssi >= -80) || (animate && currentStep >= 1)) {
    display.drawLine(x - 3, y - 2, x - 2,
                     y - 2); // dot lower left line
    display.drawLine(x - 1, y - 3, x + 2,
                     y - 3); // dot lower middle line
    display.drawLine(x + 3, y - 2, x + 4,
                     y - 2); // dot lower right line
  }

  display.fillRect(x, y, 2, 2); // the dot
}

void connectToAP(bool quiet = false) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(deviceSettings.wifiSsid, deviceSettings.wifiPassword);

  Serial.print(F("Connecting to WiFi"));
  unsigned long startMillis = millis();
  while (true) {
    Serial.print('.');

    unsigned long nowMillis = millis();
    if ((unsigned long)(nowMillis - startMillis) >= WIFI_TIMEOUT_MILLIS) {
      Serial.println(F("\tFAIL!"));
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
      Serial.println(F("\tOK!"));
      break;
    } else {
      if (!quiet) {
        display.clear();
        displayWiFiIcon(true);
        display.drawString(64, 32, F("WiFi setup..."));
        display.display();
      }

      updateCurrentStep();
      delay(500);
    }
  }
}

void setupWiFi(bool quiet = false) {
  if (deviceSettings.isSetup) {
    connectToAP(quiet);
  }
}

void handleNotFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "File Not Found");
}

void sendApiRequest(AsyncHTTPRequest *request, String sensorId) {
  static bool requestOpenResult;

  if (request->readyState() == readyStateUnsent ||
      request->readyState() == readyStateDone) {
    String apiAuthHeader =
        String(" Bearer " + String(deviceSettings.authToken));
    String apiUrlStr = String(deviceSettings.apiUrl + sensorId);

    requestOpenResult = request->open("GET", apiUrlStr.c_str());
    request->setReqHeader("Accept", "application/json");
    request->setReqHeader("Authorization", apiAuthHeader.c_str());

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
  sendApiRequest(&inTempRequest, deviceSettings.inSensorId);
}

void sendOutTempSensorApiRequest(void) {
  sendApiRequest(&outTempRequest, deviceSettings.outSensorId);
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

void displayClockRow(bool draw = false) {
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

void displaySensorRow(bool draw = false) {
  String sensorOutputFirstRow = String(insideTempSensorReading) + String("??C") +
                                String(" | ") +
                                String(outsideTempSensorReading) + String("??C");
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

void displayDateRow(bool draw = false) {
  formattedDateTime = timeClient.getFormattedDate();
  int splitT = formattedDateTime.indexOf("T");
  formattedDate = formattedDateTime.substring(0, splitT);

  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 51, String(formattedDate + F("  ") + getDow()));
  // draw separator line
  display.drawLine(82, 51, 82, 64);
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

    if (touchDuration > SLEEP_TOUCH_THRESHOLD_LONG) {
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

void processMainUI(void) {
  while (WiFi.isConnected() && !timeClient.update()) {
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
    setupWiFi(true);
  } else if (WiFi.isConnected() && !showActivityIndicator) {
    displayWiFiIcon(false);
  }

  display.display();
}

void processSetupUI(void) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(30, 40, F("SSID: dd_setup"));
  display.drawString(30, 50, F("PW:   12345"));

  displayWiFiIcon(true, 64, 26);
  display.display();

  updateCurrentStep();
}

void updateMainLoop(void) {
  if (deviceSettings.isSetup) {
    processMainUI();
  } else {
    processSetupUI();
  }
}

void initDisplay(void) {
  Serial.print(F("Initializing display..."));
  display.init();
  // lower brightness is better
  display.setBrightness(32);
  display.clear();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
  Serial.println(F("\tOK!"));
}

String processor(const String &var) {
  if (var == "IS_SETUP") {
    return String(deviceSettings.isSetup ? "checked" : "");
  } else if (var == "WIFI_SSID") {
    return String(deviceSettings.wifiSsid);
  } else if (var == "WIFI_PASSWORD") {
    return String(deviceSettings.wifiPassword);
  } else if (var == "HA_API") {
    return String(deviceSettings.apiUrl);
  } else if (var == "AUTH_TOKEN") {
    return String(deviceSettings.authToken);
  } else if (var == "IN_SENSOR_ID") {
    return String(deviceSettings.inSensorId);
  } else if (var == "OUT_SENSOR_ID") {
    return String(deviceSettings.outSensorId);
  } else if (var == "WIFI_ICON_STATE") {
    return String(deviceSettings.displayWifiIndicator ? "checked" : "");
  } else if (var == "HTTP_REQUEST_INTERVAL") {
    return String(deviceSettings.httpRequestInterval);
  } else if (var == "LONG_TOUCH_SELECTED") {
    return String(strcmp(deviceSettings.sleepTouchThreshold, "long") == 0
                      ? "selected"
                      : "");
  } else if (var == "MEDIUM_TOUCH_SELECTED") {
    return String(strcmp(deviceSettings.sleepTouchThreshold, "medium") == 0
                      ? "selected"
                      : "");
  } else if (var == "SHORT_TOUCH_SELECTED") {
    return String(strcmp(deviceSettings.sleepTouchThreshold, "short") == 0
                      ? "selected"
                      : "");
  } else if (var == "HIGH_BRIGHTNESS_SELECTED") {
    return String(
        strcmp(deviceSettings.screenBrightness, "high") == 0 ? "selected" : "");
  } else if (var == "MEDIUM_BRIGHTNESS_SELECTED") {
    return String(strcmp(deviceSettings.screenBrightness, "medium") == 0
                      ? "selected"
                      : "");
  } else if (var == "DIM_BRIGHTNESS_SELECTED") {
    return String(
        strcmp(deviceSettings.screenBrightness, "dim") == 0 ? "selected" : "");
  } else if (var == "INVERT_SCREEN") {
    return String(deviceSettings.invertScreen ? "checked" : "");
  } else if (var == "ENABLE_DEBUG") {
    return String(deviceSettings.debugMode ? "checked" : "");
  } else if (var == "SETUP_STATE") {
    return String(
        deviceSettings.isSetup
            ? "is successfully set up!"
            : "is not set up! Please fill out the form below to start.");
  } else if (var == "DEBUG_MODE_STYLING") {
    return String(deviceSettings.debugMode ? "" : " style=\"display:none\"");
  }

  return String();
}

void setupWebServer(void) {
  Serial.print("Starting HTTP server...");
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/index.html", "text/html", false, processor);
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/style.css", "text/css");
  });
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("resetChip", true)) {
      DeviceSettings defaultSettings = getDefaultSettings();
      deviceSettings = defaultSettings;
    } else {
      if (request->hasParam("wifiSsid", true)) {
        AsyncWebParameter *p = request->getParam("wifiSsid", true);
        String v = p->value();

        strcpy(deviceSettings.wifiSsid, v.c_str());
      } else {
        strcpy(deviceSettings.wifiSsid, "");
      }
      if (request->hasParam("wifiPassword", true)) {
        AsyncWebParameter *p = request->getParam("wifiPassword", true);
        String v = p->value();

        strcpy(deviceSettings.wifiPassword, v.c_str());
      } else {
        strcpy(deviceSettings.wifiPassword, "");
      }

      if (request->hasParam("apiUrl", true)) {
        AsyncWebParameter *p = request->getParam("apiUrl", true);
        String v = p->value();

        strcpy(deviceSettings.apiUrl, v.c_str());
      } else {
        strcpy(deviceSettings.apiUrl, "");
      }
      if (request->hasParam("authToken", true)) {
        AsyncWebParameter *p = request->getParam("authToken", true);
        String v = p->value();

        strcpy(deviceSettings.authToken, v.c_str());
      } else {
        strcpy(deviceSettings.authToken, "");
      }

      if (request->hasParam("inSensorId", true)) {
        AsyncWebParameter *p = request->getParam("inSensorId", true);
        String v = p->value();

        strcpy(deviceSettings.inSensorId, v.c_str());
      } else {
        strcpy(deviceSettings.inSensorId, "");
      }
      if (request->hasParam("outSensorId", true)) {
        AsyncWebParameter *p = request->getParam("outSensorId", true);
        String v = p->value();

        strcpy(deviceSettings.outSensorId, v.c_str());
      } else {
        strcpy(deviceSettings.outSensorId, "");
      }

      if (request->hasParam("httpRequestInterval", true)) {
        AsyncWebParameter *p = request->getParam("httpRequestInterval", true);
        String v = p->value();

        strcpy(deviceSettings.httpRequestInterval, v.c_str());
      } else {
        strcpy(deviceSettings.httpRequestInterval, defaultHttpRequestInterval);
      }
      if (request->hasParam("sleepTouchThreshold", true)) {
        AsyncWebParameter *p = request->getParam("sleepTouchThreshold", true);
        String v = p->value();

        if (v == "long" || v == "medium" || v == "short") {
          strcpy(deviceSettings.sleepTouchThreshold, v.c_str());
        } else {
          strcpy(deviceSettings.sleepTouchThreshold, defaultSleepTouchThreshold);
        }
      }
      if (request->hasParam("screenBrightness", true)) {
        AsyncWebParameter *p = request->getParam("screenBrightness", true);
        String v = p->value();

        if (v == "dim" || v == "medium" || v == "high") {
          strcpy(deviceSettings.screenBrightness, v.c_str());
        } else {
          strcpy(deviceSettings.screenBrightness, defaultScreenBrightness);
        }
      }

      // update booleans
      deviceSettings.displayWifiIndicator =
          request->hasParam("displayWifiIndicator", true);
      deviceSettings.invertScreen = request->hasParam("invertScreen", true);
      deviceSettings.debugMode = request->hasParam("debugMode", true);
      deviceSettings.isSetup = request->hasParam("isSetup", true);
    } 

    saveSettings();

    request->redirect("/");
  });

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println(F("\tOK!"));

  Serial.println(F("********************************"));
  Serial.print(F("I'm available at http://"));
  Serial.println(WiFi.localIP());
  Serial.println(F("********************************"));
}

void initWifiAndSleep(void) {
  bool wokeUpFromTouch =
      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TOUCHPAD;

  if (wokeUpFromTouch) {
    display.drawString(64, 32, F("Waking up..."));
    displayWiFiIcon(true);
    display.display();
    setupWiFi(true);
  } else {
    setupWiFi(false);
    delay(2000);
  }
}

void touchInterruptCb(void) {}

void initDeviceSettings(void) {
  // default config does not exist, dump it!
  if (!SPIFFS.exists(CONFIG_FILE_NAME)) {
    Serial.print("Default config doesn't exist, creating...");
    DeviceSettings defaultSettings = getDefaultSettings();

    File file = SPIFFS.open(CONFIG_FILE_NAME, "wb");

    file.write((byte *)&defaultSettings, sizeof(defaultSettings));

    deviceSettings = defaultSettings;

    Serial.println(F("\tOK!"));
  } else {
    Serial.print("Device config exists, loading...");

    File file = SPIFFS.open(CONFIG_FILE_NAME, "rb");
    file.read((byte *)&deviceSettings, sizeof(deviceSettings));

    Serial.println(F("\tOK!"));
  }
}

void initTimeClient(void) {
  Serial.print(F("Initializing NTP client..."));
  // set up time client and adjust GMT offset
  timeClient.begin();
  // GMT +3 = 3600 * 3
  timeClient.setTimeOffset(3600 * 3);
  Serial.println(F("\tOK!"));
}

void initDataFetch(void) {
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

void setup(void) {
  Serial.begin(115200);
  // Increment boot number and print it every reboot
  ++bootCount;
  Serial.println(F("Hello Hacker!"));
  Serial.println("Boot number: " + String(bootCount));

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS!");
    return;
  }

  initDeviceSettings();

  if (esp_sleep_enable_touchpad_wakeup() == ESP_OK) {
    touchAttachInterrupt(TOUCH_PIN, touchInterruptCb, TOUCH_TRESHOLD);
  } else {
    Serial.print(F("Cannot enable deep sleep from touch!"));
  }

  initDisplay();
  initWifiAndSleep();

  uiLoop.attach(UI_LOOP_INTERVAL, processInteractions);
  mainEventLoop.attach(MAIN_EVENT_LOOP_INTERVAL, updateMainLoop);

  if (deviceSettings.isSetup && WiFi.isConnected()) {
    initTimeClient();
    initDataFetch();
    setupWebServer();
  }
}

void loop(void) {}