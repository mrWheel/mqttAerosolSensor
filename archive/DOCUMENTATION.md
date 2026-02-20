# Multi-Device MQTT Project (ESP32, RTOS)

- Meerdere devices (luchtsensor + ventilatorregelaar)
- Gedeelde WiFi/MQTT configuratie via WifiManagerExt + MqttClient
- Allman style, lowerCamelCase, Object Calisthenics-achtig opgezet
- Indent 2 positions
- FreeRTOS taken per device
- Pins en topics via build_flags

Zie `documentation/mqtt-architecture-diagram.txt` voor een ASCII overzicht.


Logging: zie shared/logger.* voor centraal gebruik van Serial.printf().
