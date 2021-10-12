/**
 * Board: ESP32 Arduino => ESP32 Dev Module
 * CPU Frequency: 240MHz (WiFi/BT)
 * Flash Frequency: 80MHz
 */

#include "configuration.h"
#include "userSettings.h"
#include "src/WebSocketsServer/src/WebSocketsServer.h"
#include "src/homography/homography.h"
#include "esp32-hal-cpu.h"
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <EEPROM.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include "FS.h"
#include "SPIFFS.h"
#include "src/ESP32WebServer/src/ESP32WebServer.h"

#if defined(HW_DIY_BASIC)
  #include "src/wiiCam/wiiCam.h"
  wiiCam IRsensor;
#elif defined(HW_DIY_FULL)
  #include "src/wiiCam/wiiCam.h"
  #include "src/TinyPICO_Helper_Library/src/TinyPICO.h"
  wiiCam IRsensor;
  TinyPICO tp = TinyPICO();
#elif defined(HW_BETA)
  #include "src/PAJ7025R3/PAJ7025R3.h"
  #include "src/IR32/src/IRRecv.h"
  PAJ7025R3 IRsensor(PAJ_CS);
  IRRecv IDsensor;
#endif

#define WEBSOCKETS_NETWORK_TYPE NETWORK_ESP32 
#define MSG_LENGTH 100*MAX_IR_POINTS
#define WS_MODE_OFF 0
#define WS_MODE_SERVER 1
#define WS_MODE_CLIENT 2

ESP32WebServer webServer(80);
DNSServer dnsServer;

bool debug = false;
bool serialOutput = false;

uint8_t maxIRpoints = 16;

bool calibration = false;
bool offsetOn = false;
bool mirrorX = false;
bool mirrorY = false;
bool rotation = false;
int16_t offsetX = 0;
int16_t offsetY = 0;
bool calOpen = true;
volatile uint8_t calibrationProcedure = 0;
bool calibrationRunning = false;
float framePeriod = 50;
unsigned long timer = 0;
uint8_t averageCount = 10;

uint8_t wsMode = WS_MODE_SERVER;
uint16_t wsPort = WS_PORT_DEFAULT;
String wsIP = "";
bool wsConnected = false;
uint8_t wsClients = 0;

uint16_t scale[2];

uint8_t irMode;
uint16_t irAddress;

float vBat = 0;
uint8_t chargeState = 0;
bool lowBattery = false;

unsigned long pingTimer = 0;
unsigned long IRtimeout = 0;
volatile bool exposureDone = false;

String ssidString = "";
String passwordString = "";
String nameString = "";

uint8_t batteryCounter = 0;

void IRAM_ATTR pajInterruptHandler(){
  exposureDone = true;
}

bool noneCheck = false;

unsigned long IRtimer = 0;
unsigned long ledTimer = 0;
unsigned long leftLedTimer = 0;

WebSocketsServer webSocketServer = WebSocketsServer(wsPort);
homography cal;

void setup() {
  initialization();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    dnsServer.processNextRequest();
  }

  //checkWebServer();  
  webServer.handleClient();

  if (Serial.available() > 0) {
    bool result = checkSerial(); 
    if (result == true) if (debug) Serial.println("OK");
    else if (result == false) if (debug) Serial.println("ERROR"); 
  }
  
  wsPing();
  
  webSocketServer.loop();

  getCal();
    
  #if defined(HW_BETA)
    if (exposureDone) {
      exposureDone = false;
      IRtimer = millis();
      timer = micros();
      readIR();
      autoExpose();
    }

    while(IDsensor.available()){
      char* rcvGroup;
      uint32_t result = IDsensor.read(rcvGroup);
      //Serial.println("IR ID sensor0:" + (String)result + "\tAddress: " + (String)(result>>8) + "\tMode: " + (String)(result&255));
      if (rcvGroup == "MP" && result) {
          irMode = result&255;
          irAddress = result>>8;
          if (debug) Serial.println("IR ID sensor. Address: " + (String)irAddress + "\tMode: " + (String)irMode);
      }
    }
  
    if (millis()-IRtimeout >= 10) {
      bool productId = IRsensor.checkProductId();
      //Serial.println("ProductID: " + (String)IRsensor.checkProductId());
      if (productId == false) {
        if(debug) Serial.println("Sensor reset");
        IRsensor.initialize();
        initializeEepromIRsensor();
      }
      IRtimeout = millis();
    }
  #else
    if (millis() - IRtimer > framePeriod) {
      IRtimer = millis();
      readIR();
    }
  #endif

  #if defined(HW_DIY_FULL) 
    if (millis()-ledTimer >= 2){
      getBatteryVoltage();
      ledTimer = millis();
      if (webSocketServer.connectedClients() > 0) setRightLED(true);
      else                                        setRightLED(false);
    } 

    if (millis()-leftLedTimer >= 20) {
      setLeftLED(chargeState);
      leftLedTimer = millis();
    }
  #elif defined(HW_BETA)
    if (millis()-ledTimer >= 100){
      batteryCounter++;
      
      //Check battery voltage every second
      if (batteryCounter == 10) {
        batteryCounter = 0;
        getBatteryVoltage();
      }
    
      ledTimer = millis();
      if (webSocketServer.connectedClients() > 0) setRightLED(true);
      else                                        setRightLED(false);
    }    
                                     
    if (millis()-leftLedTimer >= 20) {
      setLeftLED(chargeState);
      leftLedTimer = millis();
    }
  #endif
}

float batStorage = 0;
uint16_t chargeCounter = 0;
uint16_t chargeSum = 0;
bool usbActive = false;
uint8_t usbCounter = 0;
uint8_t batPercentage = 0;

void getBatteryVoltage() {
  #if defined(HW_DIY_FULL)
    usbActive = analogRead(USB_ACTIVE)>1000 ? true : false;
    batStorage += tp.GetBatteryVoltage();
    chargeSum += (int)tp.IsChargingBattery();
    chargeCounter++;
    
    if (chargeCounter >= 500) {
      chargeState = 0;
      if (chargeSum < 40) { //full battery
        chargeState = BATT_FULL;
      }
      else if (chargeSum < 75){ //no battery or not charging
        chargeState = BATT_NOT_CHARGING;
      }
      else {                  //charging
        chargeState = BATT_CHARGING;
      }
      setLeftLED(chargeState);
      vBat = processBattery(batStorage/chargeCounter);
      if (debug) Serial.println("Battery Voltage: " + (String)vBat + "V\tCharge Sum: " + (String)chargeSum + "\tCharge State: " + (String)chargeState);
      batStorage = 0;
      chargeSum = 0;
      chargeCounter = 0;
    }
    
  #elif defined(HW_BETA)
    usbActive = digitalRead(USB_ACTIVE);
    //Disable charging to get more accurate battery voltage
    //digitalWrite(CHARGE_EN,HIGH);
    
    //Short delay for battery voltage to settle after charge disable
    delay(50);
    
    digitalWrite(VBAT_EN,HIGH);
    unsigned long bat = 0;
    for (int i=0; i<10; i++) bat += analogRead(VBAT_SENSE);
    bat *= 0.1;
    vBat = processBattery(bat*3.1/(4096*0.68));
    batPercentage = getBatteryPercentage(vBat);
    if (debug) Serial.println("Battery Voltage: " + (String)vBat + "V\tPercentage: " + (String)batPercentage + "%\tCharge Sum: " + (String)chargeSum + "\tCharge State: " + (String)chargeState);

    if (batPercentage >= 98) {
      chargeState = BATT_FULL;
    }
    else if (digitalRead(VBAT_STAT == LOW) && vBat < 4.15) {
      chargeState = BATT_CHARGING;
    }
    
    setLeftLED(chargeState);

    //Enable charging
    digitalWrite(CHARGE_EN,LOW);
  #endif
}

float voltageStorage[10] = {0,0,0,0,0,0,0,0,0,0};
uint8_t voltageCount = 0;

float processBattery(float vbat) {
  for (int i=8; i>=0; i--) {
    voltageStorage[i+1] = voltageStorage[i];
  }
  voltageStorage[0] = vbat;
  if (voltageCount < 10) voltageCount++;
  float tempVoltage = 0;
  for (int i=0; i<voltageCount; i++) tempVoltage += voltageStorage[i];
  return tempVoltage / voltageCount;
}

/**
 * Get a very rough estimate of the battery voltage.
 */
uint8_t getBatteryPercentage(float v) {
  if (v > 4.2) v = 4.20;

  if (v < 3.25) return round(80*(v-3.00));
  else if (v >= 3.25 && v < 3.75) return round(20 + 120*(v-3.25));
  else if (v >= 3.75 && v < 4.00) return round(80 + 60*(v-3.75));
  else if (v >= 4.00) return round(95 + 50*(v-4.00));
  else return 0;
}

void connectWifi(const char* ssid, const char* password, uint8_t ssidLength, uint8_t passwordLength) {
  ssidString = "";
  for (int i=0; i<ssidLength; i++) ssidString += ssid[i];
  
  if (WiFi.isConnected()) {
    Serial.println("Disconnecting from: \"" + (String)WiFi.SSID() + "\"");
  }
  Serial.print("Attempting to connect to \"" + (String)ssid + "\". Please wait");
  WiFi.disconnect();  
  WiFi.mode(WIFI_OFF); 
  WiFi.mode(WIFI_AP);
      
  WiFi.begin(ssid,password);
  
  int counter = 0;
  while(WiFi.status() != WL_CONNECTED) {
    counter++;
    if (counter >= WIFI_TIMEOUT*2) {
      Serial.println("\nConnection failed, starting access point\n");

      //Start access point
      WiFi.disconnect();  
      WiFi.mode(WIFI_OFF); 
      WiFi.mode(WIFI_AP);
      char name[nameString.length()];
      stringToChar(nameString, name);
      IPAddress apIP(192, 168, 4, 1);
      WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
      WiFi.softAP(name);
      dnsServer.start(53, "*", apIP);
      break;
    }
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    String ipAddress = WiFi.localIP().toString().c_str();
    Serial.println("\nWiFi connected with IP address: " + ipAddress + ", using device name: " + nameString);
    configureDNS(nameString);
  }
  else {
    IPAddress IP = WiFi.softAPIP();
    String ipAddress = IP.toString().c_str();
    Serial.println("\nStarted WiFi access point on IP address: " + ipAddress + ", using device name: " + nameString);
  }

  //Start websocket server
  wsMode = WS_MODE_SERVER;  //for now, force server mode
  if (wsMode == WS_MODE_SERVER) {
    webSocketServer.begin();
    Serial.println("Websocket server started on port: " + (String)wsPort + "\n");
  }
  
  //Connect to websocket server
  else if (wsMode == WS_MODE_CLIENT) {
    
  }
  
  webSocketServer.onEvent(webSocketServerEvent);
}

void configureDNS(String name) {
  char hostName[name.length()];
  name.toCharArray(hostName,name.length()+1);

  if (!MDNS.begin(hostName)) {
      Serial.println("Error setting up MDNS responder!");
      while(1){
          delay(1000);
      }
  }
}

#if defined(HW_DIY_FULL) || defined(HW_BETA)
  /**
   * Set the right led, depending on whether there are any active connections
   */
  void setRightLED(bool connections){
    if (connections) {
      //digitalWrite(LEDR_R,LOW);
      //digitalWrite(LEDR_G,HIGH);
      ledcWrite(LEDR_R_CH, 0);
      ledcWrite(LEDR_G_CH, LEDR_G_INTENSITY);
    }
    else {
      //digitalWrite(LEDR_R,HIGH);
      //digitalWrite(LEDR_G,LOW);
      ledcWrite(LEDR_R_CH, LEDR_R_INTENSITY);
      ledcWrite(LEDR_G_CH, 0);
    }
  }

  int16_t battLedDuty = 0;
  bool battLedDir = 1;
  
  /**
   * Set the left led, depending on power/battery state
   */
  void setLeftLED(int batteryState){
    if (usbActive) {
      if (batteryState == BATT_NOT_CHARGING) {  //no battery or not charging
        ledcWrite(LEDL_R_CH, 0);
        ledcWrite(LEDL_G_CH, 0);
      }
      else if (batteryState == BATT_CHARGING) { //charging

        //digitalWrite(LEDL_R,HIGH);
        ledcWrite(LEDL_R_CH, battLedDuty);
   
        if (battLedDir) battLedDuty += LED_STEPSIZE;
        else battLedDuty -= LED_STEPSIZE;
        
        if (battLedDuty >= LEDL_R_INTENSITY) {
          battLedDuty = LEDL_R_INTENSITY;
          battLedDir = 0;
        }
        else if (battLedDuty < 0) {
          battLedDuty = 0;
          battLedDir = 1;
        }

        ledcWrite(LEDL_G_CH, 0);
      }
      else if (batteryState == BATT_FULL) {    //full battery
        ledcWrite(LEDL_R_CH, 0);
        ledcWrite(LEDL_G_CH, LEDL_G_INTENSITY);
      }
    }
    else {
      chargeState = BATT_NOT_CHARGING;
      if (batPercentage < BAT_WARNING_PERCENTAGE){
        lowBattery = true;
        ledcWrite(LEDL_R_CH, LEDL_R_INTENSITY);
        ledcWrite(LEDL_G_CH, 0);
      }
      else {
        lowBattery = false;
        ledcWrite(LEDL_R_CH, 0);
        ledcWrite(LEDL_G_CH, LEDL_G_INTENSITY);
      }
    }
  }
#endif

char* stringToChar(String in, char* out) {
  in.toCharArray(out,in.length()+1);
}
