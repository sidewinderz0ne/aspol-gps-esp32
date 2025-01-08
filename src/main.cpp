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
#define SERIAL_BUFFER_SIZE 100  // Reduced buffer size

// Global Objects
WebServer server(80);
RTC_DS3231 rtc;
Adafruit_BMP085 bmp;
TinyGPSPlus gps;
HardwareSerial neo6m(2);

// Optimized circular buffer for serial messages
struct LogMessage {
    unsigned long timestamp;
    char message[80];  // Fixed size message buffer
};

LogMessage serialBuffer[SERIAL_BUFFER_SIZE];
int serialBufferIndex = 0;
int totalMessages = 0;

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

void loadConfig(); // Forward declaration

// Serial logging function
void serialPrintln(const char* message) {
    Serial.println(message);
    
    // Store in buffer
    serialBuffer[serialBufferIndex].timestamp = millis();
    strncpy(serialBuffer[serialBufferIndex].message, message, sizeof(serialBuffer[0].message) - 1);
    serialBuffer[serialBufferIndex].message[sizeof(serialBuffer[0].message) - 1] = '\0';
    
    serialBufferIndex = (serialBufferIndex + 1) % SERIAL_BUFFER_SIZE;
    if (totalMessages < SERIAL_BUFFER_SIZE) totalMessages++;
}

// Initialization functions
void initRTC() {
    Wire.begin();
    if (!rtc.begin()) {
        serialPrintln("RTC not found");
        rtcInitialized = false;
    } else {
        rtcInitialized = true;
        if (rtc.lostPower()) {
            serialPrintln("RTC lost power, setting time!");
            rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        }
        serialPrintln("RTC initialized successfully");
    }
}

void initBMP() {
    if (!bmp.begin()) {
        serialPrintln("BMP180 not found");
        bmpInitialized = false;
    } else {
        bmpInitialized = true;
        serialPrintln("BMP180 initialized successfully");
    }
}

bool initSDCard() {
    if (!SD.begin(SD_CS_PIN)) {
        serialPrintln("SD Card Mount Failed");
        return false;
    }
    
    sdCardAvailable = true;
    loadConfig();
    serialPrintln("SD Card initialized successfully");
    return true;
}

// Configuration functions
void loadConfig() {
    if (!SD.exists("/config.txt")) return;

    File configFile = SD.open("/config.txt", FILE_READ);
    if (!configFile) return;

    String ssid = configFile.readStringUntil('\n');
    String password = configFile.readStringUntil('\n');
    String deviceName = configFile.readStringUntil('\n');
    float pressureThreshold = configFile.parseFloat();

    ssid.trim();
    password.trim();
    deviceName.trim();

    if (ssid.length() > 0) strncpy(currentConfig.ssid, ssid.c_str(), sizeof(currentConfig.ssid) - 1);
    if (password.length() > 0) strncpy(currentConfig.password, password.c_str(), sizeof(currentConfig.password) - 1);
    if (deviceName.length() > 0) strncpy(currentConfig.deviceName, deviceName.c_str(), sizeof(currentConfig.deviceName) - 1);
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
    char ipMsg[50];
    snprintf(ipMsg, sizeof(ipMsg), "AP IP address: %d.%d.%d.%d", IP[0], IP[1], IP[2], IP[3]);
    serialPrintln(ipMsg);
}

// Web handlers
void handleSerial() {
    String logs;
    logs.reserve(SERIAL_BUFFER_SIZE * 100); // Pre-allocate space
    
    int start = (serialBufferIndex - totalMessages + SERIAL_BUFFER_SIZE) % SERIAL_BUFFER_SIZE;
    for (int i = 0; i < totalMessages; i++) {
        int index = (start + i) % SERIAL_BUFFER_SIZE;
        logs += String(serialBuffer[index].timestamp);
        logs += ": ";
        logs += serialBuffer[index].message;
        logs += "\n";
    }
    
    server.send(200, "text/plain", logs);
}

String getFileList() {
    if (!sdCardAvailable) return "<p>SD Card not available</p>";
    
    String html;
    html.reserve(1024); // Pre-allocate space
    html = "<h2>SD Card Files</h2><table>";
    html += "<tr><th>File Name</th><th>Size</th><th>Actions</th></tr>";

    File root = SD.open("/");
    while (File file = root.openNextFile()) {
        String fileName = String(file.name());
        html += "<tr><td>" + fileName + "</td>";
        html += "<td>" + String(file.size()) + " bytes</td>";
        html += "<td>";
        html += "<a href='/download?file=" + fileName + "'>Download</a> | ";
        html += "<a href='/delete?file=" + fileName + "' onclick='return confirm(\"Delete " + fileName + "?\")'>Delete</a>";
        html += "</td></tr>";
        file.close();
    }
    root.close();
    
    html += "</table>";
    return html;
}

void handleDownload() {
    String fileName = server.arg("file");
    if (!SD.exists("/" + fileName)) {
        server.send(404, "text/plain", "File not found");
        return;
    }

    File file = SD.open("/" + fileName, FILE_READ);
    if (!file) {
        server.send(500, "text/plain", "Failed to open file");
        return;
    }

    server.sendHeader("Content-Type", "application/octet-stream");
    server.sendHeader("Content-Disposition", "attachment; filename=" + fileName);
    server.sendHeader("Connection", "close");
    
    uint8_t buf[1024];
    size_t bytesRead;
    while ((bytesRead = file.read(buf, sizeof(buf))) > 0) {
        server.sendContent((char*)buf, bytesRead);
    }
    
    file.close();
}

void handleDelete() {
    String fileName = server.arg("file");
    if (SD.remove("/" + fileName)) {
        server.sendHeader("Location", "/");
        server.send(303);
    } else {
        server.send(500, "text/plain", "Failed to delete file");
    }
}

void handleConfig() {
    if (server.method() == HTTP_POST) {
        strncpy(currentConfig.ssid, server.arg("ssid").c_str(), sizeof(currentConfig.ssid) - 1);
        
        if (server.arg("password").length() > 0) {
            strncpy(currentConfig.password, server.arg("password").c_str(), sizeof(currentConfig.password) - 1);
        }
        
        strncpy(currentConfig.deviceName, server.arg("deviceName").c_str(), sizeof(currentConfig.deviceName) - 1);
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

void handleDateTime() {
    if (server.method() == HTTP_POST && rtcInitialized) {
        String dateTimeStr = server.arg("datetime");
        
        int year = dateTimeStr.substring(0, 4).toInt();
        int month = dateTimeStr.substring(5, 7).toInt();
        int day = dateTimeStr.substring(8, 10).toInt();
        int hour = dateTimeStr.substring(11, 13).toInt();
        int minute = dateTimeStr.substring(14, 16).toInt();
        
        DateTime newDateTime(year, month, day, hour, minute, 0);
        rtc.adjust(newDateTime);
        
        char timeMsg[50];
        snprintf(timeMsg, sizeof(timeMsg), "Time updated to: %04d-%02d-%02d %02d:%02d:00", 
                year, month, day, hour, minute);
        serialPrintln(timeMsg);
    }
    
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleRoot() {
    String html;
    html.reserve(4096); // Pre-allocate space
    
    html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='utf-8'>";
    html += "<title>Aspol Tracker</title>";
    html += "<style>";
    html += "body{font-family:Arial;max-width:800px;margin:auto;padding:20px;}";
    html += "table{width:100%;border-collapse:collapse;margin-bottom:20px;}";
    html += "th,td{border:1px solid #ddd;padding:8px;text-align:left;}";
    html += "input{width:100%;padding:5px;margin:5px 0;}";
    html += "#serialMonitor{background:#f8f8f8;padding:10px;font-family:monospace;height:200px;overflow-y:auto;margin:10px 0;white-space:pre;}";
    html += "</style>";
    
    // Optimized JavaScript
    html += "<script>";
    html += "function updateSerial(){fetch('/serial').then(r=>r.text()).then(d=>{const m=document.getElementById('serialMonitor');m.textContent=d;m.scrollTop=m.scrollHeight})}";
    html += "setInterval(updateSerial,2000);";
    html += "</script>";
    html += "</head><body><h1>Aspol Tracker Status</h1>";

    // Status Table
    html += "<h2>Device Status</h2><table>";
    
    // Time status
    if (rtcInitialized) {
        DateTime now = rtc.now();
        char timeStr[30];
        snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d", 
                now.year(), now.month(), now.day(), 
                now.hour(), now.minute(), now.second());
        html += "<tr><th>Current Time</th><td>" + String(timeStr) + "</td></tr>";
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
        char gpsStr[100];
        snprintf(gpsStr, sizeof(gpsStr), "Lat: %.6f, Lng: %.6f, Speed: %.1f km/h, Satellites: %d",
                gps.location.lat(), gps.location.lng(), 
                gps.speed.kmph(), gps.satellites.value());
        html += gpsStr;
    } else {
        html += "No Valid GPS Data";
    }
    html += "</td></tr></table>";

    // Serial Monitor
    html += "<h2>Serial Monitor</h2><div id='serialMonitor'></div>";

    // SD Card Files
    html += getFileList();

    // RTC Configuration
    html += "<h2>RTC Configuration</h2>";
    html += "<form method='POST' action='/datetime'>";
    html += "<table><tr><th>Set Date & Time</th><td>";
    html += "<input type='datetime-local' name='datetime' required></td></tr>";
    html += "<tr><td colspan='2'><input type='submit' value='Update DateTime'></td></tr></table></form>";

    // Device Configuration
    html += "<h2>Device Configuration</h2>";
    html += "<form method='POST' action='/config'><table>";
    html += "<tr><th>SSID</th><td><input type='text' name='ssid' value='" + String(currentConfig.ssid) + "'></td></tr>";
    html += "<tr><th>Password</th><td><input type='password' name='password' placeholder='Enter new password'></td></tr>";
    html += "<tr><th>Device Name</th><td><input type='text' name='deviceName' value='" + String(currentConfig.deviceName) + "'></td></tr>";
    html += "<tr><th>Pressure Threshold</th><td><input type='number' step='0.01' name='pressureThreshold' value='" + String(currentConfig.pressureThreshold) + "'></td></tr>";
    html += "<tr><td colspan='2'><input type='submit' value='Save Configuration'></td></tr></table></form>";
    
    html += "</body></html>";
    
    server.send(200, "text/html", html);
}

void logGPSData(float pressure) {
    if (!sdCardAvailable || !gps.location.isValid()) return;

    DateTime now = rtc.now();
    
    File logFile = SD.open("/gps_log.txt", FILE_APPEND);
    if (logFile) {
        char logEntry[100];
        snprintf(logEntry, sizeof(logEntry), "%02d/%02d/%04d %02d:%02d:%02d,%.6f,%.6f,%.2f\n",
                now.day(), now.month(), now.year(),
                now.hour(), now.minute(), now.second(),
                gps.location.lat(), gps.location.lng(),
                pressure);
        logFile.print(logEntry);
        logFile.close();
        
        char message[50];
        snprintf(message, sizeof(message), "Logged data: %.2f hPa", pressure);
        serialPrintln(message);
    } else {
        serialPrintln("Failed to open log file");
    }
}

void processGPS() {
    while (neo6m.available() > 0) {
        if (gps.encode(neo6m.read())) {
            if (gps.location.isUpdated()) {
                char message[80];
                snprintf(message, sizeof(message), "GPS Updated - Satellites: %d", gps.satellites.value());
                serialPrintln(message);
            }
        }
    }

    if (millis() > 5000 && gps.charsProcessed() < 10) {
        serialPrintln("No GPS detected");
    }
}

void checkPressureAndLog() {
    if (!bmpInitialized) return;
    
    float rawPressure = bmp.readPressure();
    if (rawPressure <= 0) {
        serialPrintln("Invalid pressure reading");
        return;
    }

    float pressure = rawPressure / 100.0;
    if (pressure > currentConfig.pressureThreshold) {
        char message[50];
        snprintf(message, sizeof(message), "Pressure threshold exceeded: %.2f hPa", pressure);
        serialPrintln(message);
        logGPSData(pressure);
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    serialPrintln("System starting...");
    
    neo6m.begin(9600, SERIAL_8N1, RXD2, TXD2);
    serialPrintln("GPS serial initialized");
    
    initRTC();
    initBMP();
    initSDCard();
    
    setupWiFi();
    server.on("/", handleRoot);
    server.on("/config", handleConfig);
    server.on("/datetime", handleDateTime);
    server.on("/serial", handleSerial);
    server.on("/download", handleDownload);
    server.on("/delete", handleDelete);
    server.begin();
    serialPrintln("Web server started");
}

void loop() {
    static unsigned long lastStatusUpdate = 0;
    const unsigned long STATUS_UPDATE_INTERVAL = 10000; // 10 seconds
    
    server.handleClient();
    processGPS();
    checkPressureAndLog();
    
    // Periodic status update
    unsigned long currentMillis = millis();
    if (currentMillis - lastStatusUpdate >= STATUS_UPDATE_INTERVAL) {
        lastStatusUpdate = currentMillis;
        
        if (bmpInitialized) {
            char statusMsg[80];
            float temperature = bmp.readTemperature();
            float pressure = bmp.readPressure() / 100.0;
            snprintf(statusMsg, sizeof(statusMsg), "Status - Temp: %.1f°C, Pressure: %.1f hPa", 
                    temperature, pressure);
            serialPrintln(statusMsg);
        }
        
        if (gps.location.isValid()) {
            char gpsMsg[100];
            snprintf(gpsMsg, sizeof(gpsMsg), "GPS - Lat: %.6f, Lng: %.6f, Satellites: %d", 
                    gps.location.lat(), gps.location.lng(), gps.satellites.value());
            serialPrintln(gpsMsg);
        }
    }
    
    delay(100);
}