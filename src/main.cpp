/*** Last Changed: 2026-02-20 - 13:03 ***/
#include <Arduino.h>
#include <string>
#include <cstring>
#include <ArduinoJson.h>
#include "wifiManagerExt.h"
#include "mqttClient.h"
#include "pms5003AirSensor.h"
#include "logger.h"
#include "telnetServer.h"

const char* PROG_VERSION = "v0.9.1";

WifiManagerExt wifiManager;
MqttConfig mqttConfig;
MqttClient* mqttClientInstance = nullptr;
AirSensor airSensor(PIN_PM_RX, PIN_PM_TX);
TelnetServer telnetServer;

SemaphoreHandle_t gConfigMutex = nullptr;

static std::string trimCopy(const std::string& s)
{
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
  {
    start++;
  }

  if (start == s.size())
  {
    return "";
  }

  size_t end = s.size() - 1;
  while (end > start && (s[end] == ' ' || s[end] == '\t'))
  {
    end--;
  }

  return s.substr(start, end - start + 1);
} //  trimCopy()

static void saveConfigToFile()
{
  if (gConfigMutex != nullptr)
  {
    xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  }

  if (!LittleFS.begin(true))
  {
    if (gConfigMutex != nullptr)
    {
      xSemaphoreGive(gConfigMutex);
    }

    Serial.println("ERROR: Failed to mount LittleFS");
    return;
  }

  JsonDocument doc;
  doc["host"] = mqttConfig.host();
  doc["port"] = mqttConfig.port();
  doc["user"] = mqttConfig.user();
  doc["pass"] = mqttConfig.pass();
  doc["topic"] = mqttConfig.topic();
  doc["interval"] = mqttConfig.measurementIntervalSec();

  File file = LittleFS.open("/config.json", "w");
  if (!file)
  {
    if (gConfigMutex != nullptr)
    {
      xSemaphoreGive(gConfigMutex);
    }

    Serial.println("ERROR: Failed to open config.json for writing");
    return;
  }

  serializeJson(doc, file);
  file.close();

  if (gConfigMutex != nullptr)
  {
    xSemaphoreGive(gConfigMutex);
  }
} //  saveConfigToFile()

static unsigned long getMeasurementIntervalSecSafe()
{
  unsigned long value = 0;

  if (gConfigMutex != nullptr)
  {
    xSemaphoreTake(gConfigMutex, portMAX_DELAY);
  }

  const char* intervalStr = mqttConfig.measurementIntervalSec();
  if (intervalStr != nullptr && intervalStr[0] != '\0')
  {
    value = strtoul(intervalStr, nullptr, 10);
  }

  if (gConfigMutex != nullptr)
  {
    xSemaphoreGive(gConfigMutex);
  }

  return value;
} //  getMeasurementIntervalSecSafe()

void handleSerialCommand(const std::string& command)
{
  if (command == "config")
  {
    if (gConfigMutex != nullptr)
    {
      xSemaphoreTake(gConfigMutex, portMAX_DELAY);
    }

    Serial.println("\nCurrent MQTT Configuration:");
    Serial.printf("  Host: %s\n", mqttConfig.host());
    Serial.printf("  Port: %s\n", mqttConfig.port());
    Serial.printf("  User: %s\n", mqttConfig.user());
    Serial.printf("  Pass: %s\n", mqttConfig.pass());
    Serial.printf("  Topic: %s\n", mqttConfig.topic());
    Serial.printf("  Interval: %s sec\n", mqttConfig.measurementIntervalSec());
    Serial.println();

    if (gConfigMutex != nullptr)
    {
      xSemaphoreGive(gConfigMutex);
    }
  }
  else if (command == "clearauth")
  {
    Serial.println("\nClearing MQTT username and password...");

    if (gConfigMutex != nullptr)
    {
      xSemaphoreTake(gConfigMutex, portMAX_DELAY);
    }

    mqttConfig.setUser("");
    mqttConfig.setPass("");

    if (gConfigMutex != nullptr)
    {
      xSemaphoreGive(gConfigMutex);
    }

    saveConfigToFile();

    Serial.println("MQTT credentials cleared!");
    Serial.println("Type 'restart' to reboot with new settings.\n");
  }
  else if (command == "restart")
  {
    Serial.println("\nRestarting device...\n");
    delay(500);
    ESP.restart();
  }
  else if (command == "help")
  {
    Serial.println("\nAvailable serial commands:");
    Serial.println("  config     - Show current MQTT configuration");
    Serial.println("  clearauth  - Clear MQTT username/password (for anonymous connection)");
    Serial.println("  restart    - Restart the device");
    Serial.println("  help       - Show this help message\n");
  }
  else
  {
    Serial.printf("\nUnknown command: %s\n", command.c_str());
    Serial.println("Type 'help' for available commands.\n");
  }
} //  handleSerialCommand()

//-- RTOS task: Monitor reset button during first 60 seconds after boot
void taskButtonMonitor(void* pvParameters)
{
  unsigned long start = millis();

  while (true)
  {
    if (millis() - start <= 60000UL)
    {
      if (digitalRead(PIN_ERASE_WIFI) == LOW)
      {
        Logger::warn("Reset button pressed in first 60s, clearing WiFi and restarting");
        wifiManager.reset();
        vTaskDelay(500 / portTICK_PERIOD_MS);
        ESP.restart();
      }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

//-- RTOS task: Keep MQTT connection alive and process messages
void taskMqttLoop(void* pvParameters)
{
  unsigned long lastReconnectAttempt = 0;
  const unsigned long reconnectInterval = 60000;

  while (true)
  {
    if (mqttClientInstance != nullptr)
    {
      mqttClientInstance->loop();

      //-- Attempt periodic reconnection if disconnected
      if (!mqttClientInstance->isConnected())
      {
        unsigned long now = millis();

        if (now - lastReconnectAttempt >= reconnectInterval)
        {
          lastReconnectAttempt = now;
          Logger::info("Attempting MQTT reconnection...");

          bool reconnected = mqttClientInstance->reconnect(
              wifiManager.getClientId(),
              10,
              []() -> bool
              {
                return false;
              });

          if (reconnected)
          {
            Logger::info("MQTT reconnected successfully");
          }
        }
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

} //  taskMqttLoop()

//-- RTOS task: Read air sensor periodically and publish data via MQTT
void taskMeasurement(void* pvParameters)
{
  static uint32_t framesOk = 0;
  static uint32_t framesError = 0;
  static uint32_t lastHeartbeat = 0;
  static uint32_t lastStats = 0;
  static uint32_t consecutiveZeroReadings = 0;
  static const uint32_t HEARTBEAT_INTERVAL_MS = 60000;
  static const uint32_t STATS_INTERVAL_MS = 30000;

  //-- Initial warmup on first boot
  Logger::info("taskMeasurement started, warming up sensor...");

  //-- Reset sensor before enabling (required for stable operation)
  airSensor.reset();

#ifndef PM_NEVER_SLEEPS
  //-- Enable sensor only if PM_NEVER_SLEEPS not defined
  digitalWrite(PIN_PM_ENABLE, HIGH);
  vTaskDelay(2000 / portTICK_PERIOD_MS);
#endif

  airSensor.wakeUp();
  Logger::info("Waiting 30 seconds for initial sensor warmup...");
  vTaskDelay(30000 / portTICK_PERIOD_MS);
  Logger::info("Initial warmup complete, starting measurement loop");

  lastHeartbeat = millis();
  lastStats = millis();

  while (true)
  {
    //-- Get measurement interval from config (in seconds)
    unsigned long intervalSec = getMeasurementIntervalSecSafe();

#ifdef PM_NEVER_SLEEPS
    //-- PM sensor is always enabled, no warmup needed, use interval from config
    if (intervalSec > 0)
    {
      Logger::info("Waiting %lu seconds before next measurement", intervalSec);

      //-- Wait with heartbeat and stats during interval
      unsigned long waitStart = millis();
      unsigned long waitEnd = waitStart + (intervalSec * 1000UL);

      while (millis() < waitEnd)
      {
        uint32_t now = millis();

        //-- Heartbeat
        if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS)
        {
          lastHeartbeat = now;
          Logger::debug("Heartbeat: millis=%lu, framesOk=%lu, framesError=%lu", now, framesOk, framesError);
        }

        //-- Stats
        if (now - lastStats >= STATS_INTERVAL_MS)
        {
          lastStats = now;
          Logger::debug("Stats: OK frames=%lu, Error frames=%lu, Total=%lu", framesOk, framesError, framesOk + framesError);
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
      }
    }
#else
    //-- PM sensor can be disabled for power saving
    //-- Minimum interval is 30 seconds when PM_NEVER_SLEEPS is not defined
    if (intervalSec < 30)
    {
      intervalSec = 30;
    }

    //-- Calculate wait time (interval - 30 seconds for warmup)
    unsigned long waitTimeSec = intervalSec - 30;
    if (waitTimeSec > 0)
    {
      Logger::info("Waiting %lu seconds before next measurement", waitTimeSec);

      //-- Wait with heartbeat and stats during interval
      unsigned long waitStart = millis();
      unsigned long waitEnd = waitStart + (waitTimeSec * 1000UL);

      while (millis() < waitEnd)
      {
        uint32_t now = millis();

        //-- Heartbeat
        if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS)
        {
          lastHeartbeat = now;
          Logger::debug("Heartbeat: millis=%lu, framesOk=%lu, framesError=%lu", now, framesOk, framesError);
        }

        //-- Stats
        if (now - lastStats >= STATS_INTERVAL_MS)
        {
          lastStats = now;
          Logger::debug("Stats: OK frames=%lu, Error frames=%lu, Total=%lu", framesOk, framesError, framesOk + framesError);
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
      }
    }

    //-- Reset sensor before enabling for warmup
    airSensor.reset();
    //-- Enable PM sensor for warmup
    Logger::info("Enabling PM sensor for warmup");
    digitalWrite(PIN_PM_ENABLE, HIGH);
    airSensor.wakeUp();

    //-- Wait 30 seconds for sensor warmup
    Logger::info("Waiting 30 seconds for sensor warmup");
    vTaskDelay(30000 / portTICK_PERIOD_MS);
#endif

    //-- Read sensor
    bool valid = airSensor.read();

    if (valid)
    {
      framesOk++;

      float pm25Value = airSensor.pm25();
      float pm10Value = airSensor.pm10();

      //-- Check for consecutive zero readings (sensor malfunction indicator)
      if (pm25Value == 0.0 && pm10Value == 0.0)
      {
        consecutiveZeroReadings++;
        Logger::warn("Zero reading detected (count: %lu)", consecutiveZeroReadings);

        //-- Reset sensor if more than 2 consecutive zero readings
        if (consecutiveZeroReadings > 2)
        {
          Logger::warn("More than 2 consecutive zero readings, resetting esp32");
          ESP.restart();
          consecutiveZeroReadings = 0;
        }
      }
      else
      {
        //-- Reset counter on valid non-zero reading
        consecutiveZeroReadings = 0;
      }

      if (mqttClientInstance != nullptr)
      {
        JsonDocument doc;
        doc["deviceId"] = wifiManager.getClientId();
        doc["pm25"] = pm25Value;
        doc["pm10"] = pm10Value;
        doc["timestamp"] = millis();

        const char* mqttTopic = mqttConfig.topic();
        mqttClientInstance->publishJson(mqttTopic, doc);
        Logger::info("Published %s", mqttTopic);
        Logger::info("  Device ID: %s", wifiManager.getClientId());
        Logger::info("  PM2.5: %.0f µg/m³", pm25Value);
        Logger::info("  PM10 : %.0f µg/m³", pm10Value);
      }
    }
    else
    {
      framesError++;
      Logger::warn("Failed to read air sensor");
    }

#ifndef PM_NEVER_SLEEPS
    //-- Disable PM sensor for power saving (only if PM_NEVER_SLEEPS not defined)
    Logger::info("Disabling PM sensor");
    airSensor.sleep();
    digitalWrite(PIN_PM_ENABLE, LOW);
#endif
  }

} //  taskMeasurement()

//-- Handle telnet commands specific to luchtsensor
void handleTelnetCommand(const std::string& command)
{
  if (command == "measure")
  {
    telnetServer.println("\r\nReading air sensor...");

    bool valid = airSensor.read();

    if (valid)
    {
      telnetServer.printf("  PM2.5: %.0f µg/m³\r\n", airSensor.pm25());
      telnetServer.printf("  PM10 : %.0f µg/m³\r\n", airSensor.pm10());
    }
    else
    {
      telnetServer.println("  Failed to read sensor");
    }

    telnetServer.println("");
  }
  else if (command == "config")
  {
    telnetServer.println("\r\nCurrent MQTT Configuration:");
    telnetServer.printf("  Host: %s\r\n", mqttConfig.host());
    telnetServer.printf("  Port: %s\r\n", mqttConfig.port());
    telnetServer.printf("  User: %s\r\n", mqttConfig.user());
    telnetServer.printf("  Pass: %s\r\n", mqttConfig.pass());
    telnetServer.printf("  Interval: %s sec\r\n", mqttConfig.measurementIntervalSec());
    telnetServer.println("");
  }
  else if (command == "clearauth")
  {
    telnetServer.println("\r\nClearing MQTT username and password...");
    mqttConfig.setUser("");
    mqttConfig.setPass("");

    //-- Save to file
    if (!LittleFS.begin(true))
    {
      telnetServer.println("ERROR: Failed to mount LittleFS");
      return;
    }

    JsonDocument doc;
    doc["host"] = mqttConfig.host();
    doc["port"] = mqttConfig.port();
    doc["user"] = "";
    doc["pass"] = "";
    doc["interval"] = mqttConfig.measurementIntervalSec();

    File file = LittleFS.open("/config.json", "w");
    if (!file)
    {
      telnetServer.println("ERROR: Failed to open config.json for writing");
      return;
    }

    serializeJson(doc, file);
    file.close();

    telnetServer.println("MQTT credentials cleared!");
    telnetServer.println("Type 'restart' to reboot with new settings.");
    telnetServer.println("");
  }
  else if (command == "restart")
  {
    telnetServer.println("\r\nRestarting device...\r\n");
    delay(500);
    ESP.restart();
  }
  else
  {
    telnetServer.printf("\r\nUnknown command: %s\r\n", command.c_str());
    telnetServer.println("Available luchtsensor commands:");
    telnetServer.println("  measure    - Read current air quality");
    telnetServer.println("  config     - Show current MQTT configuration");
    telnetServer.println("  clearauth  - Clear MQTT username/password (for anonymous connection)");
    telnetServer.println("  restart    - Restart the device");
    telnetServer.println("");
  }
}

//-- RTOS task: Handle telnet server connections and data
void taskTelnetServer(void* pvParameters)
{
  while (true)
  {
    telnetServer.loop();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }

} //  taskTelnetServer()

void taskSerialConsole(void* pvParameters)
{
  (void)pvParameters;

  std::string command;
  command.reserve(128);

  while (true)
  {
    while (Serial.available() > 0)
    {
      char c = (char)Serial.read();

      if (c == '\n')
      {
        command = trimCopy(command);

        if (!command.empty())
        {
          handleSerialCommand(command);
        }

        command.clear();
      }
      else if (c != '\r')
      {
        command.push_back(c);

        if (command.size() > 256)
        {
          command.clear();
          Serial.println("\nERROR: command too long\n");
        }
      }
    }

    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
} //  taskSerialConsole()

//-- Setup: Initialize hardware, WiFi, MQTT and start RTOS tasks
void setup()
{
  Serial.begin(115200);
  Logger::info("Booting Luchtsensor...");

  pinMode(PIN_ERASE_WIFI, INPUT_PULLUP);
  pinMode(PIN_PM_ENABLE, OUTPUT);
#ifdef PM_NEVER_SLEEPS
  //-- Keep PM sensor always enabled
  digitalWrite(PIN_PM_ENABLE, HIGH);
#else
  digitalWrite(PIN_PM_ENABLE, LOW);
#endif

  gConfigMutex = xSemaphoreCreateMutex();
  if (gConfigMutex == nullptr)
  {
    Logger::error("Failed to create config mutex");
  }

  //-- Check if config.json exists and load it
  bool configFileExists = wifiManager.configExists();

  //-- Check if host is empty or null (even if config file exists)
  bool hostIsEmpty = false;
  if (configFileExists)
  {
    //-- Load config to check host value
    if (!LittleFS.begin(true))
    {
      Logger::error("LittleFS mount failed");
    }

    File file = LittleFS.open("/config.json", "r");
    if (file)
    {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, file);
      file.close();

      if (!error)
      {
        const char* host = doc["host"];
        if (host == nullptr || strlen(host) == 0)
        {
          Logger::info("Host in config.json is empty or null");
          hostIsEmpty = true;
        }
      }
    }
  }

  //-- Start captive portal if no config exists or host is empty
  if (!configFileExists || hostIsEmpty)
  {
    if (!configFileExists)
    {
      Logger::info("Starting captive portal (no config file found)");
    }
    else if (hostIsEmpty)
    {
      Logger::info("Starting captive portal (host is empty/null)");
    }

    wifiManager.startPortal(mqttConfig);
    Logger::info("Captive portal completed, restarting...");
    delay(1000);
    ESP.restart();
  }

  //-- Normal WiFi/MQTT connection
  bool wifiOk = wifiManager.begin(mqttConfig);
  if (!wifiOk)
  {
    Logger::error("Failed to initialize WiFi/MQTT config");
  }

  mqttClientInstance = new MqttClient(mqttConfig);

  //-- Connect to MQTT with retry limit and button abort check
  bool mqttConnected = mqttClientInstance->connect(
      wifiManager.getClientId(),
      10,
      []() -> bool
      {
        return digitalRead(PIN_ERASE_WIFI) == LOW;
      });

  if (!mqttConnected)
  {
    Logger::warn("Starting without MQTT connection, will retry periodically");
  }

  airSensor.begin();

  //-- Start telnet server and link to logger
  telnetServer.begin();
  Logger::setTelnetServer(&telnetServer);
  telnetServer.setCommandCallback(handleTelnetCommand);
  Logger::info("Telnet server started on port 23");

  // xTaskCreate(
  xTaskCreatePinnedToCore(
      taskButtonMonitor,
      "ButtonMonitor",
      4096,
      nullptr,
      2,
      nullptr,
      0);

  // xTaskCreate(
  xTaskCreatePinnedToCore(
      taskMqttLoop,
      "MqttLoop",
      4096,
      nullptr,
      1,
      nullptr,
      0);

  /***
    xTaskCreate(
        taskMeasurement,
        "Measurement",
        4096,
        nullptr,
        1,
        nullptr);
  ***/

  xTaskCreatePinnedToCore(
      taskMeasurement,
      "measure",
      4096,
      NULL,
      4,
      NULL,
      1 // run ONLY on Core 1
  );

  // xTaskCreate(
  xTaskCreatePinnedToCore(
      taskTelnetServer,
      "TelnetServer",
      4096,
      nullptr,
      1,
      nullptr,
      0);

  xTaskCreatePinnedToCore(
      taskSerialConsole,
      "SerialConsole",
      4096,
      nullptr,
      1,
      nullptr,
      0);

  Logger::info("Luchtsensor device started with RTOS tasks");

} //  setup()

//-- Main loop: Handle serial commands
void loop()
{
  vTaskDelay(1000 / portTICK_PERIOD_MS);

} //  loop()
