#include <Arduino.h>
#include <WiFiManager.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <SPIFFS.h>
#include <esp_task_wdt.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <deque>
#include <ESPmDNS.h>
#include <Update.h>
#include <HTTPClient.h> // <-- Add this line
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>

// Error codes
#define ERROR_NONE 0
#define ERROR_EMERGENCY 1
#define ERROR_PUMP_TIMEOUT 3
#define ERROR_TEMP_SENSOR 4

// --- Add these defines at the top of your file ---
#define FW_VERSION "1.0.0"

// Forward declarations
void saveConfig();
void loadConfig();
void configModeCallback(WiFiManager *myWiFiManager);
void setupWiFiManager();
void updateStatusLED();
void checkSensors();
void checkTemperature();
void notifyClients();
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len);
void handleMaintenanceRequest(AsyncWebServerRequest *request);
void setupWebServer();
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void handleSetDeviceName(AsyncWebServerRequest *request);
void getTemperatureHistory(AsyncWebServerRequest *request);
bool isDeviceNameValid(const char* name);
void handleApiStatus(AsyncWebServerRequest *request);
void handleApiControl(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
void checkForUpdates();
bool verifyFirmwareSignature(const uint8_t* firmware, size_t firmware_len, const uint8_t* sig, size_t sig_len);


// EEPROM Configuration
#define EEPROM_SIZE 512
#define CONFIG_START 0
struct Config {
    char device_name[40] = "reef-ato";
    bool valid = false;
    float minTemp = 22.0;
    float maxTemp = 30.0;
} config;

// Pin Definitions
const int SUMP_SENSOR_PIN = 26;
const int EMERGENCY_SENSOR_PIN = 27;
const int RODI_SENSOR_PIN = 5;
const int PUMP_RELAY_PIN = 4;
const int ONE_WIRE_BUS = 14;
const int NEOPIXEL_PIN = 2;
const int BUZZER_PIN = 12;
const int CONFIG_RESET_PIN = 15;
const int WIFI_RESET_PIN = 23; // Use IO23 now

// NeoPixel Configuration
#define NEOPIXEL_COUNT 1
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// OTA NeoPixel blinking state
volatile bool otaInProgress = false;
unsigned long otaLastBlink = 0;
bool otaLedState = false;

// Helper for OTA NeoPixel blinking (pink)
void handleOtaBlink() {
    if (otaInProgress) {
        unsigned long now = millis();
        if (now - otaLastBlink > 200) { // Blink every 200ms
            otaLedState = !otaLedState;
            pixels.setPixelColor(0, otaLedState ? pixels.Color(255, 0, 128) : pixels.Color(0, 0, 0)); // Pink blink
            pixels.show();
            otaLastBlink = now;
        }
    }
}

// Temperature Sensor Configuration
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// NTP Configuration
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// System Timing Parameters (all in milliseconds)
const unsigned long PUMP_TIMEOUT = 120000;
const unsigned long PUMP_COOLDOWN = 60000;
const unsigned long SENSOR_CHECK_INTERVAL = 100;
const unsigned long TEMP_CHECK_INTERVAL = 30000;

// System State Variables
struct SystemState {
    bool isPumping = false;
    bool isError = false;
    bool isSumpLow = false;
    bool isEmergencyHigh = false;
    bool isRodiLow = false;
    bool maintenanceMode = false;
    float temperature = 0.0;
    unsigned long pumpStartTime = 0;
    unsigned long lastPumpRun = 0;
    unsigned long lastSensorCheck = 0;
    unsigned long lastTempCheck = 0;
    int errorCode = ERROR_NONE;
    bool stateChanged = false;
    time_t lastPumpRunTime = 0;
    float minTemp = 22.0;
    float maxTemp = 30.0;
    // --- Update check state ---
    bool firmwareUpdateAvailable = false;
    bool frontendUpdateAvailable = false;
    String firmwareUpdateUrl;
    String frontendUpdateVersion;
    String firmwareUpdateVersion;
    String frontendUpdateUrl; // <-- ADD THIS LINE
} state;

// Global objects
WiFiManager wm;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Color definitions for NeoPixel
const uint32_t COLOR_OFF = pixels.Color(0, 0, 0);
const uint32_t COLOR_NORMAL = pixels.Color(0, 255, 0);
const uint32_t COLOR_PUMPING = pixels.Color(0, 0, 255);
const uint32_t COLOR_ERROR = pixels.Color(255, 0, 0);
const uint32_t COLOR_WARNING = pixels.Color(255, 80, 0);
const uint32_t COLOR_MAINTENANCE = pixels.Color(255, 255, 0);
const uint32_t COLOR_STARTUP = pixels.Color(255, 255, 255);
const uint32_t COLOR_RODI_LOW = pixels.Color(255, 0, 255);

// Define a structure to hold temperature readings
struct TemperatureReading {
    float temperature;
    time_t timestamp;
};

// Use a deque to store the last 24 hours of temperature readings
std::deque<TemperatureReading> temperatureHistory;

// Add these lines:
bool tempAlarmActive = false;
unsigned long lastTempAlarmTime = 0;
const unsigned long TEMP_ALARM_REPEAT_INTERVAL = 5UL * 60UL * 1000UL; // 5 minutes in ms

unsigned long maintenanceStartTime = 0;
const unsigned long MAINTENANCE_TIMEOUT = 60UL * 60UL * 1000UL; // 1 hour in ms

unsigned long lastMaintenanceNotify = 0; // Add at the top of your file

unsigned long lastUpdateCheck = 0;
const unsigned long UPDATE_CHECK_INTERVAL = 48UL * 60UL * 60UL * 1000UL; // every 48 hours

const char* public_key_pem = R"KEY(
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAqHJjjR+04aJD3M22CbZQ
gHUIamSVMbAUYGscDL5PQ2SCodD8jXFDsrRRPD0wkCXszkmIXMCNCT6+PpIXrqat
07QdIWmXBivtOJg9TGXjvXmITB3EgK1iBriq2/+0mYva2sC0psY6d9welFZHW52i
flS1XX7b3u1Uew56YI+xxe3klqVqQI1s18OHCAbVQIRtMiwxFeTGL5gZUkX1puQG
fqy+S8pfjwFSQqH+MkmvUKcOVVUx7TXCH5gi+09HSnKLjJ2W2JD26IFmev4O4mpp
2fb/J1f2DiglJb9vJ6djXYe45hcpkTwcuVfq9t6/brAykayEqEYikwxuF2gkfXh1
1QIDAQAB
-----END PUBLIC KEY-----
)KEY";

// Function implementations
void saveConfig() {
    EEPROM.put(CONFIG_START, config);
    EEPROM.commit();
}

void loadConfig() {
    EEPROM.get(CONFIG_START, config);
    config.device_name[sizeof(config.device_name) - 1] = '\0'; // Ensure null-termination
    Serial.printf("Loaded config: valid=%d, minTemp=%.2f, maxTemp=%.2f\n", config.valid, config.minTemp, config.maxTemp);
    if (!config.valid || !isDeviceNameValid(config.device_name)) {
        strlcpy(config.device_name, "reef-ato", sizeof(config.device_name));
        config.valid = true;
        config.minTemp = 22.0;
        config.maxTemp = 30.0;
        saveConfig();
    }
    state.minTemp = config.minTemp;
    state.maxTemp = config.maxTemp;
}

bool isDeviceNameValid(const char* name) {
    if (name == nullptr || strlen(name) == 0) return false;
    for (size_t i = 0; i < strlen(name); i++) {
        if (!isalnum(name[i]) && name[i] != '-') return false;
    }
    return true;
}

void configModeCallback(WiFiManager *myWiFiManager) {
    Serial.println("Entered config mode");
    pixels.setPixelColor(0, pixels.Color(0, 0, 255));
    pixels.show();
}

void saveConfigCallback() {
    Serial.println("WiFi credentials saved, restarting...");
    delay(1000);
    ESP.restart();
}

void setupWiFiManager() {
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setAPCallback(configModeCallback);

    WiFiManagerParameter custom_device_name("device", "Device Name", config.device_name, 40);
    wm.addParameter(&custom_device_name);

    std::vector<const char *> menu = {"wifi", "info", "param", "sep", "restart", "exit"};
    wm.setMenu(menu);

    wm.setConfigPortalTimeout(180);

    if (!wm.autoConnect("ATO-Setup")) {
        Serial.println("Failed to connect and hit timeout");
        delay(3000);
        ESP.restart();
    }

    strlcpy(config.device_name, custom_device_name.getValue(), sizeof(config.device_name));
    config.valid = true;
    saveConfig();
}

void updateAlarm() {
    static unsigned long lastToggle = 0;
    static bool buzzerOn = false;
    unsigned long now = millis();

    if (state.isError) {
        // Beep pattern: 500ms ON, 500ms OFF
        if (now - lastToggle > 500) {
            buzzerOn = !buzzerOn;
            lastToggle = now;
            ledcWrite(0, buzzerOn ? 127 : 0);
        }
    } else if (state.isRodiLow) {
        // RODI empty alarm: 100ms ON, 900ms OFF
        if (!buzzerOn && now - lastToggle > 900) {
            ledcWrite(0, 127);
            buzzerOn = true;
            lastToggle = now;
        } else if (buzzerOn && now - lastToggle > 100) {
            ledcWrite(0, 0);
            buzzerOn = false;
            lastToggle = now;
        }
    } else if (state.temperature > 30.0) {
        // Warning beep: 200ms ON, 1800ms OFF
        if (!buzzerOn && now - lastToggle > 1800) {
            ledcWrite(0, 127);
            buzzerOn = true;
            lastToggle = now;
        } else if (buzzerOn && now - lastToggle > 200) {
            ledcWrite(0, 0);
            buzzerOn = false;
            lastToggle = now;
        }
    } else {
        // No alarm
        ledcWrite(0, 0);
        buzzerOn = false;
        lastToggle = now;
    }
}

void updateStatusLED() {
    // Priority order: Error > Maintenance > Pumping > RODI Low > Sump Low > Normal
    if (state.isError) {
        pixels.setPixelColor(0, COLOR_ERROR);
    }
    else if (state.maintenanceMode) {
        pixels.setPixelColor(0, COLOR_MAINTENANCE);
    }
    else if (state.isPumping) {
        pixels.setPixelColor(0, COLOR_PUMPING);
    }
    else if (state.isRodiLow) {
        pixels.setPixelColor(0, COLOR_RODI_LOW);
    }
    else if (state.isSumpLow) {
        pixels.setPixelColor(0, COLOR_WARNING);
    }
    else {
        pixels.setPixelColor(0, COLOR_NORMAL);
    }
    pixels.show();
}

void checkSensors() {
    // Read sensor states
    bool currentSumpLow = !digitalRead(SUMP_SENSOR_PIN);
    bool currentEmergencyHigh = digitalRead(EMERGENCY_SENSOR_PIN); // ACTIVE LOW: error when LOW
    bool currentRodiLow = !digitalRead(RODI_SENSOR_PIN);
    unsigned long currentTime = millis();
    bool stateChanged = false;

    // Check for state changes
    if (currentSumpLow != state.isSumpLow) {
        state.isSumpLow = currentSumpLow;
        stateChanged = true;
        if (currentSumpLow) {
            Serial.println("Sump low detected");
        }
    }

    if (currentEmergencyHigh != state.isEmergencyHigh) {
        state.isEmergencyHigh = currentEmergencyHigh;
        stateChanged = true;
        if (currentEmergencyHigh) {
            state.isError = true;
            state.errorCode = ERROR_EMERGENCY;
        }
    }

    if (currentRodiLow != state.isRodiLow) {
        state.isRodiLow = currentRodiLow;
        stateChanged = true;
        Serial.printf("RODI state changed: %s\n", currentRodiLow ? "LOW" : "NORMAL");
    }

    // Clear error state if conditions are normal
    if (!state.isEmergencyHigh && state.isError && state.errorCode == ERROR_EMERGENCY) {
        state.isError = false;
        state.errorCode = ERROR_NONE;
        stateChanged = true;
        Serial.println("Error state cleared");
    }

    // Pump control logic
    if (!state.isError && !state.maintenanceMode) {
        // Check if we should start the pump
        if (state.isSumpLow && !state.isPumping &&
            (currentTime - state.lastPumpRun) > PUMP_COOLDOWN &&
            !state.isRodiLow) {

            state.isPumping = true;
            state.pumpStartTime = currentTime;
            digitalWrite(PUMP_RELAY_PIN, HIGH);
            Serial.println("Pump started");
            Serial.printf("Time since last run: %lu ms\n", currentTime - state.lastPumpRun);
            state.stateChanged = true;
        }

        // Check pump timeout
        if (state.isPumping && (currentTime - state.pumpStartTime) > PUMP_TIMEOUT) {
            state.isPumping = false;
            state.lastPumpRun = currentTime;
            state.lastPumpRunTime = timeClient.getEpochTime();
            state.isError = true;
            state.errorCode = ERROR_PUMP_TIMEOUT;
            digitalWrite(PUMP_RELAY_PIN, LOW);
            Serial.println("Pump timeout error!");
            state.stateChanged = true;
        }

        // Check if water level restored
        if (state.isPumping && !state.isSumpLow) {
            state.isPumping = false;
            state.lastPumpRun = currentTime;
            state.lastPumpRunTime = timeClient.getEpochTime();
            digitalWrite(PUMP_RELAY_PIN, LOW);
            Serial.println("Water level restored, pump stopped");
            Serial.printf("Next pump start allowed at: %lu ms\n", currentTime + PUMP_COOLDOWN);
            state.stateChanged = true;
        }
    }

    // Update LED and notify clients if state changed
    if (stateChanged || state.stateChanged) {
        updateStatusLED();
        notifyClients();
        state.stateChanged = false;
    }
}

void checkTemperature() {
    sensors.requestTemperatures();
    float newTemp = sensors.getTempCByIndex(0);

    // Handle sensor error
    if (newTemp == DEVICE_DISCONNECTED_C) {
        Serial.println("Temperature sensor error!");
        state.isError = true;
        state.errorCode = ERROR_TEMP_SENSOR;
        notifyClients();
        return;
    }

    // Clear temp sensor error if sensor is restored
    if (state.isError && state.errorCode == ERROR_TEMP_SENSOR && newTemp != DEVICE_DISCONNECTED_C) {
        state.isError = false;
        state.errorCode = ERROR_NONE;
        notifyClients();
    }

    if (abs(newTemp - state.temperature) > 0.1) {
        state.temperature = newTemp;
        notifyClients();
        Serial.printf("Temperature updated: %.2f°C\n", newTemp);

        TemperatureReading reading = { newTemp, (time_t)timeClient.getEpochTime() };
        temperatureHistory.push_back(reading);

        // Remove readings older than 24 hours (86400 seconds)
        while (!temperatureHistory.empty() &&
               (timeClient.getEpochTime() - temperatureHistory.front().timestamp) > 86400) {
            temperatureHistory.pop_front();
        }
    }

    // Temperature alarm logic
    float minTemp = state.minTemp;
    float maxTemp = state.maxTemp;
    unsigned long now = millis();

    if ((newTemp > maxTemp || newTemp < minTemp)) {
        if (!tempAlarmActive || (now - lastTempAlarmTime > TEMP_ALARM_REPEAT_INTERVAL)) {
            // Trigger buzzer alarm
            for (int i = 0; i < 10; ++i) { // 10 beeps
                ledcWrite(0, 127);
                delay(100);
                ledcWrite(0, 0);
                delay(100);
            }
            lastTempAlarmTime = now;
            tempAlarmActive = true;
        }
    } else {
        tempAlarmActive = false;
    }
}

void notifyClients() {
    if (ws.count() > 0) {
        StaticJsonDocument<512> doc;
        doc["pumping"] = state.isPumping;
        doc["error"] = state.isError;
        doc["error_code"] = state.errorCode;
        doc["sump_low"] = state.isSumpLow;
        doc["emergency_high"] = state.isEmergencyHigh;
        doc["rodi_low"] = state.isRodiLow;
        doc["temperature"] = (int)(state.temperature * 10) / 10.0;
        doc["uptime"] = millis() / 1000;
        doc["wifi_signal"] = WiFi.RSSI();
        doc["maintenance_mode"] = state.maintenanceMode;
        doc["last_pump_run"] = state.lastPumpRunTime;
        doc["device_name"] = config.device_name;
        doc["min_temp"] = state.minTemp;
        doc["max_temp"] = state.maxTemp;
        doc["fw_version"] = FW_VERSION;

        // --- Add: Read frontend version from SPIFFS ---
        File fvFile = SPIFFS.open("/frontend_version.json", "r");
        if (fvFile) {
            StaticJsonDocument<64> fvDoc;
            DeserializationError err = deserializeJson(fvDoc, fvFile);
            if (!err && fvDoc.containsKey("version")) {
                doc["frontend_version"] = fvDoc["version"].as<const char*>();
            } else {
                doc["frontend_version"] = "unknown";
            }
            fvFile.close();
        } else {
            doc["frontend_version"] = "unknown";
        }

        doc["firmware_update_available"] = state.firmwareUpdateAvailable;
        doc["frontend_update_available"] = state.frontendUpdateAvailable;
        doc["firmware_update_url"] = state.firmwareUpdateUrl;
        doc["firmware_update_version"] = state.firmwareUpdateVersion;
        doc["frontend_update_version"] = state.frontendUpdateVersion;
        if (state.maintenanceMode && maintenanceStartTime > 0) {
            unsigned long elapsed = millis() - maintenanceStartTime;
            unsigned long remaining = (elapsed < MAINTENANCE_TIMEOUT) ? (MAINTENANCE_TIMEOUT - elapsed) : 0;
            doc["maintenance_remaining"] = remaining / 1000; // seconds
        } else {
            doc["maintenance_remaining"] = 0;
        }

        String response;
        serializeJson(doc, response);
        ws.textAll(response);
    }
}

void handleMaintenanceRequest(AsyncWebServerRequest *request) {
    if (request->hasParam("enable", true)) {
        bool enable = request->getParam("enable", true)->value() == "true";
        if (state.maintenanceMode != enable) {
            state.maintenanceMode = enable;
            state.stateChanged = true;
            if (enable) {
                maintenanceStartTime = millis(); // Start timer
            } else {
                maintenanceStartTime = 0;
            }
            if (enable && state.isPumping) {
                state.isPumping = false;
                digitalWrite(PUMP_RELAY_PIN, LOW);
            }
            updateStatusLED();
            notifyClients();
            Serial.printf("Maintenance mode %s\n", enable ? "enabled" : "disabled");
        }
        request->send(200, "text/plain", "OK");
    } else {
        request->send(400, "text/plain", "Missing enable parameter");
    }
}

void setupWebServer() {
    if(!SPIFFS.begin(true)) {
        Serial.println("An error occurred while mounting SPIFFS");
        return;
    }

    Serial.println("Files in SPIFFS:");
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    while(file) {
        Serial.printf("- %s (%d bytes)\n", file.name(), file.size());
        file = root.openNextFile();
    }

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/index.html", "text/html");
    });

    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/style.css", "text/css");
    });

    server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/script.js", "application/javascript");
    });

    server.on("/favicon.png", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/favicon.png", "image/png");
    });

    server.on("/maintenance", HTTP_POST, handleMaintenanceRequest);
    server.on("/temperature_history", HTTP_GET, getTemperatureHistory);
    server.on("/set_device_name", HTTP_POST, handleSetDeviceName);

    server.on("/set_temp_range", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->hasParam("min", true) && request->hasParam("max", true)) {
            float minT = request->getParam("min", true)->value().toFloat();
            float maxT = request->getParam("max", true)->value().toFloat();
            state.minTemp = minT;
            state.maxTemp = maxT;
            config.minTemp = minT;
            config.maxTemp = maxT;
            saveConfig();
            request->send(200, "text/plain", "Temperature range updated");
            Serial.printf("Temperature range updated: min=%.2f, max=%.2f\n", minT, maxT);
        } else {
            request->send(400, "text/plain", "Missing parameters");
        }
    });

    server.on("/reset_wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "WiFi settings will be reset, rebooting...");
        Serial.println("WiFi reset requested from web!");
        wm.resetSettings();
        delay(500);
        ESP.restart();
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/update.html", "text/html");
    });

    // --- Firmware OTA with NeoPixel blinking ---
    server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request){
        otaInProgress = false; // Stop blinking
        bool shouldReboot = !Update.hasError();
        request->send(200, "text/plain", shouldReboot ? "OK" : "FAIL");
        if (shouldReboot) {
            delay(1000);
            ESP.restart();
        }
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if (!index) {
            otaInProgress = true; // Start blinking
            otaLastBlink = millis();
            otaLedState = false;
            Serial.printf("Update Start: %s\n", filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
        }
        if (!Update.hasError()) {
            if (Update.write(data, len) != len) Update.printError(Serial);
        }
        if (final) {
            if (Update.end(true)) {
                Serial.printf("Update Success: %u bytes\n", index+len);
            } else {
                Update.printError(Serial);
            }
            otaInProgress = false; // Stop blinking
            pixels.setPixelColor(0, COLOR_STARTUP); // Or your normal color
            pixels.show();
        }
    });

    // --- SPIFFS OTA with NeoPixel blinking ---
    server.on("/spiffs_update", HTTP_POST, [](AsyncWebServerRequest *request){
        otaInProgress = false; // Stop blinking
        bool shouldReboot = !Update.hasError();
        request->send(200, "text/plain", shouldReboot ? "OK" : "FAIL");
        if (shouldReboot) {
            delay(1000);
            ESP.restart();
        }
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
        if (!index) {
            otaInProgress = true; // Start blinking
            otaLastBlink = millis();
            otaLedState = false;
            Serial.printf("SPIFFS Update Start: %s\n", filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) Update.printError(Serial);
        }
        if (!Update.hasError()) {
            if (Update.write(data, len) != len) Update.printError(Serial);
        }
        if (final) {
            if (Update.end(true)) {
                Serial.printf("SPIFFS Update Success: %u bytes\n", index+len);
            } else {
                Update.printError(Serial);
            }
            otaInProgress = false; // Stop blinking
            pixels.setPixelColor(0, COLOR_STARTUP); // Or your normal color
            pixels.show();
        }
    });

    server.on("/logo.png", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/logo.png", "image/png");
    });

    server.on("/api/status", HTTP_GET, handleApiStatus);

    server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *request){},
        NULL,
        handleApiControl);

    // --- One-click OTA update endpoint ---
    server.on("/api/auto_update", HTTP_POST, [](AsyncWebServerRequest *request){
    // Firmware update takes priority
    if (state.firmwareUpdateAvailable && state.firmwareUpdateUrl.length() > 0) {
        request->send(200, "text/plain", "Starting firmware OTA update...");
        String* url = new String(state.firmwareUpdateUrl);
        xTaskCreate([](void* param){
            otaInProgress = true;
            String url = *(String*)param;
            delete (String*)param;
            delay(1000);

            HTTPClient http;
            http.begin(url);
            int code = http.GET();
            if (code != 200) {
                Serial.printf("Firmware download failed: %d\n", code);
                http.end();
                otaInProgress = false;
                vTaskDelete(NULL);
                return;
            }
            size_t firmware_len = http.getSize();
            size_t totalBytes = SPIFFS.totalBytes();
            size_t usedBytes = SPIFFS.usedBytes();
            Serial.printf("SPIFFS total: %u, used: %u, free: %u\n", totalBytes, usedBytes, totalBytes - usedBytes);
            if (totalBytes - usedBytes < firmware_len + 4096) {
                Serial.println("Not enough SPIFFS space for update!");
                otaInProgress = false;
                vTaskDelete(NULL);
                http.end();
                return;
            }
            File fwFile = SPIFFS.open("/fwupdate.bin", FILE_WRITE);
            if (!fwFile) {
                Serial.println("Failed to open file for writing");
                otaInProgress = false;
                vTaskDelete(NULL);
                http.end();
                return;
            }
            WiFiClient* stream = http.getStreamPtr();
            uint8_t buf[1024];
            size_t total = 0;
            while (http.connected() && total < firmware_len) {
                size_t available = stream->available();
                if (available) {
                    int read = stream->read(buf, std::min(available, sizeof(buf)));
                    if (read > 0) {
                        fwFile.write(buf, read);
                        total += read;
                    }
                }
                delay(1);
            }
            fwFile.close();
            http.end();

            // --- Download firmware.bin.sig ---
            String sigUrl = url + ".sig";
            http.begin(sigUrl);
            code = http.GET();
            if (code != 200) {
                Serial.printf("Signature download failed: %d\n", code);
                http.end();
                SPIFFS.remove("/fwupdate.bin");
                otaInProgress = false;
                vTaskDelete(NULL);
                return;
            }
            size_t sig_len = http.getSize();
            std::unique_ptr<uint8_t[]> sig_data(new uint8_t[sig_len]);
            stream = http.getStreamPtr();
            size_t offset = 0;
            while (http.connected() && offset < sig_len) {
                size_t available = stream->available();
                if (available) {
                    int read = stream->read(sig_data.get() + offset, available);
                    offset += read;
                }
                delay(1);
            }
            http.end();

            // --- Hash firmware in chunks for signature verification ---
            fwFile = SPIFFS.open("/fwupdate.bin", FILE_READ);
            if (!fwFile) {
                Serial.println("Failed to open firmware file for verification");
                SPIFFS.remove("/fwupdate.bin");
                otaInProgress = false;
                vTaskDelete(NULL);
                return;
            }
            mbedtls_sha256_context ctx;
            mbedtls_sha256_init(&ctx);
            mbedtls_sha256_starts_ret(&ctx, 0);
            while (fwFile.available()) {
                int read = fwFile.read(buf, sizeof(buf));
                if (read > 0) {
                    mbedtls_sha256_update_ret(&ctx, buf, read);
                }
            }
            uint8_t hash[32];
            mbedtls_sha256_finish_ret(&ctx, hash);
            mbedtls_sha256_free(&ctx);
            fwFile.close();

            // --- Verify signature ---
            if (!verifyFirmwareSignature(hash, sizeof(hash), sig_data.get(), sig_len)) {
                Serial.println("Firmware signature verification failed!");
                SPIFFS.remove("/fwupdate.bin");
                otaInProgress = false;
                vTaskDelete(NULL);
                return;
            }
            Serial.println("Firmware signature verified!");

            // --- Stream from SPIFFS to Update ---
            fwFile = SPIFFS.open("/fwupdate.bin", FILE_READ);
            if (!fwFile) {
                Serial.println("Failed to open firmware file for flashing");
                SPIFFS.remove("/fwupdate.bin");
                otaInProgress = false;
                vTaskDelete(NULL);
                return;
            }
            if (Update.begin(firmware_len)) {
                size_t written = 0;
                while (fwFile.available()) {
                    size_t toRead = std::min((size_t)1024, (size_t)(firmware_len - written));
                    int read = fwFile.read(buf, toRead);
                    if (read > 0) {
                        if (Update.write(buf, read) != (size_t)read) {
                            Serial.println("Update.write() failed!");
                            break;
                        }
                        written += read;
                    }
                }
                if (Update.end()) {
                    Serial.println("Firmware OTA Success, rebooting...");
                    fwFile.close();
                    SPIFFS.remove("/fwupdate.bin");
                    otaInProgress = false;
                    delay(1000);
                    ESP.restart();
                } else {
                    Serial.println("Update.end() failed!");
                }
            } else {
                Serial.println("Update.begin() failed!");
            }
            fwFile.close();
            SPIFFS.remove("/fwupdate.bin");
            otaInProgress = false;
            vTaskDelete(NULL);
        }, "ota_task", 16384, url, 1, NULL);
        return;
    }

// --- SPIFFS (frontend) update with signature verification (STREAMED, NO TEMP FILE) ---
if (state.frontendUpdateAvailable && state.frontendUpdateUrl.length() > 0) {
    request->send(200, "text/plain", "Starting SPIFFS OTA update...");
    String* url = new String(state.frontendUpdateUrl);
    xTaskCreate([](void* param){
        otaInProgress = true;
        String url = *(String*)param;
        delete (String*)param;
        delay(1000);

        HTTPClient http;
        http.begin(url);
        int code = http.GET();
        if (code != 200) {
            Serial.printf("SPIFFS download failed: %d\n", code);
            http.end();
            otaInProgress = false;
            vTaskDelete(NULL);
            return;
        }
        size_t spiffs_len = http.getSize();

        Serial.printf("Starting SPIFFS OTA: %u bytes\n", (unsigned)spiffs_len);

        // --- Download signature ---
        String sigUrl = url + ".sig";
        HTTPClient sigHttp;
        sigHttp.begin(sigUrl);
        int sigCode = sigHttp.GET();
        if (sigCode != 200) {
            Serial.printf("Signature download failed: %d\n", sigCode);
            sigHttp.end();
            http.end();
            otaInProgress = false;
            vTaskDelete(NULL);
            return;
        }
        size_t sig_len = sigHttp.getSize();
        std::unique_ptr<uint8_t[]> sig_data(new uint8_t[sig_len]);
        WiFiClient* sigStream = sigHttp.getStreamPtr();
        size_t sig_offset = 0;
        while (sigHttp.connected() && sig_offset < sig_len) {
            size_t available = sigStream->available();
            if (available) {
                int read = sigStream->read(sig_data.get() + sig_offset, available);
                sig_offset += read;
            }
            delay(1);
        }
        sigHttp.end();

        // --- Stream & hash SPIFFS image directly ---
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[1024];
        size_t written = 0;
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);

        if (Update.begin(spiffs_len, U_SPIFFS)) {
        size_t lastPrinted = 0;
        while (http.connected() && written < spiffs_len) {
            size_t available = stream->available();
            if (available) {
                int read = stream->read(buf, std::min(available, sizeof(buf)));
                if (read > 0) {
                    mbedtls_sha256_update(&ctx, buf, read);
                    if (Update.write(buf, read) != (size_t)read) {
                        Serial.println("SPIFFS Update.write() failed!");
                        break;
                    }
                    written += read;
                    int percent = (100 * written) / spiffs_len;
                    if (percent - lastPrinted >= 5) {
                        Serial.printf("SPIFFS OTA Progress: %d%% (%u/%u bytes)\n", percent, (unsigned)written, (unsigned)spiffs_len);
                        lastPrinted = percent;
                    }
                }      
            }
            delay(1);
}
            uint8_t hash[32];
            mbedtls_sha256_finish(&ctx, hash);
            mbedtls_sha256_free(&ctx);

            // --- Verify signature ---
            if (!verifyFirmwareSignature(hash, sizeof(hash), sig_data.get(), sig_len)) {
                Serial.println("SPIFFS signature verification failed!");
                Update.abort();
                otaInProgress = false;
                http.end();
                vTaskDelete(NULL);
                return;
            }
            Serial.println("SPIFFS signature verified!");

            if (Update.end()) {
                Serial.println("SPIFFS OTA Success, rebooting...");
                otaInProgress = false;
                delay(1000);
                ESP.restart();
            } else {
                Serial.println("SPIFFS Update.end() failed!");
            }
        } else {
            Serial.println("SPIFFS Update.begin() failed!");
        }
        http.end();
        otaInProgress = false;
        vTaskDelete(NULL);
    }, "spiffs_ota_task", 16384, url, 1, NULL);
    return;
}

    // If neither update is available
    request->send(400, "text/plain", "No firmware or SPIFFS update available.");
});

    server.on("/frontend_version.json", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/frontend_version.json", "application/json");
    });

    server.on("/api/check_updates", HTTP_POST, [](AsyncWebServerRequest *request){
    checkForUpdates();
    request->send(200, "text/plain", "Checked for updates!");
    });

    //server.serveStatic("/", SPIFFS, "/"); // LAST
    server.begin();
    Serial.println("Web server started");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
            notifyClients();
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("WebSocket client #%u disconnected\n", client->id());
            break;
        case WS_EVT_DATA:
            handleWebSocketMessage(arg, data, len);
            break;
        case WS_EVT_PONG:
        case WS_EVT_ERROR:
            break;
    }
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        char msg[len + 1];
        memcpy(msg, data, len);
        msg[len] = 0;
        StaticJsonDocument<200> doc;
        DeserializationError error = deserializeJson(doc, msg);

        if (!error) {
            if (doc.containsKey("maintenance")) {
                bool enable = doc["maintenance"];
                if (state.maintenanceMode != enable) {
                    state.maintenanceMode = enable;
                    state.stateChanged = true;
                    if (enable) {
                        maintenanceStartTime = millis(); // <-- ADD THIS LINE
                    } else {
                        maintenanceStartTime = 0;
                    }
                    if (enable && state.isPumping) {
                        state.isPumping = false;
                        digitalWrite(PUMP_RELAY_PIN, LOW);
                    }
                    updateStatusLED();
                    notifyClients();
                }
            }
            else if (doc.containsKey("reset_error")) {
                if (state.isError && state.errorCode == ERROR_PUMP_TIMEOUT) {
                    state.isError = false;
                    state.errorCode = ERROR_NONE;
                    state.lastPumpRun = millis() - PUMP_COOLDOWN;
                    state.stateChanged = true;
                    updateStatusLED();
                    notifyClients();
                    Serial.println("Error state cleared by user");
                }
            }
        }
    }
}

void getTemperatureHistory(AsyncWebServerRequest *request) {
    StaticJsonDocument<2048> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& reading : temperatureHistory) {
        JsonObject entry = arr.createNestedObject();
        entry["temperature"] = reading.temperature;
        entry["timestamp"] = reading.timestamp;
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void handleSetDeviceName(AsyncWebServerRequest *request) {
    if (request->hasParam("device_name", true)) {
        String newDeviceName = request->getParam("device_name", true)->value();
        newDeviceName.trim();

        // Validate: only alphanumeric and hyphen
        bool valid = true;
        for (size_t i = 0; i < newDeviceName.length(); i++) {
            char c = newDeviceName.charAt(i);
            if (!isalnum(c) && c != '-') {
                valid = false;
                break;
            }
        }
        if (!valid || newDeviceName.isEmpty()) {
            request->send(400, "text/plain", "Invalid device name (use alphanumeric + hyphen)");
            return;
        }

        // Append "-ato" if not present
        String mdnsName = newDeviceName;
        if (!mdnsName.endsWith("-ato")) {
            mdnsName += "-ato";
        }

        // Store in config and EEPROM
        memset(config.device_name, 0, sizeof(config.device_name));
        strlcpy(config.device_name, mdnsName.c_str(), sizeof(config.device_name) - 1);
        saveConfig();

        // Restart mDNS
        MDNS.end();
        if (MDNS.begin(config.device_name)) {
            MDNS.addService("http", "tcp", 80);
            request->send(200, "text/plain", "Device name updated! Now available at http://" + mdnsName + ".local");
            Serial.printf("mDNS updated: http://%s.local\n", config.device_name);
        } else {
            request->send(500, "text/plain", "Failed to restart mDNS");
        }
    } else {
        request->send(400, "text/plain", "Missing device_name parameter");
    }
}

void handleApiStatus(AsyncWebServerRequest *request) {
    StaticJsonDocument<512> doc;
    doc["pumping"] = state.isPumping;
    doc["error"] = state.isError;
    doc["error_code"] = state.errorCode;
    doc["sump_low"] = state.isSumpLow;
    doc["emergency_high"] = state.isEmergencyHigh;
    doc["rodi_low"] = state.isRodiLow;
    doc["temperature"] = (int)(state.temperature * 10) / 10.0;
    doc["uptime"] = millis() / 1000;
    doc["wifi_signal"] = WiFi.RSSI();
    doc["maintenance_mode"] = state.maintenanceMode;
    doc["last_pump_run"] = state.lastPumpRunTime;
    doc["device_name"] = config.device_name;
    doc["min_temp"] = state.minTemp;
    doc["max_temp"] = state.maxTemp;
    doc["fw_version"] = FW_VERSION;

    // --- Add: Read frontend version from SPIFFS ---
    File fvFile = SPIFFS.open("/frontend_version.json", "r");
    if (fvFile) {
        StaticJsonDocument<64> fvDoc;
        DeserializationError err = deserializeJson(fvDoc, fvFile);
        if (!err && fvDoc.containsKey("version")) {
            doc["frontend_version"] = fvDoc["version"].as<const char*>();
        } else {
            doc["frontend_version"] = "unknown";
        }
        fvFile.close();
    } else {
        doc["frontend_version"] = "unknown";
    }

    doc["firmware_update_available"] = state.firmwareUpdateAvailable;
    doc["frontend_update_available"] = state.frontendUpdateAvailable;
    //  doc["firmware_update_url"] = state.firmwareUpdateUrl;
    doc["firmware_update_version"] = state.firmwareUpdateVersion;
    doc["frontend_update_version"] = state.frontendUpdateVersion;
    //  doc["frontend_update_url"] = state.frontendUpdateUrl;
    if (state.maintenanceMode && maintenanceStartTime > 0) {
        unsigned long elapsed = millis() - maintenanceStartTime;
        unsigned long remaining = (elapsed < MAINTENANCE_TIMEOUT) ? (MAINTENANCE_TIMEOUT - elapsed) : 0;
        doc["maintenance_remaining"] = remaining / 1000; // seconds
    } else {
        doc["maintenance_remaining"] = 0;
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

void handleApiControl(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, data, len);
    if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    if (doc.containsKey("maintenance")) {
        bool enable = doc["maintenance"];
        if (state.maintenanceMode != enable) {
            state.maintenanceMode = enable;
            state.stateChanged = true;
            if (enable) {
                maintenanceStartTime = millis();
            } else {
                maintenanceStartTime = 0;
            }
            if (enable && state.isPumping) {
                state.isPumping = false;
                digitalWrite(PUMP_RELAY_PIN, LOW);
            }
            updateStatusLED();
            notifyClients();
        }
    }
    if (doc.containsKey("reset_error")) {
        if (state.isError && state.errorCode == ERROR_PUMP_TIMEOUT) {
            state.isError = false;
            state.errorCode = ERROR_NONE;
            state.lastPumpRun = millis() - PUMP_COOLDOWN;
            state.stateChanged = true;
            updateStatusLED();
            notifyClients();
        }
    }
    request->send(200, "application/json", "{\"result\":\"OK\"}");
}

void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting ATO System...");

    EEPROM.begin(EEPROM_SIZE);

    loadConfig();

    pixels.begin();
    pixels.setBrightness(153); // 60% brightness
    pixels.clear();
    pixels.setPixelColor(0, COLOR_STARTUP);
    pixels.show();

    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);

    pinMode(SUMP_SENSOR_PIN, INPUT_PULLUP);
    pinMode(EMERGENCY_SENSOR_PIN, INPUT_PULLUP);
    pinMode(RODI_SENSOR_PIN, INPUT_PULLUP);
    pinMode(PUMP_RELAY_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(CONFIG_RESET_PIN, INPUT_PULLUP);
    pinMode(WIFI_RESET_PIN, INPUT_PULLUP);

    digitalWrite(PUMP_RELAY_PIN, LOW);

    ledcSetup(0, 2000, 8);
    ledcAttachPin(BUZZER_PIN, 0);
    ledcWrite(0, 0);

    if (digitalRead(CONFIG_RESET_PIN) == LOW) {
        Serial.println("Config reset requested");
        wm.resetSettings();
        EEPROM.put(CONFIG_START, Config());
        EEPROM.commit();
        ESP.restart();
    }

    setupWiFiManager();

    if (!MDNS.begin(config.device_name)) {
        Serial.println("Error starting mDNS");
        // Fallback to default if current name is invalid
        memset(config.device_name, 0, sizeof(config.device_name));
        strlcpy(config.device_name, "reef-ato", sizeof(config.device_name));
        if (!MDNS.begin(config.device_name)) {
            Serial.println("Failed to start mDNS with fallback name!");
        }
    } else {
        Serial.printf("mDNS started: http://%s.local\n", config.device_name);
        MDNS.addService("http", "tcp", 80);
    }

    sensors.begin();
    checkTemperature();

    setupWebServer();

    timeClient.begin();

    checkSensors();

    if (!state.isEmergencyHigh && state.isError && state.errorCode == ERROR_EMERGENCY) {
        state.isError = false;
        state.errorCode = ERROR_NONE;
        Serial.println("Error state cleared on boot (no emergency detected)");
    }

    updateStatusLED();

    Serial.println("ATO System Initialized");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    checkForUpdates();
}

void loop() {
    handleOtaBlink();
    esp_task_wdt_reset();
    ws.cleanupClients();
    unsigned long currentTime = millis();
    timeClient.update();

    if (currentTime - state.lastSensorCheck >= SENSOR_CHECK_INTERVAL) {
        checkSensors();
        state.lastSensorCheck = currentTime;
    }

    if (currentTime - state.lastTempCheck >= TEMP_CHECK_INTERVAL) {
        checkTemperature();
        state.lastTempCheck = currentTime;
    }

    if (!state.isError && !state.maintenanceMode) {
        if (state.isSumpLow && !state.isPumping) {
            Serial.printf("Pump conditions: Sump=%d, Pump=%d, RODI=%d, Cooldown=%d\n",
                state.isSumpLow, state.isPumping, state.isRodiLow,
                (currentTime - state.lastPumpRun) > PUMP_COOLDOWN);

            if (state.isRodiLow) {
                Serial.println("Pump cannot start: RODI reservoir is empty");
            } else if ((currentTime - state.lastPumpRun) <= PUMP_COOLDOWN) {
                Serial.println("Pump cannot start: Cooldown period not elapsed");
            } else {
                Serial.println("Pump should start: All conditions met");
            }
        }

        if (state.isSumpLow && !state.isPumping &&
            (currentTime - state.lastPumpRun) > PUMP_COOLDOWN &&
            !state.isRodiLow) {
            state.isPumping = true;
            state.pumpStartTime = currentTime;
            digitalWrite(PUMP_RELAY_PIN, HIGH);
            Serial.println("Pump started");
            Serial.printf("Time since last run: %lu ms\n", currentTime - state.lastPumpRun);
            state.stateChanged = true;
        }

        if (state.isPumping && (currentTime - state.pumpStartTime) > PUMP_TIMEOUT) {
            state.isPumping = false;
            state.lastPumpRun = currentTime;
            state.lastPumpRunTime = timeClient.getEpochTime();
            state.isError = true;
            state.errorCode = ERROR_PUMP_TIMEOUT;
            digitalWrite(PUMP_RELAY_PIN, LOW);
            Serial.println("Pump timeout error!");
            state.stateChanged = true;
        }

        if (state.isPumping && !state.isSumpLow) {
            state.isPumping = false;
            state.lastPumpRun = currentTime;
            state.lastPumpRunTime = timeClient.getEpochTime();
            digitalWrite(PUMP_RELAY_PIN, LOW);
            Serial.println("Water level restored, pump stopped");
            Serial.printf("Next pump start allowed at: %lu ms\n", currentTime + PUMP_COOLDOWN);
            state.stateChanged = true;
        }
    }

    // --- Maintenance mode auto-exit ---
    if (state.maintenanceMode && maintenanceStartTime > 0) {
        unsigned long elapsed = millis() - maintenanceStartTime;
        if (elapsed >= MAINTENANCE_TIMEOUT) {
            state.maintenanceMode = false;
            maintenanceStartTime = 0;
            state.stateChanged = true;
            Serial.println("Maintenance mode auto-exited after timeout");
            updateStatusLED();
            notifyClients();
        } else {
            // Notify clients every second
            if (millis() - lastMaintenanceNotify > 1000) {
                notifyClients();
                lastMaintenanceNotify = millis();
            }
        }
    }

    if (state.stateChanged) {
        updateStatusLED();
        notifyClients();
        state.stateChanged = false;
    }

    static unsigned long resetBtnStart = 0;
    if (digitalRead(WIFI_RESET_PIN) == LOW) {
        if (resetBtnStart == 0) resetBtnStart = millis();
        if (millis() - resetBtnStart > 3000) {
            Serial.println("WiFi reset button pressed, resetting WiFi settings...");
            wm.resetSettings();
            delay(500);
            ESP.restart();
        }
    } else {
        resetBtnStart = 0;
    }

    delay(10);

    updateAlarm();
    if (millis() - lastUpdateCheck > UPDATE_CHECK_INTERVAL) {
        checkForUpdates();
        lastUpdateCheck = millis();
    }
}

// --- Add this function ---
void checkForUpdates() {
    Serial.println("[Update] Checking for updates...");
    HTTPClient http;
    http.setTimeout(5000);
    http.begin("http://updates.reef.mk/ato-controller/version.json");
    http.setAuthorization("edcom2021#", "83P54^iKdjwtab%");
    int httpCode = http.GET();
    Serial.printf("[Update] HTTP status: %d\n", httpCode);
    if (httpCode == 200) {
        String payload = http.getString();
        StaticJsonDocument<2048> doc;
        DeserializationError error = deserializeJson(doc, payload);
        if (!error) {
            String latestFw = doc["firmware"]["version"] | "";
            String latestFrontend = doc["frontend"]["version"] | "";
            if (latestFw != FW_VERSION) {
                state.firmwareUpdateAvailable = true;
                state.firmwareUpdateUrl = doc["firmware"]["url"].as<String>();
                state.firmwareUpdateVersion = latestFw;
            } else {
                state.firmwareUpdateAvailable = false;
                state.firmwareUpdateVersion = "";
                state.firmwareUpdateUrl = "";
            }
            if (!latestFrontend.isEmpty()) {
                state.frontendUpdateAvailable = true;
                state.frontendUpdateVersion = latestFrontend;
                if (doc["frontend"].containsKey("url") && doc["frontend"]["url"].is<const char*>()) {
                    state.frontendUpdateUrl = doc["frontend"]["url"].as<const char*>();
                } else {
                    state.frontendUpdateUrl = "";
                }
            } else {
                state.frontendUpdateAvailable = false;
                state.frontendUpdateVersion = "";
                state.frontendUpdateUrl = "";
            }
        }
    }
    http.end();
}

bool verifyFirmwareSignature(const uint8_t* hash, size_t hash_len, const uint8_t* sig, size_t sig_len) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_public_key(&pk, (const uint8_t*)public_key_pem, strlen(public_key_pem) + 1);
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        return false;
    }
    ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, hash_len, sig, sig_len);
    mbedtls_pk_free(&pk);
    return ret == 0;
}
