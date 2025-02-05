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

#define FLOW_SENSOR_PIN 15
volatile uint16_t pulseCount = 0;
float flowRate = 0.0;
const float calibrationFactor = 7.5; // Adjust based on sensor specs

int lastLogTime = 3000;

struct FlowHistory
{
    float readings[PRESSURE_HISTORY_SIZE];
    int index = 0;
    int count = 0;
};
FlowHistory flowHistory;
void logGPSDataFlow(float flow);

// Global Objects
WebServer server(80);
RTC_DS3231 rtc;
Adafruit_BMP085 bmp;
TinyGPSPlus gps;
HardwareSerial neo6m(2);
void checkPressureAndLog();
void handlePressure();
void handleTimeTemp();
void saveConfig();

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
struct Config {
    char ssid[32] = "Aspol Tracker";
    char password[32] = "sulungresearch";
    char deviceName[32] = "PressureTracker";
    char currentSensor[10] = "BMP";  // Default to BMP
    float pressureThreshold = 0.2;  // Default 0.2%
    float flowThreshold = 20.0;      // Default 20%
};

void IRAM_ATTR pulseCounter()
{
    pulseCount++;
}

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

void initBMP() {
    int retryCount = 0;
    while (!bmp.begin() && retryCount < 5) {
        serialPrintln("BMP180 not found, retrying...");
        delay(500);
        retryCount++;
    }
    if (!bmp.begin()) {
        serialPrintln("BMP180 initialization failed");
        bmpInitialized = false;
    } else {
        bmpInitialized = true;
        serialPrintln("BMP180 initialized successfully");
    }
}

void initSDCard() {
    if (!SD.begin(SD_CS_PIN)) {
        serialPrintln("SD Card Mount Failed");
        sdCardAvailable = false;
        return;
    }

    sdCardAvailable = true;
    
    // Create config file if it doesn't exist
    if (!SD.exists("/config.txt")) {
        serialPrintln("Creating new config file");
        saveConfig();  // Save default configuration
    }
    
    loadConfig();
    serialPrintln("SD Card initialized successfully");
}

void loadConfig() {
    if (!SD.exists("/config.txt")) {
        serialPrintln("No config file found, using defaults");
        return;
    }

    File configFile = SD.open("/config.txt", FILE_READ);
    if (!configFile) {
        serialPrintln("Failed to open config file");
        return;
    }

    // Read values with proper error checking
    String ssid = configFile.readStringUntil('\n');
    String password = configFile.readStringUntil('\n');
    String deviceName = configFile.readStringUntil('\n');
    String currentSensor = configFile.readStringUntil('\n');
    float pressureThreshold = configFile.parseFloat();
    float flowThreshold = configFile.parseFloat();

    // Clear any remaining newline characters
    while (configFile.available()) configFile.read();

    // Apply loaded values
    ssid.trim();
    password.trim();
    deviceName.trim();
    currentSensor.trim();

    ssid.toCharArray(currentConfig.ssid, sizeof(currentConfig.ssid));
    password.toCharArray(currentConfig.password, sizeof(currentConfig.password));
    deviceName.toCharArray(currentConfig.deviceName, sizeof(currentConfig.deviceName));
    currentSensor.toCharArray(currentConfig.currentSensor, sizeof(currentConfig.currentSensor));
    
    if (pressureThreshold > 0) currentConfig.pressureThreshold = pressureThreshold;
    if (flowThreshold > 0) currentConfig.flowThreshold = flowThreshold;

    configFile.close();
    serialPrintln("Configuration loaded from SD card");
}

void saveConfig() {
    File configFile = SD.open("/config.txt", FILE_WRITE);
    if (!configFile) {
        serialPrintln("Failed to open config file for writing");
        return;
    }

    configFile.println(currentConfig.ssid);
    configFile.println(currentConfig.password);
    configFile.println(currentConfig.deviceName);
    configFile.println(currentConfig.currentSensor);
    configFile.println(currentConfig.pressureThreshold);
    configFile.println(currentConfig.flowThreshold);
    
    configFile.close();
    serialPrintln("Configuration saved to SD card");
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

void handleConfig() {
    if (server.method() == HTTP_POST) {
        // Handle sensor configuration
        server.arg("sensorType").toCharArray(currentConfig.currentSensor, 
            sizeof(currentConfig.currentSensor));
        
        currentConfig.pressureThreshold = server.arg("pressureThreshold").toFloat();
        currentConfig.flowThreshold = server.arg("flowThreshold").toFloat();
        
        // Handle other parameters
        strncpy(currentConfig.ssid, server.arg("ssid").c_str(), sizeof(currentConfig.ssid));
        if (server.arg("password").length() > 0) {
            strncpy(currentConfig.password, server.arg("password").c_str(), 
                sizeof(currentConfig.password));
        }
        strncpy(currentConfig.deviceName, server.arg("deviceName").c_str(), 
            sizeof(currentConfig.deviceName));

        saveConfig();  // Save to SD card
        setupWiFi();   // Restart AP with new config

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
    html += "input, select {";
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
    html += "#sensorData {";
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
    html += "            document.getElementById('currentValue').textContent = data.current.toFixed(2);";
    html += "            document.getElementById('avgValue').textContent = data.average.toFixed(2);";
    html += "            document.getElementById('threshold').textContent = data.threshold.toFixed(2);";
    html += "            const valueElement = document.getElementById('currentValue').parentElement;";
    html += "            valueElement.style.backgroundColor = data.current > data.threshold ? '#ffebee' : '';";
    html += "        });";
    html += "    fetch('/serial')";
    html += "        .then(response => response.text())";
    html += "        .then(data => {";
    html += "            const monitor = document.getElementById('serialMonitor');";
    html += "            monitor.textContent = data;";
    html += "            monitor.scrollTop = monitor.scrollHeight;";
    html += "        });";
    html += "    fetch('/timeTemp')";
    html += "        .then(response => response.json())";
    html += "        .then(data => {";
    html += "            document.getElementById('time').textContent = data.time;";
    html += "            if(data.temperature) {";
    html += "                document.getElementById('temperature').textContent = data.temperature + ' °C';";
    html += "            }";
    html += "        });";
    html += "}";
    html += "setInterval(updateData, 1000);";
    html += "document.addEventListener('DOMContentLoaded', function() {";
    html += "    updateData();";
    html += "    document.querySelector('[name=\"sensorType\"]').addEventListener('change', function() {";
    html += "        document.getElementById('pressureRow').style.display = (this.value === 'BMP') ? 'table-row' : 'none';";
    html += "        document.getElementById('flowRow').style.display = (this.value === 'YF401') ? 'table-row' : 'none';";
    html += "    });";
    html += "});";
    html += "</script>";
    html += "</head>";

    // Body Content
    html += "<body>";
    html += "<h1>Aspol Tracker Status</h1>";

    // Sensor Data Section
    html += "<div id='sensorData'>";
    html += "<h2>Sensor Monitoring</h2>";
    html += "<table>";
    html += "<tr><th>Current Value</th><td><span id='currentValue'>0.00</span></td></tr>";
    html += "<tr><th>Average Value</th><td><span id='avgValue'>0.00</span></td></tr>";
    html += "<tr><th>Threshold Level</th><td><span id='threshold'>0.00</span></td></tr>";
    html += "</table>";
    html += "</div>";

    // Device Status Section
    html += "<div class='status-card'>";
    html += "<h2>Device Status</h2><table>";

    // Time Status
    if (rtcInitialized) {
        html += "<tr><th>Current Time</th><td><span id='time'>Loading...</span></td></tr>";
    } else {
        html += "<tr><th>Time</th><td>RTC Not Initialized</td></tr>";
    }

    // Temperature Status
    if (bmpInitialized) {
        html += "<tr><th>Temperature</th><td><span id='temperature'>Loading...</span></td></tr>";
    } else {
        html += "<tr><th>Temperature</th><td>BMP Not Initialized</td></tr>";
    }

    // GPS Status
    char gpsMsgWeb[100];
    html += "<tr><th>GPS Status</th><td>";
    if (gps.location.isValid()) {
        html +=html += "LAT: "+String(gps.location.lat(),6) + "| LNG:"+ String(gps.location.lng(),6)+ "| SAT:" + String(gps.satellites.value());
    }else{
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
    html += "<tr><th>Sensor Type</th><td><select name='sensorType'>";
    html += "<option value='BMP'" + String(strcmp(currentConfig.currentSensor, "BMP") == 0 ? " selected" : "") + ">BMP Pressure Sensor</option>";
    html += "<option value='YF401'" + String(strcmp(currentConfig.currentSensor, "YF401") == 0 ? " selected" : "") + ">YF-401 Flow Meter</option>";
    html += "</select></td></tr>";
    html += "<tr id='pressureRow' style='display:" + String(strcmp(currentConfig.currentSensor, "BMP") == 0 ? "table-row" : "none") + ";'>";
    html += "<th>Pressure Threshold (%)</th><td><input type='number' step='0.1' name='pressureThreshold' value='" + String(currentConfig.pressureThreshold) + "'></td></tr>";
    html += "<tr id='flowRow' style='display:" + String(strcmp(currentConfig.currentSensor, "YF401") == 0 ? "table-row" : "none") + ";'>";
    html += "<th>Flow Threshold (%)</th><td><input type='number' step='0.1' name='flowThreshold' value='" + String(currentConfig.flowThreshold) + "'></td></tr>";
    html += "<tr><th>SSID</th><td><input type='text' name='ssid' value='" + String(currentConfig.ssid) + "'></td></tr>";
    html += "<tr><th>Password</th><td><input type='password' name='password' placeholder='Enter new password'></td></tr>";
    html += "<tr><th>Device Name</th><td><input type='text' name='deviceName' value='" + String(currentConfig.deviceName) + "'></td></tr>";
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
                // char message[80];
                // snprintf(message, sizeof(message), "GPS Updated - Satellites: %d", gps.satellites.value());
                // serialPrintln(message);
            }
        }
    }

    if (millis() > 5000 && gps.charsProcessed() < 10)
    {
        serialPrintln("No GPS detected");
    }
}

void addFlowReading(float flow)
{
    flowHistory.readings[flowHistory.index] = flow;
    flowHistory.index = (flowHistory.index + 1) % PRESSURE_HISTORY_SIZE;
    if (flowHistory.count < PRESSURE_HISTORY_SIZE)
        flowHistory.count++;
}

float calculateAverageFlow()
{
    if (flowHistory.count == 0)
        return 0;
    float sum = 0;
    for (int i = 0; i < flowHistory.count; i++)
        sum += flowHistory.readings[i];
    return sum / flowHistory.count;
}

void checkFlowAndLog()
{
    static unsigned long lastCheckTime = 0;
    unsigned long currentTime = millis();
    unsigned long timeDiff = currentTime - lastCheckTime;
    unsigned long test1 = 100;
    
    // Add guard clause for time difference
    if (timeDiff == 0) return;
    
    if (timeDiff >= 1000) {
        detachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN));
        flowRate = (pulseCount / calibrationFactor) * (1000.0 / max(timeDiff, test1));
        pulseCount = 0;
        lastCheckTime = currentTime;
        attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), pulseCounter, FALLING);

        addFlowReading(flowRate);
    }
    float averageFlow = calculateAverageFlow();
    float threshold = averageFlow * (1.0 + (currentConfig.flowThreshold/100.0));

    if (flowRate > threshold && millis() - lastLogTime > 2500 && flowRate > threshold)
    {
        lastLogTime = millis();
        char msg[100];
        snprintf(msg, sizeof(msg), "Flow rate: %.2f L/min (Avg: %.2f/T: %.2f)", flowRate, averageFlow,threshold);
        serialPrintln(msg);
        logGPSDataFlow(flowRate);
    }
}

void logGPSDataFlow(float flow)
{
    if (!sdCardAvailable || !gps.location.isValid())
        return;
    DateTime now = rtc.now();
    File file = SD.open("/flow_log.txt", FILE_APPEND);
    if (file)
    {
        file.printf("%02d/%02d/%04d,%02d:%02d:%02d,%.6f,%.6f,%.2f\n",
                    now.day(), now.month(), now.year(),
                    now.hour(), now.minute(), now.second(),
                    gps.location.lat(), gps.location.lng(), flow);
        file.close();
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

    pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), pulseCounter, FALLING);

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
void handlePressure() {
    String json;
    if (strcmp(currentConfig.currentSensor, "BMP") == 0) {
        if (!bmpInitialized) {
            server.send(500, "application/json", "{\"error\":\"BMP sensor not initialized\"}");
            return;
        }
        // BMP pressure handling
    } else {
        // Flow sensor handling
        if (flowRate == INFINITY) {  // Handle potential invalid values
            server.send(500, "application/json", "{\"error\":\"Invalid flow reading\"}");
            return;
        }
    }
    server.send(200, "application/json", json);
}

void checkPressureAndLog()
{
    if (!bmpInitialized) {
        serialPrintln("BMP sensor not initialized");
        return;
    }

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
    if (rtcInitialized)
    {
        DateTime now = rtc.now();
        String json = "{";
        json += "\"time\":\"" + String(now.timestamp(DateTime::TIMESTAMP_TIME)) + "\",";
        float temperature = 0.00;
        if (bmpInitialized){
            temperature = bmp.readTemperature();
        }
        json += "\"temperature\":" + String(temperature, 2);
        
        json += "}";

        server.send(200, "application/json", json);
    }
    else
    {
        server.send(500, "application/json", "{\"error\":\"RTC and BMP not initialized\"}");
    }
}

void loop()
{
    static unsigned long lastStatusUpdate = 0;
    const unsigned long STATUS_UPDATE_INTERVAL = 10000; // 10 seconds

    server.handleClient();
    processGPS();

    if (strcmp(currentConfig.currentSensor, "BMP") == 0) {
        if (bmpInitialized) checkPressureAndLog();
    } else {
        checkFlowAndLog();
    }

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
            // char gpsMsg[100];
            // snprintf(gpsMsg, sizeof(gpsMsg), "GPS - Lat: %.6f, Lng: %.6f, Satellites: %d",
            //          gps.location.lat(), gps.location.lng(), gps.satellites.value());
            // serialPrintln(gpsMsg);
        }
    }

    delay(100);
}