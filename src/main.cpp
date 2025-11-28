#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// Web server for configuration
WebServer server(80);
Preferences preferences;

// Configuration variables
String wifi_ssid = "";
String wifi_password = "";
String mqtt_server = "";
String mqtt_port = "1883";
String mqtt_user = "";
String mqtt_pass = "";
String mqtt_topic = "sensors/data";
String device_id = "device01";

#define CONFIG_BUTTON 0  // Boot button for reset config

// Add LED pin definition
#define LED_PIN 2  // Built-in LED on most ESP32 boards

// OLED display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1        // Reset pin (-1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C  // Usually 0x3C for 128x64
#define OLED_SDA 21         // Default SDA pin for ESP32
#define OLED_SCL 22         // Default SCL pin for ESP32

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Define RS485 pins for ESP32
#define RX_PIN 16  // GPIO16 
#define TX_PIN 17  // GPIO17
#define RTS_PIN 4  // GPIO4 for flow control

// Task handles
TaskHandle_t TaskWiFiHandle = NULL;
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t displayTaskHandle = NULL;
TaskHandle_t mqttTaskHandle = NULL;

// Queue handle
QueueHandle_t sensorQueue = NULL;
QueueHandle_t mqttQueue = NULL;

// Mutex for serial communication
SemaphoreHandle_t serialMutex = NULL;

// Create a Serial instance for RS485
HardwareSerial RS485Serial(2); // Using UART2

// Create WiFi and MQTT clients
WiFiClient espClient;
PubSubClient client(espClient);

// Configuration mode flag
bool configMode = false;

// JXCT NPK Sensor commands
const byte readN[] = {0x01, 0x03, 0x00, 0x1E, 0x00, 0x01, 0xE4, 0x0C};
const byte readP[] = {0x01, 0x03, 0x00, 0x1F, 0x00, 0x01, 0xB5, 0xCC};
const byte readK[] = {0x01, 0x03, 0x00, 0x20, 0x00, 0x01, 0x85, 0xC0};
const byte readNPK[] = {0x01, 0x03, 0x00, 0x1E, 0x00, 0x03, 0x65, 0xCD};
const byte readTemp[] = {0x01, 0x03, 0x00, 0x13, 0x00, 0x01, 0x75, 0xCF};
const byte readMoisture[] = {0x01, 0x03, 0x00, 0x12, 0x00, 0x01, 0x24, 0x0F};
const byte readEC[] = {0x01, 0x03, 0x00, 0x15, 0x00, 0x01, 0x95, 0xCE};
const byte readPH[] = {0x01, 0x03, 0x00, 0x06, 0x00, 0x01, 0x64, 0x0B};

const int commandLength = 8;

// Struct to store sensor values
struct SensorData {
    float nitrogen;
    float phosphorus;
    float potassium;
    float pH;
    float moisture;
    float temperature;
    float conductivity;
    int timestamp;
    bool isValid;
    uint8_t displayPage; // To track which page of data to show
};

// Function declarations
void setupOLED();
void displayTask(void *parameter);
void blinkLED(int times = 1, int duration = 100);
void loadConfig();
void startConfigMode();

// HTML page for configuration
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 NPK Sensor Config</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; margin: 20px; background: #f0f0f0; }
        .container { max-width: 500px; margin: auto; background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #333; text-align: center; }
        .form-group { margin-bottom: 20px; }
        label { display: block; margin-bottom: 5px; color: #555; font-weight: bold; }
        input { width: 100%; padding: 10px; border: 1px solid #ddd; border-radius: 5px; box-sizing: border-box; font-size: 14px; }
        button { width: 100%; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; font-weight: bold; }
        button:hover { background: #45a049; }
        .info { background: #e7f3fe; padding: 15px; border-radius: 5px; margin-bottom: 20px; border-left: 4px solid #2196F3; }
        .success { background: #d4edda; padding: 15px; border-radius: 5px; margin-top: 20px; border-left: 4px solid #28a745; display: none; }
    </style>
</head>
<body>
    <div class="container">
        <h1>NPK Sensor Setup</h1>
        <div class="info">
            <strong>Configuration Mode</strong><br>
            Fill in your WiFi and MQTT details below.
        </div>
        <form action="/save" method="POST">
            <div class="form-group">
                <label>WiFi SSID:</label>
                <input type="text" name="ssid" placeholder="Enter WiFi name" required>
            </div>
            <div class="form-group">
                <label>WiFi Password:</label>
                <input type="password" name="password" placeholder="Enter WiFi password" required>
            </div>
            <div class="form-group">
                <label>MQTT Server:</label>
                <input type="text" name="mqtt_server" placeholder="e.g., denodev.duckdns.org" required>
            </div>
            <div class="form-group">
                <label>MQTT Port:</label>
                <input type="number" name="mqtt_port" value="1883" required>
            </div>
            <div class="form-group">
                <label>MQTT Topic:</label>
                <input type="text" name="mqtt_topic" value="sensors/data" required>
            </div>
            <div class="form-group">
                <label>Device ID:</label>
                <input type="text" name="device_id" value="device01" required>
            </div>
            <div class="form-group">
                <label>MQTT Username (optional):</label>
                <input type="text" name="mqtt_user" placeholder="Leave blank if not required">
            </div>
            <div class="form-group">
                <label>MQTT Password (optional):</label>
                <input type="password" name="mqtt_pass" placeholder="Leave blank if not required">
            </div>
            <button type="submit">Save & Restart</button>
        </form>
        <div class="success" id="success">
            Configuration saved! Device will restart in 3 seconds...
        </div>
    </div>
</body>
</html>
)rawliteral";

// Handle root page
void handleRoot() {
  server.send(200, "text/html", index_html);
}

// Handle save configuration
void handleSave() {
  wifi_ssid = server.arg("ssid");
  wifi_password = server.arg("password");
  mqtt_server = server.arg("mqtt_server");
  mqtt_port = server.arg("mqtt_port");
  mqtt_topic = server.arg("mqtt_topic");
  device_id = server.arg("device_id");
  mqtt_user = server.arg("mqtt_user");
  mqtt_pass = server.arg("mqtt_pass");
  
  // Save to preferences
  preferences.begin("npk-config", false);
  preferences.putString("ssid", wifi_ssid);
  preferences.putString("password", wifi_password);
  preferences.putString("mqtt_server", mqtt_server);
  preferences.putString("mqtt_port", mqtt_port);
  preferences.putString("mqtt_topic", mqtt_topic);
  preferences.putString("device_id", device_id);
  preferences.putString("mqtt_user", mqtt_user);
  preferences.putString("mqtt_pass", mqtt_pass);
  preferences.putBool("configured", true);
  preferences.end();
  
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;text-align:center;padding:50px;background:#f0f0f0;}";
  html += ".msg{background:white;padding:30px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1);max-width:400px;margin:auto;}";
  html += "h1{color:#4CAF50;}</style></head><body><div class='msg'><h1>Saved!</h1>";
  html += "<p>Configuration saved successfully.<br>Device will restart in 3 seconds...</p></div>";
  html += "<script>setTimeout(function(){window.location.href='/';},3000);</script></body></html>";
  
  server.send(200, "text/html", html);
  
  delay(3000);
  ESP.restart();
}

// Load configuration from preferences
void loadConfig() {
  preferences.begin("npk-config", true);
  wifi_ssid = preferences.getString("ssid", "");
  wifi_password = preferences.getString("password", "");
  mqtt_server = preferences.getString("mqtt_server", "denodev.duckdns.org");
  mqtt_port = preferences.getString("mqtt_port", "1883");
  mqtt_topic = preferences.getString("mqtt_topic", "sensors/data");
  device_id = preferences.getString("device_id", "device01");
  mqtt_user = preferences.getString("mqtt_user", "");
  mqtt_pass = preferences.getString("mqtt_pass", "");
  bool configured = preferences.getBool("configured", false);
  preferences.end();
  
  if (!configured || wifi_ssid == "") {
    configMode = true;
  }
}

// Start AP mode for configuration
void startConfigMode() {
  Serial.println("Starting Configuration Mode...");
  
  // Show config mode on OLED
  display.clearDisplay();
  display.setCursor(0,0);
  display.println(F("Config Mode"));
  display.println(F(""));
  display.println(F("WiFi:"));
  display.println(F("NPK-Sensor-AP"));
  display.println(F("Pass: 12345678"));
  display.println(F(""));
  display.println(F("Go to:"));
  display.println(F("192.168.4.1"));
  display.display();
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP("NPK-Sensor-AP", "12345678");
  
  Serial.println("Access Point Started");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  Serial.println("Connect to WiFi: NPK-Sensor-AP");
  Serial.println("Password: 12345678");
  Serial.println("Then open browser to: http://192.168.4.1");
  
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  
  // Non-blocking config mode with timeout
  unsigned long startTime = millis();
  unsigned long timeout = 300000; // 5 minutes timeout
  
  while (configMode) {
    server.handleClient();
    yield(); // Feed watchdog
    
    // Blink LED to indicate config mode
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
    
    // Auto-exit config mode after timeout if not configured
    if (millis() - startTime > timeout) {
      Serial.println("Config timeout - restarting...");
      ESP.restart();
    }
  }
}

// Function to calculate CRC16 Modbus
uint16_t calculateCRC16(byte* data, int length) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// Function to read a specific parameter
bool readParameter(const byte* command, float &value, const char* paramName) {
    byte responseBuffer[8];
    bool success = false;
    
    if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {  // 1 second timeout for mutex
        digitalWrite(RTS_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(10));
        
        // Clear any existing data in the buffer
        while(RS485Serial.available()) {
            RS485Serial.read();
        }
        
        RS485Serial.write(command, commandLength);
        RS485Serial.flush();
        digitalWrite(RTS_PIN, LOW);
        
        unsigned long startTime = millis();
        bool timeout = false;
        
        // Wait for response with timeout
        while (RS485Serial.available() < 7) {
            if (millis() - startTime > 1000) {  // 1 second timeout
                timeout = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));  // Give other tasks a chance to run
        }
        
        if (!timeout && RS485Serial.available() >= 7) {
            int bytesRead = RS485Serial.readBytes(responseBuffer, 7);
            if (bytesRead == 7) {
                uint16_t rawValue = (responseBuffer[3] << 8) | responseBuffer[4];
                
                // Special handling for pH values
                if (command == readPH) {
                    value = rawValue / 100.0; // Divide by 100 instead of 10 for pH
                    // Sanity check for pH values
                    if (value < 0 || value > 14) {
                        Serial.println("Warning: pH value out of range!");
                        success = false;
                    } else {
                        success = true;
                    }
                } else {
                    value = rawValue / 10.0; // Normal scaling for other parameters
                    success = true;
                }
                
                Serial.printf("%s: %.1f\n", paramName, value);
            }
        } else {
            Serial.printf("Timeout or error reading %s\n", paramName);
        }
        
        xSemaphoreGive(serialMutex);
    }
    
    return success;
}

// LED Flash function
void flashLED() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, HIGH);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    digitalWrite(LED_PIN, LOW);
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// Task to handle MQTT communications
void mqttTask(void *parameter) {
    Serial.println("MQTT Task started");
    
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    
    client.setServer(mqtt_server.c_str(), mqtt_port.toInt());
    client.setKeepAlive(60); // Set keepalive to 60 seconds
    client.setSocketTimeout(10); // Set socket timeout to 10 seconds
    
    SensorData sensorData;
    
    for (;;) {
        // Non-blocking reconnection with timeout
        if (!client.connected()) {
            Serial.print("Attempting MQTT connection...");
            String clientId = "ESP32Client-";
            clientId += String(random(0xffff), HEX);
            
            bool connected = false;
            if (mqtt_user != "" && mqtt_pass != "") {
                connected = client.connect(clientId.c_str(), mqtt_user.c_str(), mqtt_pass.c_str());
            } else {
                connected = client.connect(clientId.c_str());
            }
            
            if (connected) {
                Serial.println("connected");
            } else {
                Serial.print("failed, rc=");
                Serial.print(client.state());
                Serial.println(" retry in 5 seconds");
                vTaskDelay(5000 / portTICK_PERIOD_MS);
                yield(); // Feed watchdog
                continue;
            }
        }
        
        // Non-blocking MQTT loop
        client.loop();
        
        if (xQueueReceive(mqttQueue, &sensorData, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (sensorData.isValid) {
                // Create JSON document matching simulator format
                StaticJsonDocument<512> doc;
                
                doc["sensor_id"] = device_id;
                
                JsonObject sensorDataObj = doc.createNestedObject("sensor_data");
                sensorDataObj["temperature"] = round(sensorData.temperature * 100.0) / 100.0;
                sensorDataObj["moisture"] = round(sensorData.moisture * 100.0) / 100.0;
                sensorDataObj["Ph"] = round(sensorData.pH * 100.0) / 100.0;
                sensorDataObj["Ec"] = round(sensorData.conductivity * 100.0) / 100.0;
                sensorDataObj["Nitrogen"] = round(sensorData.nitrogen * 100.0) / 100.0;
                sensorDataObj["Phosphorus"] = round(sensorData.phosphorus * 100.0) / 100.0;
                sensorDataObj["Potassium"] = round(sensorData.potassium * 100.0) / 100.0;
                
                char jsonBuffer[512];
                serializeJson(doc, jsonBuffer);
                
                // Publish with connection check
                if (client.connected()) {
                    bool published = client.publish(mqtt_topic.c_str(), jsonBuffer);
                    
                    if (published) {
                        Serial.println("--------------------------------------------------------");
                        Serial.print("Temperature: "); Serial.println(sensorData.temperature);
                        Serial.print("Moisture: "); Serial.println(sensorData.moisture);
                        Serial.print("pH: "); Serial.println(sensorData.pH);
                        Serial.print("EC: "); Serial.println(sensorData.conductivity);
                        Serial.print("Nitrogen: "); Serial.println(sensorData.nitrogen);
                        Serial.print("Phosphorus: "); Serial.println(sensorData.phosphorus);
                        Serial.print("Potassium: "); Serial.println(sensorData.potassium);
                        Serial.println("MQTT Published Successfully!");
                        Serial.println("--------------------------------------------------------");
                        
                        flashLED();
                    } else {
                        Serial.println("MQTT publish failed");
                    }
                }
            }
        }
        
        // Feed watchdog
        yield();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// Task to read sensor data
void sensorTask(void *parameter) {
    Serial.println("Sensor Task started");
    
    const int MAX_RETRIES = 3;
    const int RETRY_DELAY = 1000;  // 1 second between retries
    
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    for (;;) {
        SensorData sensorData;
        sensorData.isValid = false;
        int retryCount = 0;
        bool readSuccess = false;
        
        do {
            bool success = true;
            
            // Try to read each parameter
            success &= readParameter(readTemp, sensorData.temperature, "Temperature");
            vTaskDelay(pdMS_TO_TICKS(100));
            
            if (!success) {
                retryCount++;
                Serial.printf("Retry %d of %d for sensor readings\n", retryCount, MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY));
                continue;
            }
            
            success &= readParameter(readMoisture, sensorData.moisture, "Moisture");
            vTaskDelay(pdMS_TO_TICKS(100));
            success &= readParameter(readPH, sensorData.pH, "pH");
            vTaskDelay(pdMS_TO_TICKS(100));
            success &= readParameter(readEC, sensorData.conductivity, "Conductivity");
            vTaskDelay(pdMS_TO_TICKS(100));
            success &= readParameter(readN, sensorData.nitrogen, "Nitrogen");
            vTaskDelay(pdMS_TO_TICKS(100));
            success &= readParameter(readP, sensorData.phosphorus, "Phosphorus");
            vTaskDelay(pdMS_TO_TICKS(100));
            success &= readParameter(readK, sensorData.potassium, "Potassium");
            
            if (success) {
                sensorData.isValid = true;
                readSuccess = true;
                retryCount = 0;
                xQueueSend(sensorQueue, &sensorData, 0);
                xQueueSend(mqttQueue, &sensorData, 0);
                blinkLED(1, 50);
            } else {
                retryCount++;
                Serial.printf("Retry %d of %d for sensor readings\n", retryCount, MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY));
            }
            
        } while (!readSuccess && retryCount < MAX_RETRIES);
        
        if (!readSuccess) {
            Serial.println("Failed to read sensor after maximum retries");
            SensorData errorData;
            errorData.isValid = false;
            xQueueSend(sensorQueue, &errorData, 0);
        }
        
        // Feed watchdog
        yield();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// WiFi Task - maintains WiFi connection (non-blocking)
void TaskWiFi(void *pvParameters) {
    Serial.println("WiFi Task started");
    
    // Connect to WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());
    
    Serial.print("Connecting to WiFi");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        Serial.print(".");
        attempts++;
        // Feed watchdog
        yield();
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        
        // Show WiFi connected on OLED
        display.clearDisplay();
        display.setCursor(0,0);
        display.println(F("WiFi Connected!"));
        display.println(WiFi.localIP());
        display.display();
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    } else {
        Serial.println("\nWiFi connection failed! Restarting...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        ESP.restart();
    }
    
    // Monitor WiFi connection (non-blocking)
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi disconnected. Reconnecting...");
            WiFi.reconnect();
            
            // Wait max 20 seconds for reconnection
            int reconnectAttempts = 0;
            while (WiFi.status() != WL_CONNECTED && reconnectAttempts < 40) {
                vTaskDelay(500 / portTICK_PERIOD_MS);
                reconnectAttempts++;
                yield();
            }
            
            if (WiFi.status() == WL_CONNECTED) {
                Serial.println("WiFi reconnected");
            } else {
                Serial.println("WiFi reconnection failed. Restarting ESP32...");
                vTaskDelay(2000 / portTICK_PERIOD_MS);
                ESP.restart();
            }
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS); // Check every 5 seconds
    }
}

// Function to initialize OLED
void setupOLED() {
    Wire.begin(OLED_SDA, OLED_SCL);
    
    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        return;
    }
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.cp437(true);
    
    // Show initial message
    display.clearDisplay();
    display.setCursor(0,0);
    display.println(F("NPK Sensor"));
    display.println(F("Initializing..."));
    display.display();
}

// Task to display sensor data
void displayTask(void *parameter) {
    Serial.println("Display Task started");
    
    SensorData sensorData;
    unsigned long lastPageChange = 0;
    const unsigned long PAGE_DURATION = 3000;
    uint8_t currentPage = 0;
    
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    
    for (;;) {
        if (xQueueReceive(sensorQueue, &sensorData, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (!sensorData.isValid) {
                // Show error message on OLED
                display.clearDisplay();
                display.setCursor(0,0);
                display.setTextSize(1);
                display.println("Sensor Error!");
                display.println("Check connection");
                display.println("Retrying...");
                display.display();
                
                Serial.println("Sensor disconnected or not responding");
                continue;
            }
            
            // Update OLED display
            if (millis() - lastPageChange >= PAGE_DURATION) {
                lastPageChange = millis();
                currentPage = (currentPage + 1) % 3;  // 3 pages total
                
                display.clearDisplay();
                display.setCursor(0,0);
                display.setTextSize(1);
                
                switch(currentPage) {
                    case 0:
                        // Page 1: Temperature, Moisture, pH
                        display.println("Soil Conditions:");
                        display.printf("Temp: %.1fC\n", sensorData.temperature);
                        display.printf("Moist: %.1f%%\n", sensorData.moisture);
                        display.printf("pH: %s\n", String(sensorData.pH, 1).c_str());
                        display.printf("EC: %.1f us/cm\n", sensorData.conductivity);
                        break;
                        
                    case 1:
                        // Page 2: NPK values
                        display.println("NPK Values:");
                        display.printf("N: %.1f mg/kg\n", sensorData.nitrogen);
                        display.printf("P: %.1f mg/kg\n", sensorData.phosphorus);
                        display.printf("K: %.1f mg/kg\n", sensorData.potassium);
                        break;
                        
                    case 2:
                        // Page 3: Network Status
                        display.println("Network Status:");
                        display.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
                        display.printf("MQTT: %s\n", client.connected() ? "Connected" : "Disconnected");
                        display.printf("RSSI: %d dBm\n", WiFi.RSSI());
                        break;
                }
                
                // Show device ID
                display.setCursor(0, 56);
                display.printf("Device: %s", device_id.c_str());
                
                display.display();
            }
        }
        
        // Feed watchdog
        yield();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// LED blink function
void blinkLED(int times, int duration) {
    for(int i = 0; i < times; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(duration);
        digitalWrite(LED_PIN, LOW);
        if(i < times - 1) {
            delay(duration);
        }
    }
}

void setup() {
    // Initialize LED pin
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);  // Start with LED off

    // Initialize debug serial
    Serial.begin(115200);
    delay(1000); // Wait for serial to stabilize
    Serial.println("\n\nESP32 NPK Sensor with RTOS");

    // Initialize OLED
    setupOLED();
    
    pinMode(CONFIG_BUTTON, INPUT_PULLUP);
    
    // Load saved configuration first
    loadConfig();
    
    // Check if config button is pressed AFTER boot (give 3 seconds window)
    Serial.println("Press and hold BOOT button for 3 seconds to enter config mode...");
    bool buttonPressed = false;
    for (int i = 0; i < 30; i++) {  // Check for 3 seconds
        if (digitalRead(CONFIG_BUTTON) == LOW) {
            buttonPressed = true;
            delay(100);
        } else if (buttonPressed) {
            buttonPressed = false;  // Button was released early
        }
        delay(100);
    }
    
    if (buttonPressed && digitalRead(CONFIG_BUTTON) == LOW) {
        Serial.println("Config button held - entering configuration mode");
        preferences.begin("npk-config", false);
        preferences.putBool("configured", false);
        preferences.end();
        configMode = true;
    }
    
    // If not configured, enter config mode
    if (configMode) {
        startConfigMode();
        return;
    }
    
    // Initialize RS485 serial
    RS485Serial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
    
    // Configure RTS pin for flow control
    pinMode(RTS_PIN, OUTPUT);
    digitalWrite(RTS_PIN, LOW);  // Set to receive mode by default
    
    // Create mutex for serial communication
    serialMutex = xSemaphoreCreateMutex();
    
    // Create queue
    sensorQueue = xQueueCreate(5, sizeof(SensorData));
    mqttQueue = xQueueCreate(5, sizeof(SensorData));
    
    // Create tasks
    xTaskCreatePinnedToCore(TaskWiFi, "WiFi", 4096, NULL, 1, &TaskWiFiHandle, 0);
    xTaskCreatePinnedToCore(sensorTask, "Sensor", 4096, NULL, 2, &sensorTaskHandle, 1);
    xTaskCreatePinnedToCore(displayTask, "Display", 4096, NULL, 1, &displayTaskHandle, 1);
    xTaskCreatePinnedToCore(mqttTask, "MQTT", 8192, NULL, 1, &mqttTaskHandle, 0);
    
    Serial.println("All tasks created successfully");
    Serial.println("To reconfigure: Hold BOOT button and press RESET");
    
    // Show ready message on OLED
    display.clearDisplay();
    display.setCursor(0,0);
    display.println(F("System Ready"));
    display.println(F("Reading sensors..."));
    display.display();
}

void loop() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}
