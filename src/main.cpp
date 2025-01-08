#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <Adafruit_BMP085.h>
#include <TinyGPSPlus.h>

// Pin Definitions
#define RXD2 16
#define TXD2 17
#define SD_CS_PIN 5

// Global Objects
WebServer server(80);
RTC_DS3231 rtc;
Adafruit_BMP085 bmp;
TinyGPSPlus gps;
HardwareSerial neo6m(2);

// Configuration Structure
struct Config {
  char ssid[32] = "Aspol Tracker";
  char password[32] = "sulungresearch";
  char deviceName[32] = "PressureTracker";
  float pressureThreshold = 1013.25;
};

Config currentConfig;
bool sdCardAvailable = false;
bool rtcInitialized = false;
bool bmpInitialized = false;

// Function Prototypes
void initRTC();
void initBMP();
bool initSDCard();
void loadConfig();
void saveConfig();
void setupWiFi();
void setupWebServer();
void handleRoot();
void handleConfig();
void logGPSData(float pressure);
void processGPS();
void checkPressureAndLog();

void initRTC() {
  Wire.begin();
  if (!rtc.begin()) {
    Serial.println("RTC not found");
    rtcInitialized = false;
  } else {
    rtcInitialized = true;
    if (rtc.lostPower()) {
      Serial.println("RTC lost power, setting time!");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }
}

void initBMP() {
  if (!bmp.begin()) {
    Serial.println("BMP180 not found");
    bmpInitialized = false;
  } else {
    bmpInitialized = true;
  }
}

bool initSDCard() {
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD Card Mount Failed");
    return false;
  }
  
  sdCardAvailable = true;
  loadConfig();
  return true;
}

void loadConfig() {
  if (!SD.exists("/config.txt")) return;

  File configFile = SD.open("/config.txt", FILE_READ);
  if (!configFile) return;

  // Simple config parsing (minimal error handling)
  String ssid = configFile.readStringUntil('\n');
  String password = configFile.readStringUntil('\n');
  String deviceName = configFile.readStringUntil('\n');
  float pressureThreshold = configFile.parseFloat();

  ssid.trim();
  password.trim();
  deviceName.trim();

  if (ssid.length() > 0) strncpy(currentConfig.ssid, ssid.c_str(), sizeof(currentConfig.ssid));
  if (password.length() > 0) strncpy(currentConfig.password, password.c_str(), sizeof(currentConfig.password));
  if (deviceName.length() > 0) strncpy(currentConfig.deviceName, deviceName.c_str(), sizeof(currentConfig.deviceName));
  if (pressureThreshold > 0) currentConfig.pressureThreshold = pressureThreshold;

  configFile.close();
}

void saveConfig() {
  File configFile = SD.open("/config.txt", FILE_WRITE);
  if (!configFile) return;

  configFile.println(currentConfig.ssid);
  configFile.println(currentConfig.password);
  configFile.println(currentConfig.deviceName);
  configFile.println(currentConfig.pressureThreshold);

  configFile.close();
}

void setupWiFi() {
  WiFi.softAP(currentConfig.ssid, currentConfig.password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='utf-8'>";
  html += "<title>Aspol Tracker</title>";
  html += "<style>body{font-family:Arial;max-width:600px;margin:auto;padding:20px;}";
  html += "table{width:100%;border-collapse:collapse;}";
  html += "th,td{border:1px solid #ddd;padding:8px;}";
  html += "input{width:100%;padding:5px;margin:5px 0;}</style>";
  html += "</head><body><h1>Aspol Tracker Status</h1>";

  html += "<h2>Device Status</h2><table>";
  
  // Time
  if (rtcInitialized) {
    DateTime now = rtc.now();
    html += "<tr><th>Time</th><td>" + 
            String(now.year()) + "-" + 
            String(now.month()) + "-" + 
            String(now.day()) + " " + 
            String(now.hour()) + ":" + 
            String(now.minute()) + ":" + 
            String(now.second()) + "</td></tr>";
  } else {
    html += "<tr><th>Time</th><td>RTC Not Initialized</td></tr>";
  }

  // Temperature and Pressure
  if (bmpInitialized) {
    html += "<tr><th>Temperature</th><td>" + String(bmp.readTemperature()) + " °C</td></tr>";
    html += "<tr><th>Air Pressure</th><td>" + String(bmp.readPressure() / 100.0) + " hPa</td></tr>";
  } else {
    html += "<tr><th>Temperature</th><td>BMP Not Initialized</td></tr>";
    html += "<tr><th>Air Pressure</th><td>BMP Not Initialized</td></tr>";
  }

  // GPS Status
  html += "<tr><th>GPS Status</th><td>";
  if (gps.location.isValid()) {
    html += "Lat: " + String(gps.location.lat(), 6) + ", ";
    html += "Lng: " + String(gps.location.lng(), 6) + ", ";
    html += "Speed: " + String(gps.speed.kmph()) + " km/h, ";
    html += "Satellites: " + String(gps.satellites.value());
  } else {
    html += "No Valid GPS Data";
  }
  html += "</td></tr>";
  html += "</table>";

  // Configuration Section
  html += "<h2>Device Configuration</h2>";
  html += "<form method='POST' action='/config'>";
  html += "<table>";
  html += "<tr><th>SSID</th><td><input type='text' name='ssid' value='" + String(currentConfig.ssid) + "'></td></tr>";
  html += "<tr><th>Password</th><td><input type='password' name='password' placeholder='Enter new password'></td></tr>";
  html += "<tr><th>Device Name</th><td><input type='text' name='deviceName' value='" + String(currentConfig.deviceName) + "'></td></tr>";
  html += "<tr><th>Pressure Threshold</th><td><input type='number' step='0.01' name='pressureThreshold' value='" + String(currentConfig.pressureThreshold) + "'></td></tr>";
  html += "<tr><td colspan='2'><input type='submit' value='Save Configuration'></td></tr>";
  html += "</table></form></body></html>";

  server.send(200, "text/html", html);
}

void handleConfig() {
  if (server.method() == HTTP_POST) {
    strncpy(currentConfig.ssid, server.arg("ssid").c_str(), sizeof(currentConfig.ssid));
    
    if (server.arg("password") != "") {
      strncpy(currentConfig.password, server.arg("password").c_str(), sizeof(currentConfig.password));
    }
    
    strncpy(currentConfig.deviceName, server.arg("deviceName").c_str(), sizeof(currentConfig.deviceName));
    currentConfig.pressureThreshold = server.arg("pressureThreshold").toFloat();
    
    saveConfig();
    setupWiFi(); // Restart AP with new config
    
    server.sendHeader("Location", "/");
    server.send(303);
  } else {
    server.sendHeader("Location", "/");
    server.send(303);
  }
}

void logGPSData(float pressure) {
  if (!sdCardAvailable || !gps.location.isValid()) return;

  DateTime now = rtc.now();
  
  File logFile = SD.open("/gps_log.txt", FILE_APPEND);
  if (logFile) {
    logFile.printf("%02d/%02d/%04d %02d:%02d:%02d,%.6f,%.6f,%.2f\n", 
                   now.day(), now.month(), now.year(), 
                   now.hour(), now.minute(), now.second(),
                   gps.location.lat(), gps.location.lng(), 
                   pressure);
    logFile.close();
  }
}

void processGPS() {
  while (neo6m.available() > 0) {
    gps.encode(neo6m.read());
  }
}

void checkPressureAndLog() {
  if (!bmpInitialized) return;
  
  float rawPressure = bmp.readPressure();
  if (rawPressure <= 0) return;

  float pressure = rawPressure / 100.0;
  if (pressure > currentConfig.pressureThreshold) {
    logGPSData(pressure);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  neo6m.begin(9600, SERIAL_8N1, RXD2, TXD2);
  
  initRTC();
  initBMP();
  initSDCard();
  
  setupWiFi();
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.begin();
}

void loop() {
  server.handleClient();
  processGPS();
  checkPressureAndLog();
  delay(100);
}