#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// WiFi credentials
const char* ssid = "norazlin@unifi";
const char* password = "bkh223811286";

// MQTT Broker settings
const char* mqtt_server = "test.mosquitto.org";
const int mqtt_port = 1883;
const char* mqtt_user = "";
const char* mqtt_password = "";
const char* mqtt_topic = "sensors/data";
const char* device_id = "device01";  // Device identifier

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
PubSubClient mqttClient(espClient);

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
void setupWiFi();
void setupOLED();
void reconnectMQTT();
void displayTask(void *parameter);

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
    
    if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
        digitalWrite(RTS_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(10));
        RS485Serial.write(command, commandLength);
        RS485Serial.flush();
        digitalWrite(RTS_PIN, LOW);
        
        unsigned long startTime = millis();
        while (RS485Serial.available() < 7 && (millis() - startTime) < 1000) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        if (RS485Serial.available() >= 7) {
            int bytesRead = RS485Serial.readBytes(responseBuffer, 7);
            if (bytesRead == 7) {
                uint16_t receivedCRC = (responseBuffer[bytesRead-1] << 8) | responseBuffer[bytesRead-2];
                uint16_t calculatedCRC = calculateCRC16(responseBuffer, bytesRead-2);
                
                if (receivedCRC == calculatedCRC) {
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
            }
        }
        xSemaphoreGive(serialMutex);
    }
    return success;
}

// Task to handle MQTT communications
void mqttTask(void *parameter) {
    SensorData sensorData;
    
    while (1) {
        if (xQueueReceive(mqttQueue, &sensorData, portMAX_DELAY) == pdTRUE) {
            if (!mqttClient.connected()) {
                reconnectMQTT();
            }
            
            if (sensorData.isValid) {
                // Create JSON document matching simulator format
                StaticJsonDocument<512> doc;
                
                doc["sensor_id"] = device_id;
                
                JsonObject sensorDataObj = doc.createNestedObject("sensor_data");
                sensorDataObj["temperature"] = round(sensorData.temperature * 100.0) / 100.0;  // Round to 2 decimals
                sensorDataObj["moisture"] = round(sensorData.moisture * 100.0) / 100.0;
                sensorDataObj["Ph"] = round(sensorData.pH * 100.0) / 100.0;
                sensorDataObj["Ec"] = round(sensorData.conductivity * 100.0) / 100.0;
                sensorDataObj["Nitrogen"] = round(sensorData.nitrogen * 100.0) / 100.0;
                sensorDataObj["Phosphorus"] = round(sensorData.phosphorus * 100.0) / 100.0;
                sensorDataObj["Potassium"] = round(sensorData.potassium * 100.0) / 100.0;
                
                char jsonBuffer[512];
                serializeJson(doc, jsonBuffer);
                
                // Publish to MQTT
                mqttClient.publish(mqtt_topic, jsonBuffer);
                Serial.println("Published to MQTT:");
                Serial.println(jsonBuffer);
            }
        }
        mqttClient.loop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Task to read sensor data
void sensorTask(void *parameter) {
    while (1) {
        SensorData sensorData;
        sensorData.isValid = false;
        sensorData.timestamp = millis();
        
        bool success = true;
        success &= readParameter(readN, sensorData.nitrogen, "Nitrogen");
        vTaskDelay(pdMS_TO_TICKS(100));
        success &= readParameter(readP, sensorData.phosphorus, "Phosphorus");
        vTaskDelay(pdMS_TO_TICKS(100));
        success &= readParameter(readK, sensorData.potassium, "Potassium");
        vTaskDelay(pdMS_TO_TICKS(100));
        success &= readParameter(readTemp, sensorData.temperature, "Temperature");
        vTaskDelay(pdMS_TO_TICKS(100));
        success &= readParameter(readMoisture, sensorData.moisture, "Moisture");
        vTaskDelay(pdMS_TO_TICKS(100));
        success &= readParameter(readEC, sensorData.conductivity, "Conductivity");
        vTaskDelay(pdMS_TO_TICKS(100));
        success &= readParameter(readPH, sensorData.pH, "pH");
        
        sensorData.isValid = success;

        if (success) {
            // Send data to display queue
            xQueueSend(sensorQueue, &sensorData, 0);
            
            // Send data to MQTT queue
            xQueueSend(mqttQueue, &sensorData, 0);
        }
        
        // Wait before next reading
        vTaskDelay(pdMS_TO_TICKS(5000));  // 5 second delay
    }
}

void setupWiFi() {
    Serial.printf("Connecting to %s", ssid);
    WiFi.begin(ssid, password);
    
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    
    Serial.println("\nWiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

void reconnectMQTT() {
    while (!mqttClient.connected()) {
        Serial.print("Attempting MQTT connection...");
        String clientId = "ESP32Client-" + String(random(0xffff), HEX);
        
        if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
            Serial.println("connected");
        } else {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" retrying in 5 seconds");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
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
    SensorData sensorData;
    unsigned long lastPageChange = 0;
    const unsigned long PAGE_DURATION = 3000;  // 3 seconds per page
    uint8_t currentPage = 0;
    
    while (1) {
        if (xQueueReceive(sensorQueue, &sensorData, portMAX_DELAY) == pdTRUE) {
            if (xSemaphoreTake(serialMutex, portMAX_DELAY) == pdTRUE) {
                if (sensorData.isValid) {
                    Serial.println("\n=== Complete Sensor Reading ===");
                    Serial.printf("Nitrogen: %.1f mg/kg\n", sensorData.nitrogen);
                    Serial.printf("Phosphorus: %.1f mg/kg\n", sensorData.phosphorus);
                    Serial.printf("Potassium: %.1f mg/kg\n", sensorData.potassium);
                    Serial.printf("pH: %.1f\n", sensorData.pH);
                    Serial.printf("Moisture: %.1f %%\n", sensorData.moisture);
                    Serial.printf("Temperature: %.1f °C\n", sensorData.temperature);
                    Serial.printf("Conductivity: %.1f us/cm\n", sensorData.conductivity);
                    Serial.println("==============================\n");
                } else {
                    Serial.println("Failed to read complete sensor data!");
                }
                xSemaphoreGive(serialMutex);
            }
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
                    display.printf("pH: %.1f\n", sensorData.pH);
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
                    display.printf("MQTT: %s\n", mqttClient.connected() ? "Connected" : "Disconnected");
                    display.printf("RSSI: %d dBm\n", WiFi.RSSI());
                    break;
            }
            
            // Show update time
            display.setCursor(0, 56);
            display.printf("Last: %d sec ago", (millis() - lastPageChange) / 1000);
            
            display.display();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void setup() {
    // Initialize debug serial
    Serial.begin(115200);

    // Initialize OLED
    setupOLED();
    
    // Initialize RS485 serial
    RS485Serial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
    
    // Configure RTS pin for flow control
    pinMode(RTS_PIN, OUTPUT);
    digitalWrite(RTS_PIN, LOW);  // Set to receive mode by default

    // Setup WiFi
    setupWiFi();

    // Setup MQTT
    mqttClient.setServer(mqtt_server, mqtt_port);
    
    // Create mutex for serial communication
    serialMutex = xSemaphoreCreateMutex();
    
    // Create queue
    sensorQueue = xQueueCreate(5, sizeof(SensorData));
    mqttQueue = xQueueCreate(5, sizeof(SensorData));
    
    // Create tasks
    xTaskCreatePinnedToCore(
        sensorTask,          // Task function
        "SensorTask",        // Name
        4096,               // Stack size
        NULL,               // Parameters
        2,                  // Priority
        &sensorTaskHandle,  // Task handle
        0                   // Core ID (0)
    );
    
    xTaskCreatePinnedToCore(
        displayTask,         // Task function
        "DisplayTask",       // Name
        4096,               // Stack size
        NULL,               // Parameters
        1,                  // Priority
        &displayTaskHandle, // Task handle
        1                   // Core ID (1)
    );

    xTaskCreatePinnedToCore(
        mqttTask,           // Task function
        "MQTTTask",         // Name
        4096,               // Stack size
        NULL,               // Parameters
        1,                  // Priority
        &mqttTaskHandle,    // Task handle
        1                   // Core ID (1)
    );
    
    // Show ready message on OLED
    display.clearDisplay();
    display.setCursor(0,0);
    display.println(F("System Ready"));
    display.println(F("Reading sensors..."));
    display.display();
    
    Serial.println("ESP32 NPK Sensor Reader Started");
}

void loop() {
    // Empty loop - tasks handle everything
    vTaskDelay(pdMS_TO_TICKS(1000));
}