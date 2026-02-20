# luchtSensor

A firmware project for **ESP32 DevKit boards** using **PlatformIO** and the **Arduino framework**.

## Overview

This project manages ESP32 firmware application for a luchtSensor:

### Air Sensor (Luchtsensor)
   - Measures particulate matter using PMSerial sensor
   - Publishes periodic measurements via MQTT
   
Code:
- Use **FreeRTOS** for task management
- Configure WiFi and MQTT via **WiFiManagerExt** captive portal
- Support **JSON** via ArduinoJson
- Feature a button (active within 30s after boot) to clear WiFi credentials
- Implement comprehensive logging via the **Logger** utility

## Architecture

```
┌────────────────────┐
│   MQTT Broker      │
└────────┬───────────┘
         │
         ▼                                        
┌─────────────────────┐  
│   Air Sensor        │  
│  (ESP32-Dev)        │  
│─────────────────────│  
│ WiFiManagerExt      │  
│ MqttClient          │  
│ pms5003AirSensor    |  
|           (PMSerial)│  
│─────────────────────│  
│ Publishes:          │  
│  - luchtsensor/data │  
└─────────────────────┘  
```

## Project Structure

```
luchtSensor/
├── platformio.ini              # PlatformIO configuratie
├── .gitignore                  # Git exclusions
├── DOCUMENTATION.md            # Projectdocumentatie
├── roadmap.md                  # Dit bestand
│
├── include/                    # Gedeelde code
│   ├── pms5003AirSensor.h      # Fijnstofsensor wrapper (PMSerial)
│   ├── logger.h                # Centrale logging utility
│   ├── mqttClient.h            # MQTT client wrapper (PubSubClient)
│   ├── mqttConfig.h            # MQTT configuratie storage
│   └── wifiManagerExt.h        # WiFi/MQTT configuratie manager
│
├── src/                    
│   ├── main.cpp                # Hoofd programma
│   ├── pms5003AirSensor.cpp    # Fijnstofsensor wrapper (PMSerial)
│   ├── logger.cpp              # Centrale logging utility
│   ├── mqttClient.cpp          # MQTT client wrapper (PubSubClient)
│   ├── mqttConfig.cpp          # MQTT configuratie storage
│   └── wifiManagerExt.cpp      # WiFi/MQTT configuratie manager
│
├── documentation/             # Extra documentatie
│   └── mqtt-architecture-diagram.txt
│
├── config_examples/           # Voorbeeld configuraties
│   └── multi_device_mqtt.yaml
│
└── .pio.nosync/              # Build artifacts (niet in git)
```

## Features

### include

#### Logger
Centralized logging utility with:
- Log levels: `INFO`, `WARN`, `ERROR`, `DEBUG`
- Automatic timestamps in milliseconds
- Printf-style formatting
- Consistent output across all modules

```cpp
Logger::info("Device started");
Logger::warn("Low memory");
Logger::error("Sensor read failed");
Logger::debug("Variable x = %d", x);
```

#### WiFiManagerExt
Extended WiFi configuration manager:
- Captive portal for WiFi and MQTT setup
- Stores credentials in LittleFS (`/config.json`)
- **PIN_ERASE_WIFI** monitoring during WiFi connection
- 60-second timeout with automatic restart

#### MqttClient
MQTT client wrapper:
- Simplified publish/subscribe with JSON support
- Automatic reconnection handling
- Built on PubSubClient library

#### Air Sensor
- Measures PM2.5, PM10 concentrations
- Configurable measurement interval (default: 2 minutes)
- Publishes to `luchtsensor/data` topic
- **PIN_ERASE_WIFI** clears credentials within 20 seconds of boot

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) installed
- ESP32 DevKit board
- USB cable for programming

### Building

The project uses environment-specific builds. Never build the default environment:

```bash
# Build air sensor firmware
pio run -e luchtsensor
```

### Uploading

```bash
# Upload air sensor
pio run -e luchtsensor --target upload
```

### Monitoring

```bash
# Monitor serial output (115200 baud)
pio device monitor -e luchtsensor
```

## Configuration

### First Boot

On first boot, the device will:
1. if no config.json found ór if there are not allready WiFi-credentiels;
   - Start a WiFi access point (AP mode) 
   - Display AP name on serial output
   - Open configuration portal at `192.168.4.1`

### Configuration Portal

Connect to the device's WiFi AP and navigate to the portal to configure:
- **WiFi SSID and password**
- **MQTT broker host** (IP or hostname)
- **MQTT port** (default: 1883)
- **MQTT username** (optional)
- **MQTT password** (optional)

Settings are saved to LittleFS and persist across reboots.

### PIN_ERASE_WIFI

To clear all WiF credentials:
1. Press and release the PIN_ERASE_WIFI button a few times during boot within 30 seconds after boot-start
2. Release button
3. Device will restart in configuration mode

## GPIO Pin Assignments

### Air Sensor

| Pin | Function | Notes |
|-----|----------|-------|
| GPIO16 | PM Sensor RX | UART receive from sensor |
| GPIO17 | PM Sensor TX | UART transmit to sensor |
| GPIOx | WiFi erase Button | Active within 20s of boot |


## MQTT Topics
**Publishes:**
- `luchtsensor/data` - Measurement data in JSON format

**Example payload:**
```json
{
  "deviceId": "LS609c3e08",
  "pm25": 8.7,
  "pm10": 12.3,
  "timestamp": 123456
}
```

**Fields:**
- `deviceId`: Unique device identifier based on MAC address (format: LS + last 8 hex digits of MAC)
- `pm25`: PM2.5 concentration in µg/m³
- `pm10`: PM10 concentration in µg/m³
- `timestamp`: Milliseconds since device boot

## FreeRTOS Tasks

| Task | Function |
|------|----------|
| taskButtonMonitor | Monitors PIN_ERASE_WIFI |
| taskMqttLoop | Maintains MQTT connection |
| taskMeasurement | Periodic sensor readings |

## Development

### PlatformIO Configuration

The project uses `platformio.ini`:

- **[env]**: Shared configuration
- **[env:luchtsensor]**: Air sensor specific settings

### Build Flags

Build flags define specific parameters:
- Device identifiers
- GPIO pin assignments
- Timing intervals
- MQTT topics

See `platformio.ini` for complete configuration details.

## Dependencies

### Core Libraries
- **WiFiManager** (^2.0.17) - WiFi configuration portal
- **PubSubClient** - MQTT client
- **ArduinoJson** (^7.0.4) - JSON parsing/serialization
- **FS & LittleFS** - Filesystem for configuration storage

### Device-Specific Libraries
- **PMSerial** (^1.2.0) - Particulate matter sensor (air sensor only)

## Contributing

Toine & Willem

## Support

For issues and questions, please use the GitHub issue tracker.
