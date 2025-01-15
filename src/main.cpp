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
#define SERIAL_BUFFER_SIZE 100 // Reduced buffer size

// Add circular buffer for pressure readings
#define PRESSURE_HISTORY_SIZE 10
struct PressureHistory
{
    float readings[PRESSURE_HISTORY_SIZE];
    int index = 0;
    int count = 0;
};

PressureHistory pressureHistory;

// Global Objects
WebServer server(80);
RTC_DS3231 rtc;
Adafruit_BMP085 bmp;
TinyGPSPlus gps;
HardwareSerial neo6m(2);
void checkPressureAndLog();
void handlePressure();
void handleTimeTemp();

// Optimized circular buffer for serial messages
struct LogMessage
{
    unsigned long timestamp;
    char message[80]; // Fixed size message buffer
};

LogMessage serialBuffer[SERIAL_BUFFER_SIZE];
int serialBufferIndex = 0;
int totalMessages = 0;

// Update Config structure to use percentage threshold
struct Config
{
    char ssid[32] = "Aspol Tracker";
    char password[32] = "sulungresearch";
    char deviceName[32] = "PressureTracker";
    float pressureThreshold = 20.0; // Default 20% above average
};

Config currentConfig;
bool sdCardAvailable = false;
bool rtcInitialized = false;
bool bmpInitialized = false;

// Function to add new pressure reading to history
void addPressureReading(float pressure)
{
    pressureHistory.readings[pressureHistory.index] = pressure;
    pressureHistory.index = (pressureHistory.index + 1) % PRESSURE_HISTORY_SIZE;
    if (pressureHistory.count < PRESSURE_HISTORY_SIZE)
    {
        pressureHistory.count++;
    }
}

// Function to calculate average pressure
float calculateAveragePressure()
{
    if (pressureHistory.count == 0)
        return 0;

    float sum = 0;
    for (int i = 0; i < pressureHistory.count; i++)
    {
        sum += pressureHistory.readings[i];
    }
    return sum / pressureHistory.count;
}

void loadConfig(); // Forward declaration

// Serial logging function
void serialPrintln(const char *message)
{
    Serial.println(message);

    // Store in buffer
    serialBuffer[serialBufferIndex].timestamp = millis();
    strncpy(serialBuffer[serialBufferIndex].message, message, sizeof(serialBuffer[0].message) - 1);
    serialBuffer[serialBufferIndex].message[sizeof(serialBuffer[0].message) - 1] = '\0';

    serialBufferIndex = (serialBufferIndex + 1) % SERIAL_BUFFER_SIZE;
    if (totalMessages < SERIAL_BUFFER_SIZE)
        totalMessages++;
}

// Initialization functions
void initRTC()
{
    Wire.begin();
    if (!rtc.begin())
    {
        serialPrintln("RTC not found");
        rtcInitialized = false;
    }
    else
    {
        rtcInitialized = true;
        if (rtc.lostPower())
        {
            serialPrintln("RTC lost power, setting time!");
            rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        }
        serialPrintln("RTC initialized successfully");
    }
}

void initBMP()
{
    if (!bmp.begin())
    {
        serialPrintln("BMP180 not found");
        bmpInitialized = false;
    }
    else
    {
        bmpInitialized = true;
        serialPrintln("BMP180 initialized successfully");
    }
}

bool initSDCard()
{
    if (!SD.begin(SD_CS_PIN))
    {
        serialPrintln("SD Card Mount Failed");
        return false;
    }

    sdCardAvailable = true;
    loadConfig();
    serialPrintln("SD Card initialized successfully");
    return true;
}

// Configuration functions
void loadConfig()
{
    if (!SD.exists("/config.txt"))
        return;

    File configFile = SD.open("/config.txt", FILE_READ);
    if (!configFile)
        return;

    String ssid = configFile.readStringUntil('\n');
    String password = configFile.readStringUntil('\n');
    String deviceName = configFile.readStringUntil('\n');
    float pressureThreshold = configFile.parseFloat();

    ssid.trim();
    password.trim();
    deviceName.trim();

    if (ssid.length() > 0)
        strncpy(currentConfig.ssid, ssid.c_str(), sizeof(currentConfig.ssid) - 1);
    if (password.length() > 0)
        strncpy(currentConfig.password, password.c_str(), sizeof(currentConfig.password) - 1);
    if (deviceName.length() > 0)
        strncpy(currentConfig.deviceName, deviceName.c_str(), sizeof(currentConfig.deviceName) - 1);
    if (pressureThreshold > 0)
        currentConfig.pressureThreshold = pressureThreshold;

    configFile.close();
}

void saveConfig()
{
    File configFile = SD.open("/config.txt", FILE_WRITE);
    if (!configFile)
        return;

    configFile.println(currentConfig.ssid);
    configFile.println(currentConfig.password);
    configFile.println(currentConfig.deviceName);
    configFile.println(currentConfig.pressureThreshold);

    configFile.close();
}

void setupWiFi()
{
    WiFi.softAP(currentConfig.ssid, currentConfig.password);
    IPAddress IP = WiFi.softAPIP();
    char ipMsg[50];
    snprintf(ipMsg, sizeof(ipMsg), "AP IP address: %d.%d.%d.%d", IP[0], IP[1], IP[2], IP[3]);
    serialPrintln(ipMsg);
}

// Web handlers
void handleSerial()
{
    String logs;
    logs.reserve(SERIAL_BUFFER_SIZE * 100); // Pre-allocate space

    int start = (serialBufferIndex - totalMessages + SERIAL_BUFFER_SIZE) % SERIAL_BUFFER_SIZE;
    for (int i = 0; i < totalMessages; i++)
    {
        int index = (start + i) % SERIAL_BUFFER_SIZE;
        logs += String(serialBuffer[index].timestamp);
        logs += ": ";
        logs += serialBuffer[index].message;
        logs += "\n";
    }

    server.send(200, "text/plain", logs);
}

String getFileList()
{
    if (!sdCardAvailable)
        return "<p>SD Card not available</p>";

    String html;
    html.reserve(1024); // Pre-allocate space
    html = "<h2>SD Card Files</h2><table>";
    html += "<tr><th>File Name</th><th>Size</th><th>Actions</th></tr>";

    File root = SD.open("/");
    while (File file = root.openNextFile())
    {
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

void handleDownload()
{
    String fileName = server.arg("file");
    if (!SD.exists("/" + fileName))
    {
        server.send(404, "text/plain", "File not found");
        return;
    }

    File file = SD.open("/" + fileName, FILE_READ);
    if (!file)
    {
        server.send(500, "text/plain", "Failed to open file");
        return;
    }

    server.sendHeader("Content-Type", "application/octet-stream");
    server.sendHeader("Content-Disposition", "attachment; filename=" + fileName);
    server.sendHeader("Connection", "close");

    uint8_t buf[1024];
    size_t bytesRead;
    while ((bytesRead = file.read(buf, sizeof(buf))) > 0)
    {
        server.sendContent((char *)buf, bytesRead);
    }

    file.close();
}

void handleDelete()
{
    String fileName = server.arg("file");
    if (SD.remove("/" + fileName))
    {
        server.sendHeader("Location", "/");
        server.send(303);
    }
    else
    {
        server.send(500, "text/plain", "Failed to delete file");
    }
}

void handleConfig()
{
    if (server.method() == HTTP_POST)
    {
        strncpy(currentConfig.ssid, server.arg("ssid").c_str(), sizeof(currentConfig.ssid) - 1);

        if (server.arg("password").length() > 0)
        {
            strncpy(currentConfig.password, server.arg("password").c_str(), sizeof(currentConfig.password) - 1);
        }

        strncpy(currentConfig.deviceName, server.arg("deviceName").c_str(), sizeof(currentConfig.deviceName) - 1);
        currentConfig.pressureThreshold = server.arg("pressureThreshold").toFloat();

        saveConfig();
        setupWiFi(); // Restart AP with new config

        server.sendHeader("Location", "/");
        server.send(303);
    }
    else
    {
        server.sendHeader("Location", "/");
        server.send(303);
    }
}

void handleDateTime()
{
    if (server.method() == HTTP_POST && rtcInitialized)
    {
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

void handleRoot()
{
    String html;
    html.reserve(4096);

    // HTML Start and Head
    html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='utf-8'>";
    html += "<title>Aspol Tracker</title>";

    // CSS Styles
    html += "<style>";
    html += "body {";
    html += "    font-family: Arial, sans-serif;";
    html += "    max-width: 800px;";
    html += "    margin: auto;";
    html += "    padding: 20px;";
    html += "    background-color: #f5f5f5;";
    html += "}";
    html += "h1, h2 {";
    html += "    color: #333;";
    html += "    border-bottom: 2px solid #ddd;";
    html += "    padding-bottom: 10px;";
    html += "}";
    html += "table {";
    html += "    width: 100%;";
    html += "    border-collapse: collapse;";
    html += "    margin-bottom: 20px;";
    html += "    background-color: white;";
    html += "    box-shadow: 0 1px 3px rgba(0,0,0,0.1);";
    html += "}";
    html += "th, td {";
    html += "    border: 1px solid #ddd;";
    html += "    padding: 12px;";
    html += "    text-align: left;";
    html += "}";
    html += "th { background-color: #f8f9fa; font-weight: bold; }";
    html += "input {";
    html += "    width: 100%;";
    html += "    padding: 8px;";
    html += "    margin: 5px 0;";
    html += "    border: 1px solid #ddd;";
    html += "    border-radius: 4px;";
    html += "    box-sizing: border-box;";
    html += "}";
    html += "input[type='submit'] {";
    html += "    background-color: #007bff;";
    html += "    color: white;";
    html += "    border: none;";
    html += "    padding: 10px;";
    html += "    cursor: pointer;";
    html += "    font-weight: bold;";
    html += "}";
    html += "input[type='submit']:hover { background-color: #0056b3; }";
    html += "#serialMonitor {";
    html += "    background: #f8f8f8;";
    html += "    padding: 15px;";
    html += "    font-family: monospace;";
    html += "    height: 200px;";
    html += "    overflow-y: auto;";
    html += "    margin: 10px 0;";
    html += "    white-space: pre;";
    html += "    border: 1px solid #ddd;";
    html += "    border-radius: 4px;";
    html += "}";
    html += "#pressureData {";
    html += "    background: #f0f8ff;";
    html += "    padding: 20px;";
    html += "    margin: 20px 0;";
    html += "    border-radius: 8px;";
    html += "    box-shadow: 0 2px 4px rgba(0,0,0,0.1);";
    html += "}";
    html += ".status-card {";
    html += "    background: white;";
    html += "    padding: 15px;";
    html += "    margin: 10px 0;";
    html += "    border-radius: 8px;";
    html += "    box-shadow: 0 2px 4px rgba(0,0,0,0.1);";
    html += "}";
    html += "</style>";

    // JavaScript
    html += "<script>";
    html += "function updateData() {";
    html += "    fetch('/pressure')";
    html += "        .then(response => response.json())";
    html += "        .then(data => {";
    html += "            document.getElementById('currentPressure').textContent = data.current.toFixed(2);";
    html += "            document.getElementById('avgPressure').textContent = data.average.toFixed(2);";
    html += "            document.getElementById('threshold').textContent = data.threshold.toFixed(2);";
    html += "            const pressureElement = document.getElementById('currentPressure').parentElement;";
    html += "            if (data.current > data.threshold) {";
    html += "                pressureElement.style.backgroundColor = '#ffebee';";
    html += "            } else {";
    html += "                pressureElement.style.backgroundColor = '';";
    html += "            }";
    html += "        });";
    html += "    fetch('/serial')";
    html += "        .then(response => response.text())";
    html += "        .then(data => {";
    html += "            const monitor = document.getElementById('serialMonitor');";
    html += "            monitor.textContent = data;";
    html += "            monitor.scrollTop = monitor.scrollHeight;";
    html += "        });";
    html += "}";
    html += "setInterval(updateData, 1000);";
    html += "document.addEventListener('DOMContentLoaded', updateData);";
    html += "function updateTimeAndTemp() {";
    html += "    fetch('/timeTemp')"; // Endpoint for time and temperature
    html += "        .then(response => response.json())";
    html += "        .then(data => {";
    html += "            document.getElementById('time').textContent = data.time;";
    html += "            document.getElementById('temperature').textContent = data.temperature.toFixed(2) + ' °C';";
    html += "        });";
    html += "}";
    html += "setInterval(updateTimeAndTemp, 1000);";                             // Update every 1 second
    html += "document.addEventListener('DOMContentLoaded', updateTimeAndTemp);"; // Initial call
    html += "</script>";
    html += "</head>";

    // Body Content
    html += "<body>";
    html += "<h1>Aspol Tracker Status</h1>";

    // Pressure Monitoring Section
    html += "<div id='pressureData'>";
    html += "<h2>Pressure Monitoring</h2>";
    html += "<table>";
    html += "<tr><th>Current Pressure</th><td><span id='currentPressure'>0.00</span> hPa</td></tr>";
    html += "<tr><th>Average Pressure (10 readings)</th><td><span id='avgPressure'>0.00</span> hPa</td></tr>";
    html += "<tr><th>Threshold Level</th><td><span id='threshold'>0.00</span> hPa</td></tr>";
    html += "</table>";
    html += "</div>";

    // Device Status Section
    html += "<div class='status-card'>";
    html += "<h2>Device Status</h2><table>";

    // Time Status
    if (rtcInitialized)
    {
        DateTime now = rtc.now();
        char timeStr[30];
        snprintf(timeStr, sizeof(timeStr), "%04d-%02d-%02d %02d:%02d:%02d",
                 now.year(), now.month(), now.day(),
                 now.hour(), now.minute(), now.second());
        html += "<tr><th>Current Time</th><td><span id='time'>Loading...</span></div></td></tr>";
    }
    else
    {
        html += "<tr><th>Time</th><td>RTC Not Initialized</td></tr>";
    }

    // Temperature Status
    if (bmpInitialized)
    {
        html += "<tr><th>Temperature</th><td><span id='temperature'>Loading...</span></div></td></tr>";
    }
    else
    {
        html += "<tr><th>Temperature</th><td>BMP Not Initialized</td></tr>";
    }

    // GPS Status
    html += "<tr><th>GPS Status</th><td>";
    if (gps.location.isValid())
    {
        char gpsStr[100];
        snprintf(gpsStr, sizeof(gpsStr), "Lat: %.6f, Lng: %.6f, Speed: %.1f km/h, Satellites: %d",
                 gps.location.lat(), gps.location.lng(),
                 gps.speed.kmph(), gps.satellites.value());
        html += gpsStr;
    }
    else
    {
        html += "No Valid GPS Data";
    }
    html += "</td></tr></table></div>";

    // Serial Monitor Section
    html += "<div class='status-card'>";
    html += "<h2>Serial Monitor</h2>";
    html += "<div id='serialMonitor'></div>";
    html += "</div>";

    // SD Card Files Section
    html += "<div class='status-card'>";
    html += getFileList();
    html += "</div>";

    // RTC Configuration Section
    html += "<div class='status-card'>";
    html += "<h2>RTC Configuration</h2>";
    html += "<form method='POST' action='/datetime'>";
    html += "<table><tr><th>Set Date & Time</th><td>";
    html += "<input type='datetime-local' name='datetime' required></td></tr>";
    html += "<tr><td colspan='2'><input type='submit' value='Update DateTime'></td></tr></table>";
    html += "</form></div>";

    // Device Configuration Section
    html += "<div class='status-card'>";
    html += "<h2>Device Configuration</h2>";
    html += "<form method='POST' action='/config'><table>";
    html += "<tr><th>SSID</th><td><input type='text' name='ssid' value='" + String(currentConfig.ssid) + "'></td></tr>";
    html += "<tr><th>Password</th><td><input type='password' name='password' placeholder='Enter new password'></td></tr>";
    html += "<tr><th>Device Name</th><td><input type='text' name='deviceName' value='" + String(currentConfig.deviceName) + "'></td></tr>";
    html += "<tr><th>Pressure Threshold (%)</th><td><input type='number' step='0.1' name='pressureThreshold' value='" +
            String(currentConfig.pressureThreshold) + "'></td></tr>";
    html += "<tr><td colspan='2'><input type='submit' value='Save Configuration'></td></tr></table>";
    html += "</form></div>";

    html += "</body></html>";

    server.send(200, "text/html", html);
}

void logGPSData(float pressure)
{
    if (!sdCardAvailable || !gps.location.isValid())
        return;

    DateTime now = rtc.now();

    File logFile = SD.open("/gps_log.txt", FILE_APPEND);
    if (logFile)
    {
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
    }
    else
    {
        serialPrintln("Failed to open log file");
    }
}

void processGPS()
{
    while (neo6m.available() > 0)
    {
        if (gps.encode(neo6m.read()))
        {
            if (gps.location.isUpdated())
            {
                char message[80];
                snprintf(message, sizeof(message), "GPS Updated - Satellites: %d", gps.satellites.value());
                serialPrintln(message);
            }
        }
    }

    if (millis() > 5000 && gps.charsProcessed() < 10)
    {
        serialPrintln("No GPS detected");
    }
}

void setup()
{
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
    server.on("/pressure", handlePressure); // Add new endpoint
    server.on("/download", handleDownload);
    server.on("/delete", handleDelete);
    server.on("/timeTemp", handleTimeTemp);
    server.begin();
    serialPrintln("Web server started");
}

// Add new handler for real-time pressure data
void handlePressure()
{
    float currentPressure = bmpInitialized ? (bmp.readPressure() / 100.0) : 0;
    float averagePressure = calculateAveragePressure();
    float thresholdLevel = averagePressure * (1 + currentConfig.pressureThreshold / 100.0);

    String json = "{";
    json += "\"current\":" + String(currentPressure, 2) + ",";
    json += "\"average\":" + String(averagePressure, 2) + ",";
    json += "\"threshold\":" + String(thresholdLevel, 2);
    json += "}";

    server.send(200, "application/json", json);
}

void checkPressureAndLog()
{
    if (!bmpInitialized)
        return;

    float rawPressure = bmp.readPressure();
    if (rawPressure <= 0)
    {
        serialPrintln("Invalid pressure reading");
        return;
    }

    float pressure = rawPressure / 100.0;
    addPressureReading(pressure);

    float averagePressure = calculateAveragePressure();
    float thresholdLevel = averagePressure * (1 + currentConfig.pressureThreshold / 100.0);

    if (pressure > thresholdLevel)
    {
        char message[100];
        snprintf(message, sizeof(message),
                 "Pressure threshold exceeded: %.2f hPa (Avg: %.2f, Threshold: %.2f)",
                 pressure, averagePressure, thresholdLevel);
        serialPrintln(message);
        logGPSData(pressure);
    }
}

void handleTimeTemp()
{
    if (rtcInitialized && bmpInitialized)
    {
        DateTime now = rtc.now();
        float temperature = bmp.readTemperature();

        String json = "{";
        json += "\"time\":\"" + String(now.timestamp(DateTime::TIMESTAMP_TIME)) + "\",";
        json += "\"temperature\":" + String(temperature, 2);
        json += "}";

        server.send(200, "application/json", json);
    }
    else
    {
        server.send(500, "application/json", "{\"error\":\"RTC or BMP not initialized\"}");
    }
}

void loop()
{
    static unsigned long lastStatusUpdate = 0;
    const unsigned long STATUS_UPDATE_INTERVAL = 10000; // 10 seconds

    server.handleClient();
    processGPS();
    checkPressureAndLog();

    // Periodic status update
    unsigned long currentMillis = millis();
    if (currentMillis - lastStatusUpdate >= STATUS_UPDATE_INTERVAL)
    {
        lastStatusUpdate = currentMillis;

        if (bmpInitialized)
        {
            char statusMsg[80];
            float temperature = bmp.readTemperature();
            float pressure = bmp.readPressure() / 100.0;
            snprintf(statusMsg, sizeof(statusMsg), "Status - Temp: %.1f°C, Pressure: %.1f hPa",
                     temperature, pressure);
            serialPrintln(statusMsg);
        }

        if (gps.location.isValid())
        {
            char gpsMsg[100];
            snprintf(gpsMsg, sizeof(gpsMsg), "GPS - Lat: %.6f, Lng: %.6f, Satellites: %d",
                     gps.location.lat(), gps.location.lng(), gps.satellites.value());
            serialPrintln(gpsMsg);
        }
    }

    delay(100);
}