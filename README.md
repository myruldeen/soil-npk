# ESP32 NPK Soil Sensor Reader

IoT soil monitoring system using ESP32 to read NPK (Nitrogen, Phosphorus, Potassium) values and other soil parameters from JXCT sensors via RS485.

## Features

- **Multi-Parameter Sensing**: NPK, pH, moisture, temperature, electrical conductivity
- **Real-time Display**: OLED screen with rotating pages
- **IoT Connectivity**: WiFi and MQTT integration
- **Robust Communication**: RS485 with error handling and retries
- **Multi-tasking**: FreeRTOS-based concurrent operations

## Hardware Requirements

- **ESP32 Development Board**
- **JXCT NPK Soil Sensor** (RS485 Modbus RTU)
- **128x64 OLED Display** (SSD1306 I2C)
- **RS485 to TTL Converter** (if needed)

### Pin Connections

| Component | ESP32 Pin | Description |
|-----------|-----------|-------------|
| RS485 RX | GPIO 16 | Receive data |
| RS485 TX | GPIO 17 | Transmit data |
| RS485 RTS | GPIO 4 | Flow control |
| OLED SDA | GPIO 21 | I2C data |
| OLED SCL | GPIO 22 | I2C clock |
| Status LED | GPIO 2 | Built-in LED |

## Quick Setup

### 1. Hardware Assembly
```
ESP32          RS485 Converter          JXCT Sensor
GPIO 16 ────── RX
GPIO 17 ────── TX
GPIO 4  ────── RTS
3.3V   ────── VCC
GND    ────── GND

ESP32          OLED Display
GPIO 21 ────── SDA
GPIO 22 ────── SCL
3.3V   ────── VCC
GND    ────── GND
```

### 2. Software Configuration
Edit `src/main.cpp`:
```cpp
// WiFi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// MQTT settings
const char* mqtt_server = "your.mqtt.broker.com";
const char* mqtt_user = "your_username";
const char* mqtt_password = "your_password";
const char* device_id = "device01";
```

### 3. Compile and Upload
```bash
# Using PlatformIO
pio run --target upload
pio device monitor
```

## System Architecture

### FreeRTOS Tasks
1. **Sensor Task** (Core 0): Reads sensor data every 5 seconds
2. **Display Task** (Core 1): Updates OLED with rotating pages
3. **MQTT Task** (Core 1): Publishes data to MQTT broker

### Data Flow
```
Sensor → RS485 → ESP32 → Processing → OLED Display
                    ↓
                MQTT Broker → Cloud/Application
```

## Usage

### Display Information
**Page 1: Soil Conditions**
- Temperature (°C), Moisture (%), pH, EC (μS/cm)

**Page 2: NPK Values**
- Nitrogen, Phosphorus, Potassium (mg/kg)

**Page 3: Network Status**
- WiFi/MQTT status, Signal strength

### LED Status
- **Single blink**: Successful sensor reading
- **Double blink**: Successful MQTT publish
- **Long blink**: MQTT publish failure

## MQTT Data Format

```json
{
  "sensor_id": "device01",
  "sensor_data": {
    "temperature": 25.5,
    "moisture": 45.2,
    "Ph": 6.8,
    "Ec": 1250.0,
    "Nitrogen": 150.0,
    "Phosphorus": 25.0,
    "Potassium": 200.0
  }
}
```

## Modbus RTU Commands

| Parameter | Register | Command Array |
|-----------|----------|---------------|
| Nitrogen | 0x001E | `{0x01, 0x03, 0x00, 0x1E, 0x00, 0x01, 0xE4, 0x0C}` |
| Phosphorus | 0x001F | `{0x01, 0x03, 0x00, 0x1F, 0x00, 0x01, 0xB5, 0xCC}` |
| Potassium | 0x0020 | `{0x01, 0x03, 0x00, 0x20, 0x00, 0x01, 0x85, 0xC0}` |
| Temperature | 0x0013 | `{0x01, 0x03, 0x00, 0x13, 0x00, 0x01, 0x75, 0xCF}` |
| Moisture | 0x0012 | `{0x01, 0x03, 0x00, 0x12, 0x00, 0x01, 0x24, 0x0F}` |
| pH | 0x0006 | `{0x01, 0x03, 0x00, 0x06, 0x00, 0x01, 0x64, 0x0B}` |
| EC | 0x0015 | `{0x01, 0x03, 0x00, 0x15, 0x00, 0x01, 0x95, 0xCE}` |

## Troubleshooting

### Common Issues

**Sensor Not Responding**
- Check RS485 wiring (RX, TX, RTS)
- Verify sensor power supply (12V)
- Check sensor address (default: 0x01)
- Ensure baud rate (9600)

**WiFi Connection Failed**
- Verify SSID and password
- Check WiFi signal strength
- Ensure 2.4GHz network

**MQTT Connection Failed**
- Verify broker address and port
- Check username/password
- Ensure network connectivity

**OLED Display Issues**
- Check I2C connections (SDA, SCL)
- Verify display address (0x3C)
- Ensure proper power supply

## Configuration

### Timing Constants
```cpp
const int MAX_RETRIES = 3;                    // Sensor retry attempts
const int RETRY_DELAY = 1000;                 // Retry delay (ms)
const unsigned long PAGE_DURATION = 3000;     // Display page duration (ms)
```

### Communication Parameters
```cpp
const int mqtt_port = 1883;                   // MQTT broker port
const char* device_id = "device01";           // Device identifier
```

## Dependencies

### Required Libraries
- PubSubClient
- ArduinoJson
- Adafruit GFX Library
- Adafruit SSD1306
- FreeRTOS (built-in)

### PlatformIO Configuration
```ini
[env:esp32doit-devkit-v1]
platform = espressif32
board = esp32doit-devkit-v1
framework = arduino
monitor_speed = 115200
lib_deps =
    knolleary/PubSubClient @ ^2.8
    bblanchon/ArduinoJson @ ^6.21.3
    adafruit/Adafruit SSD1306 @ ^2.5.7
    adafruit/Adafruit GFX Library @ ^1.11.5
```

## Performance

- **Sensor Reading**: Every 5 seconds
- **Display Update**: 3 seconds per page
- **MQTT Loop**: 100ms interval
- **Memory Usage**: ~30-40% of ESP32 RAM
- **Power Consumption**: ~130-150mA during operation

## API Reference

### Key Functions
- `readParameter()`: Read sensor parameter via RS485
- `setupWiFi()`: Initialize WiFi connection
- `setupOLED()`: Initialize OLED display
- `reconnectMQTT()`: Establish MQTT connection
- `blinkLED()`: Control status LED

### Data Structure
```cpp
struct SensorData {
    float nitrogen, phosphorus, potassium;
    float pH, moisture, temperature, conductivity;
    int timestamp;
    bool isValid;
    uint8_t displayPage;
};
```

## License

This project is open source. Please refer to the license file for details.

## Support

For technical support:
1. Check the troubleshooting section
2. Review the code comments
3. Open an issue on the project repository

---

**Note**: This documentation assumes familiarity with Arduino/ESP32 development and basic electronics. 