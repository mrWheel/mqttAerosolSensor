
# 🧠 Projectbeschrijving — luchtSensor

Een firmware project voor **ESP32-dev boards** met **PlatformIO** en **Arduino framework**.

## codingRules

Lees codingRules.md

---

## 🔹 Doel

Code ontwikkeling voor de luchtSensor firmwares ontwikkelen:

**Luchtsensor**
   - Meet fijnstof via **PMSerial**.
   - Publiceert periodiek via **MQTT**.

De code:
- Gebruikt **FreeRTOS**.
- Configureert WiFi en MQTT via **WiFiManagerExt**.
- Ondersteunt **JSON** via **ArduinoJson**.
- Heeft een eraseknop (werkt binnen 30s na opstart) om WiFi-credentials te wissen.
- Loggt uitvoerig met `Serial.printf()` via de **Logger** utility.

---

## 🔹 PlatformIO configuratie

### platformio.ini
```
[platformio]
workspace_dir = .pio.nosync
default_envs = mqttAerosolSensor

[env]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
lib_ldf_mode = deep
lib_archive = false
lib_deps = 
    tzapu/WiFiManager @ ^2.0.17
    knolleary/PubSubClient
    FS
    LittleFS
    bblanchon/ArduinoJson @ ^7.0.4
    https://github.com/fu-hsi/PMS.git

[env:mqttAerosolSensor]
build_flags =
    -DPIN_PM_RX=16
    -DPIN_PM_TX=17
    -DPIN_PM_ENABLE=4
    -DPIN_PM_RESET=13
    -DPIN_ERASE_WIFI=0
    -DPM_NEVER_SLEEPS
    -DTOPIC_DATA="\"luchtsensor/data\""
    -DLOG_LEVEL=LOG_LEVEL_DEBUG
```

Gebruik: `pio run -e mqttAerosolSensor`

---

## 🔹 Mappenstructuur

```
mqttAerosolSensor/
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

### Directory details

#### luchtsensor/
Device-specifieke code voor de luchtsensor:
- `main.cpp`: Hoofdapplicatie met FreeRTOS taken voor metingen en MQTT

#### include/
Header (.h) files voor herbruikbare modules:

**airSensor.cpp/.h**
- Wrapper voor PMSerial library en PMS sensor hardware
- Meet PM1.0, PM2.5, PM10 concentraties (atmospheric environment µg/m³)
- Gebruikt voor luchtsensor device

**Functionaliteit:**
- `begin()`: Initialiseer UART communicatie en optionele RESET pin
- `reset()`: Hardware reset via PIN_PM_RESET (LOW pulse van 100ms)
- `wakeUp()`: Stuur wakeup commando naar sensor
- `sleep()`: Zet sensor in sleep mode voor stroomverbruik besparing
- `read()`: Lees sensor data met 5 seconden timeout en RTOS-friendly polling
- `pm1()`, `pm25()`, `pm10()`: Haal laatste gemeten waardes op
- `getLastData()`: Verkrijg complete PMS::DATA structuur

**Hardware pins:**
- `PIN_PM_RESET`: Optionele hardware reset pin (default -1, sensor heeft interne pull-up)
- Wanneer gedefinieerd: Configured als OUTPUT, normaal HIGH, reset door LOW pulse
- `PIN_PM_ENABLE`: Hardware enable pin, sensor heeft interne pull-up

**logger.cpp/.h**
- Centrale logging utility (zie volgende sectie)
- Gebruikt door alle modules

**mqttClient.cpp/.h**
- Wrapper voor PubSubClient library
- Vereenvoudigt MQTT publish/subscribe met JSON
- Automatische reconnection handling

**mqttConfig.cpp/.h**
- Storage class voor MQTT instellingen (host, port, user, pass)
- Gebruikt door wifiManagerExt en mqttClient

**telnetServer.cpp/.h**
- Telnet server voor remote console toegang
- Single-connection mode: maximaal 1 actieve client tegelijk
- Automatische disconnectie van oude sessie bij nieuwe connectie
- Gebruikt voor remote debugging en device management

**Functionaliteit:**
- `begin()`: Start telnet server op poort 23
- `stop()`: Stop server en disconnect alle clients
- `loop()`: Hoofdloop voor client handling (roep aan in RTOS task)
- `hasClient()`: Check of client verbonden is
- `print()`, `println()`, `printf()`: Output naar telnet client
- `setCommandCallback()`: Registreer custom command handler

**Single-Connection Gedrag:**
- Bij nieuwe connectie wordt oude client automatisch gedisconnect
- Oude client ontvangt notificatie voordat sessie wordt gesloten
- "Last connection wins" principe: nieuwste client krijgt altijd toegang
- Oude sessie krijgt bericht:
  ```
  ===================================
    New connection detected!
    Your session will be closed.
  ===================================
  ```

**Built-in Commands:**
- `help`: Toon beschikbare commando's
- `status`: Toon device status (uptime, memory, WiFi RSSI)
- `clear`: Clear screen (VT100 escape codes)
- Custom commands via callback functie

**Integratie met Logger:**
- Logger output kan naar telnet client worden gestuurd
- `Logger::setTelnetServer(&telnetServer)` voor telnet logging
- Logs worden naar zowel Serial als Telnet gestuurd

**mDNS Service:**
- Geadverteerd als `_telnet._tcp` op poort 23
- Toegankelijk via: `telnet sensor-XXXX.local`

**wifiManagerExt.cpp/.h**
- Uitbreiding op WiFiManager library
- Configuratie portal met MQTT velden
- Opslag van credentials in LittleFS (/config.json)
- Reset button monitoring tijdens WiFi connectie
- 60 seconden timeout met automatische restart
- **MAC-gebaseerde identificatie en mDNS**

**MAC Address Identificatie:**
- Leest ESP32 base MAC address via `esp_read_mac()`
- Genereert twee formats:
  - Volledig: Laatste 4 bytes (8 hex karakters) voor MQTT Client ID
  - Kort: Laatste 2 bytes (4 hex karakters) voor WiFi AP en mDNS namen

**WiFi Access Point namen:**
- Luchtsensor: `sensor-XXXX` (bijv. "sensor-3c4d")

**MQTT Client IDs:**
- Luchtsensor: `LS` + volledig MAC suffix (bijv. "LS1a2b3c4d")
- Toegankelijk via `wifiManager.getClientId()`

**mDNS (Multicast DNS):**
- Automatisch gestart na succesvolle WiFi verbinding
- Hostname format: `<device>-<kort_mac>.local`
- Luchtsensor: `sensor-XXXX.local` (bijv. "sensor-3c4d.local")
- Geadverteerde services:
  - HTTP op poort 80 (`_http._tcp`)
  - Telnet op poort 23 (`_telnet._tcp`)
- Gebruik: `ping sensor-3c4d.local`

**Functies:**
- `initMacAddress()`: Initialiseer MAC address en genereer IDs
- `startMDNS()`: Start mDNS service met hostname
- `getMacSuffix()`: Verkrijg volledig MAC suffix
- `getClientId()`: Verkrijg MQTT client ID

#### documentation/
Extra projectdocumentatie en diagrammen

#### config_examples/
Voorbeeld configuraties voor diverse setups

---

## 🔹 Centrale logging (logger.*)

### Overzicht

De `Logger` utility biedt een centrale, gestructureerde manier om log messages te schrijven naar de seriële poort. Alle modules in het project gebruiken dezelfde logging interface voor consistente output.

### Kenmerken

**Log levels:**
- `INFO`: Normale operationele berichten (altijd zichtbaar)
- `WARN`: Waarschuwingen die aandacht vereisen
- `ERROR`: Foutmeldingen
- `DEBUG`: Gedetailleerde debug informatie (kan uit worden gezet)

**Timestamp formatting:**
- Automatische timestamps in formaat: `[12345]` (milliseconden sinds boot)
- Helpt bij timing analysis en debugging

**Type indicators:**
- `[INFO]`, `[WARN]`, `[ERROR]`, `[DEBUG]` prefixes
- Makkelijk te filteren in serial output

**Printf-style formatting:**
- Ondersteunt variabelen met format specifiers
- Bijvoorbeeld: `Logger::info("Speed: %.1f", speed);`

### Gebruik

**Include in je code:**
```cpp
#include "logger.h"
```

**Basis logging:**
```cpp
Logger::info("Device started");
Logger::warn("Low memory");
Logger::error("Sensor read failed");
Logger::debug("Variable x = %d", x);
```

**Met variabelen:**
```cpp
float temperature = 23.5;
int count = 42;
Logger::info("Temperature: %.1f°C, Count: %d", temperature, count);
```

**String variabelen:**
```cpp
const char* status = "connected";
Logger::info("MQTT status: %s", status);
```

### Output voorbeeld

```
[  1234] [INFO] Booting Ventilatorregelaar...
[  1567] [INFO] taskButtonMonitor started, initial PIN_ERASE_WIFI state: HIGH
[  1589] [INFO] Loaded MQTT config from file:
[  1590] [INFO]   Host: 'mqtt.local'
[  1591] [INFO]   Port: '1883'
[  2345] [INFO] Starting WiFi connection attempt (60 second timeout)...
[  8901] [INFO] MQTT connection details:
[  8902] [INFO]   clientId: ventilator
[  8903] [INFO]   host: mqtt.local
[  8904] [INFO]   user: esp32
[  9234] [INFO] MQTT connected
[ 10123] [INFO] Ventilatorregelaar device started with RTOS tasks
```

### Implementatie details

De Logger is geïmplementeerd als statische class met:
- `logger.h`: Header met public interface
- `logger.cpp`: Implementatie met `Serial.printf()` calls

Alle log functies gebruiken `Serial.printf()` voor geformatteerde output naar baudrate 115200.

### Best practices

1. **Gebruik juiste log level:**
   - `info()`: Normale flow events (startup, config geladen, connected)
   - `warn()`: Potentiële problemen (retry, timeout warning)
   - `error()`: Echte fouten (sensor fail, connection lost)
   - `debug()`: Tijdelijke debug info (alleen tijdens ontwikkeling)

2. **Wees duidelijk:**
   - Gebruik beschrijvende messages
   - Include relevante variabele waardes
   - Vermijd te veel logging in tight loops

3. **Format strings:**
   - `%d` voor integers
   - `%f` of `%.1f` voor floats (met precisie)
   - `%s` voor strings
   - `%x` voor hexadecimale waardes

4. **State changes loggen:**
   - Pin state changes
   - Connection status changes
   - Mode transitions
   - Configuration changes

---

## 🔹 Devicebeschrijving

### 🌫️ Luchtsensor

De luchtsensor device meet fijnstof concentraties met een PMS5003 sensor via de PMSerial library.

#### Hardware configuratie
- **PMS5003 sensor**: Particulate matter sensor
  - `PIN_PM_RX` (GPIO16): UART receive pin
  - `PIN_PM_TX` (GPIO17): UART transmit pin
  - `PIN_PM_ENABLE`: Power control pin voor sensor aan/uit schakelen
  - `PIN_PM_RESET`: Hardware reset pin (optioneel, default -1)

#### Sensor modes

**PM_NEVER_SLEEPS mode** (build flag):
- Sensor blijft continu ingeschakeld
- Geen warmup tijd nodig tussen metingen
- Measurement interval direct uit config.json gebruikt
- Hogere stroomverbruik maar snellere metingen mogelijk

**Normal mode** (zonder PM_NEVER_SLEEPS):
- Sensor wordt uitgeschakeld tussen metingen voor stroomverbruik optimalisatie
- Minimum interval: 30 seconden (waarvan 30s warmup)
- Sensor reset + enable + wakeUp voor elke meting
- Lagere stroomverbruik maar langere minimum interval

#### Sensor reset sequentie

Bij opstarten en tussen metingen (normal mode):
1. `airSensor.reset()` - Hardware reset via PIN_PM_RESET (indien gedefinieerd)
2. `digitalWrite(PIN_PM_ENABLE, HIGH)` - Schakel sensor power in
3. `vTaskDelay(2000ms)` - Wacht op power stabilisatie
4. `airSensor.wakeUp()` - Stuur wakeup commando naar sensor
5. `vTaskDelay(30000ms)` - Warmup tijd voor stabiele metingen

#### Zero reading detection

Automatische detectie en recovery van sensor malfuncties:
- Detecteert wanneer PM2.5 EN PM10 beide 0.0 zijn
- Telt consecutive zero readings
- **Bij meer dan 2 opeenvolgende nul-metingen:**
  1. Log waarschuwing
  2. Voer sensor reset uit
  3. WakeUp sensor
  4. Wacht 30 seconden warmup
  5. Reset counter en hervat metingen
- Counter wordt gereset bij elke geldige non-zero meting

#### MQTT publicatie

Publiceert naar `luchtsensor/data` met JSON payload:
```json
{
  "pm25": 12.5,
  "pm10": 15.3,
  "timestamp": 123456789
}
```

#### Measurement statistieken

Tracking in taskMeasurement:
- `framesOk`: Aantal succesvolle metingen
- `framesError`: Aantal gefaalde metingen
- `consecutiveZeroReadings`: Teller voor nul-metingen
- Heartbeat logging elke 60 seconden
- Stats logging elke 30 seconden

#### Reset functionaliteit

- Resetknop binnen 60s na opstart → wissen WiFi/MQTT + herstart
- GPIO0 (PIN_ERASE_WIFI) moet LOW zijn tijdens boot window

#### Telnet interface

Telnet server op port 23 met commands:
- `measure`: Voer directe sensor meting uit en toon resultaat
- Telnet output via Logger wordt naar alle verbonden clients gestuurd


---

## 🔹 RTOS taken

### Luchtsensor taken

| Taak | Functie | Core | Prioriteit |
|------|----------|------|------------|
| taskButtonMonitor | Detecteert resetknop binnen 60s boot window | 0 | 2 |
| taskMqttLoop | Houdt MQTT verbinding levend, automatische reconnect | 0 | 1 |
| taskMeasurement | Sensor metingen, zero detection, MQTT publish | 1 | 4 |
| taskTelnetServer | Telnet server loop voor remote debugging | 0 | 1 |


---

## 🔹 GPIO-overzicht (ESP32 DevKit)

| Pin | Functie | Opmerking |
|------|----------|------------|
| EN | Hardware reset | Niet gebruiken |
| GPIO0 | Bootloader | Niet gebruiken |
| GPIO0 | Erase WiFi | Aanbevolen |
| GPIO16/17 | PM RX/TX | Fijnstofsensor |
| GPIOxx | PM Reset | Fijnstofsensor |
| GPIOxx | PM Enable | Fijnstofsensor |

---

## 🔹 MQTT topics

| Device | Publishes | Subscribes |
|---------|------------|-------------|
| Luchtsensor | luchtsensor/data | — |

---

## 🔹 ASCII Architectuurdiagram

```
┌────────────────────┐
│   MQTT Broker      │
└────────┬───────────┘
         │
 ┌───────┴────────────────────────────────┐
 │                                        │
 ▼                                        ▼
┌───────────────-─────┐           ┌────────────────────────┐
│   Luchtsensor       │           │   Ventilatorregelaar   │
│  (ESP32-Dev)        │           │  (ESP32-Dev)           │
│────────────────-────│           │────────────────────────│
│ WiFiManagerExt      │           │ WiFiManagerExt         │
│ MqttClient          │           │ MqttClient             │
│ AirSensor (PMSerial)│           │ StepperMotor           │
│─────────────────-───│           │────────────────────────│
│ Publishes:          │           │ Publishes:             │
│  - luchtsensor/data │           │  - ventilator/status   │
│                     │           │ Subscribes:            │
│                     │           │  - ventilator/command  │
└──────────────────-──┘           └────────────────────────┘
```
