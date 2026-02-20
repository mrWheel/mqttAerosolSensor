/*** Last Changed: 2026-02-20 - 13:03 ***/
#include "wifiManagerExt.h"
#include "logger.h"
#include <WiFi.h>
#include <esp_system.h>

//-- Initialize MAC address and generate device-specific identifiers
void WifiManagerExt::initMacAddress()
{
  uint8_t mac[6];
  //-- Get ESP32 base MAC address (not WiFi MAC)
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  //-- Format last 4 bytes as hex string (8 characters) for MQTT client ID
  snprintf(macSuffix, sizeof(macSuffix), "%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);

  //-- Format last 2 bytes as hex string (4 characters) for mDNS and AP names
  snprintf(macSuffixShort, sizeof(macSuffixShort), "%02x%02x", mac[4], mac[5]);

  snprintf(clientId, sizeof(clientId), "LS%s", macSuffix);

  Logger::info("MAC Address: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Logger::info("MAC Suffix (full): %s", macSuffix);
  Logger::info("MAC Suffix (short): %s", macSuffixShort);
  Logger::info("Client ID: %s", clientId);
}

//-- Start mDNS service for network discovery
void WifiManagerExt::startMDNS()
{
  //-- Create mDNS hostname based on device type and short MAC suffix
  char hostname[32];

  snprintf(hostname, sizeof(hostname), "sensor-%s", macSuffixShort);

  if (MDNS.begin(hostname))
  {
    Logger::info("mDNS started: %s.local", hostname);
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("telnet", "tcp", 23);
  }
  else
  {
    Logger::warn("mDNS failed to start");
  }
}

//-- Get MAC address suffix (last 4 bytes as hex string)
const char* WifiManagerExt::getMacSuffix() const
{
  return macSuffix;
}

//-- Get MQTT client ID
const char* WifiManagerExt::getClientId() const
{
  return clientId;
}

//-- Initialize WiFi and MQTT configuration with captive portal and reset button monitoring
bool WifiManagerExt::begin(MqttConfig& config, int resetButtonPin)
{
  resetPin = resetButtonPin;

  //-- Initialize MAC address and identifiers
  initMacAddress();

  if (!LittleFS.begin(true))
  {
    Logger::error("LittleFS mount failed");
  }

  loadFromFile(config);

  Logger::info("Loaded MQTT config from file:");
  Logger::info("  Host: '%s'", config.host());
  Logger::info("  Port: '%s'", config.port());
  Logger::info("  User: '%s'", config.user());
  Logger::info("  Pass: '%s'", config.pass());
  Logger::info("  Topic: '%s'", config.topic());

  //-- Copy values to stable buffers for WiFiManager
  char hostBuffer[41] = {0};
  char portBuffer[7] = {0};
  char userBuffer[33] = {0};
  char passBuffer[33] = {0};
  char topicBuffer[65] = {0};
  char intervalBuffer[7] = {0};

  strncpy(hostBuffer, config.host(), 40);
  strncpy(portBuffer, config.port(), 6);
  strncpy(userBuffer, config.user(), 32);
  strncpy(passBuffer, config.pass(), 32);
  strncpy(topicBuffer, config.topic(), 64);
  strncpy(intervalBuffer, config.measurementIntervalSec(), 6);

  WiFiManagerParameter hostParam("host", "MQTT Host", hostBuffer, 40);
  WiFiManagerParameter portParam("port", "MQTT Port", portBuffer, 6);
  WiFiManagerParameter userParam("user", "MQTT User", userBuffer, 32);
  WiFiManagerParameter passParam("pass", "MQTT Password", passBuffer, 32);
  WiFiManagerParameter topicParam("topic", "MQTT Topic", topicBuffer, 64);
  WiFiManagerParameter intervalParam("interval", "Measurement Interval (sec)", intervalBuffer, 6);

  Logger::info("Created WiFiManager parameters with values from config");

  WiFiManager manager;
  manager.addParameter(&hostParam);
  manager.addParameter(&portParam);
  manager.addParameter(&userParam);
  manager.addParameter(&passParam);
  manager.addParameter(&topicParam);
  manager.addParameter(&intervalParam);

  //-- Set non-blocking mode to allow button checking
  manager.setConfigPortalBlocking(false);
  manager.setConfigPortalTimeout(240);

  //-- Create AP name with short MAC suffix
  char apName[32];
  snprintf(apName, sizeof(apName), "sensor-%s", macSuffixShort);

  Logger::info("WiFi AP name: %s", apName);

  //-- Start autoConnect in non-blocking mode
  if (!manager.autoConnect(apName, ""))
  {
    //-- Connection attempt started, now monitor in loop
    Logger::info("Starting WiFi connection attempt (240 second timeout)...");

    unsigned long startTime = millis();
    int lastResetButtonState = HIGH;
    if (resetPin >= 0)
    {
      lastResetButtonState = digitalRead(resetPin);
      Logger::info("Initial PIN_ERASE_WIFI state: %s", lastResetButtonState == LOW ? "LOW" : "HIGH");
    }

    while (WiFi.status() != WL_CONNECTED)
    {
      //-- Check timeout (60 seconds = 60000 ms)
      if (millis() - startTime >= 60000UL)
      {
        Logger::error("WiFi connection timeout after 240 seconds, restarting");
        delay(1000);
        ESP.restart();
        return false;
      }

      //-- Check reset button during connection attempt
      if (resetPin >= 0)
      {
        int currentResetButtonState = digitalRead(resetPin);

        //-- Log state change
        if (currentResetButtonState != lastResetButtonState)
        {
          Logger::info("PIN_ERASE_WIFI state changed: %s -> %s",
                       lastResetButtonState == LOW ? "LOW" : "HIGH",
                       currentResetButtonState == LOW ? "LOW" : "HIGH");
          lastResetButtonState = currentResetButtonState;
        }

        //-- Check if button is pressed
        if (currentResetButtonState == LOW)
        {
          Logger::warn("Reset button pressed during WiFi connection, clearing WiFi/MQTT and restarting");
          reset();
          delay(500);
          ESP.restart();
          return false;
        }
      }

      //-- Process WiFiManager state machine
      manager.process();
      delay(100);
    }
  } //  if()

  //-- Final check if connected
  if (WiFi.status() != WL_CONNECTED)
  {
    Logger::error("WiFi connection failed, restarting");
    delay(2000);
    ESP.restart();
    return false;
  }

  //-- Start mDNS service
  startMDNS();

  //-- Update config with values from form (including empty values for username/password)
  const char* newHost = hostParam.getValue();
  const char* newPort = portParam.getValue();
  const char* newUser = userParam.getValue();
  const char* newPass = passParam.getValue();
  const char* newTopic = topicParam.getValue();
  const char* newInterval = intervalParam.getValue();

  if (newHost != nullptr && strlen(newHost) > 0)
  {
    config.setHost(newHost);
  }

  if (newPort != nullptr && strlen(newPort) > 0)
  {
    config.setPort(newPort);
  }

  //-- Always update user/pass, even if empty (for anonymous MQTT)
  if (newUser != nullptr)
  {
    config.setUser(newUser);
  }

  if (newPass != nullptr)
  {
    config.setPass(newPass);
  }

  if (newTopic != nullptr && strlen(newTopic) > 0)
  {
    config.setTopic(newTopic);
  }

  if (newInterval != nullptr && strlen(newInterval) > 0)
  {
    config.setMeasurementIntervalSec(newInterval);
  }

  saveToFile(config);
  return true;

} //  WifiManagerExt::begin()

//-- Reset WiFi and MQTT configuration settings
void WifiManagerExt::reset()
{
  WiFiManager manager;
  manager.resetSettings();
  //--LittleFS.remove("/config.json");
  Logger::info("WiFi config reset requested");

} //  WifiManagerExt::reset()

//-- Check if config.json exists
bool WifiManagerExt::configExists()
{
  if (!LittleFS.begin(true))
  {
    Logger::error("LittleFS mount failed in configExists");
    return false;
  }

  bool exists = LittleFS.exists("/config.json");
  Logger::info("Config file exists: %s", exists ? "YES" : "NO");
  return exists;

} //  WifiManagerExt::configExists()

//-- Start captive portal with existing config values
void WifiManagerExt::startPortal(MqttConfig& config)
{
  Logger::info("Starting captive portal with existing credentials");

  //-- Initialize MAC address if not already done
  initMacAddress();

  if (!LittleFS.begin(true))
  {
    Logger::error("LittleFS mount failed");
  }

  loadFromFile(config);

  //-- Copy values to stable buffers for WiFiManager
  char hostBuffer[41] = {0};
  char portBuffer[7] = {0};
  char userBuffer[33] = {0};
  char passBuffer[33] = {0};
  char topicBuffer[65] = {0};
  char intervalBuffer[7] = {0};

  strncpy(hostBuffer, config.host(), 40);
  strncpy(portBuffer, config.port(), 6);
  strncpy(userBuffer, config.user(), 32);
  strncpy(passBuffer, config.pass(), 32);
  strncpy(topicBuffer, config.topic(), 64);
  strncpy(intervalBuffer, config.measurementIntervalSec(), 6);

  WiFiManagerParameter hostParam("host", "MQTT Host", hostBuffer, 40);
  WiFiManagerParameter portParam("port", "MQTT Port", portBuffer, 6);
  WiFiManagerParameter userParam("user", "MQTT User", userBuffer, 32);
  WiFiManagerParameter passParam("pass", "MQTT Password", passBuffer, 32);
  WiFiManagerParameter topicParam("topic", "MQTT Topic", topicBuffer, 64);
  WiFiManagerParameter intervalParam("interval", "Measurement Interval (sec)", intervalBuffer, 6);

  WiFiManager manager;
  manager.addParameter(&hostParam);
  manager.addParameter(&portParam);
  manager.addParameter(&userParam);
  manager.addParameter(&passParam);
  manager.addParameter(&topicParam);
  manager.addParameter(&intervalParam);

  //-- Start blocking portal
  manager.setConfigPortalBlocking(true);
  manager.setConfigPortalTimeout(0);

  //-- Create AP name with short MAC suffix
  char apName[32];
  snprintf(apName, sizeof(apName), "sensor-%s", macSuffixShort);

  Logger::info("WiFi AP name: %s", apName);

  if (!manager.startConfigPortal(apName, ""))
  {
    Logger::error("Failed to start config portal");
    ESP.restart();
  }

  //-- Update config with values from form (including empty values for username/password)
  const char* newHost = hostParam.getValue();
  const char* newPort = portParam.getValue();
  const char* newUser = userParam.getValue();
  const char* newPass = passParam.getValue();
  const char* newTopic = topicParam.getValue();
  const char* newInterval = intervalParam.getValue();

  if (newHost != nullptr && strlen(newHost) > 0)
  {
    config.setHost(newHost);
  }

  if (newPort != nullptr && strlen(newPort) > 0)
  {
    config.setPort(newPort);
  }

  //-- Always update user/pass, even if empty (for anonymous MQTT)
  if (newUser != nullptr)
  {
    config.setUser(newUser);
  }

  if (newPass != nullptr)
  {
    config.setPass(newPass);
  }

  if (newTopic != nullptr && strlen(newTopic) > 0)
  {
    config.setTopic(newTopic);
  }

  if (newInterval != nullptr && strlen(newInterval) > 0)
  {
    config.setMeasurementIntervalSec(newInterval);
  }

  saveToFile(config);
  Logger::info("Config portal completed, credentials saved");

} //  WifiManagerExt::startPortal()

//-- Load MQTT configuration from LittleFS JSON file
void WifiManagerExt::loadFromFile(MqttConfig& config)
{
  if (!LittleFS.exists("/config.json"))
  {
    Logger::info("MQTT config file does not exist");
    return;
  }

  File file = LittleFS.open("/config.json", "r");
  if (!file)
  {
    Logger::error("Failed to open /config.json for reading");
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);

  if (!error)
  {
    config.setHost(doc["host"] | "");
    config.setPort(doc["port"] | "1883");
    config.setUser(doc["user"] | "");
    config.setPass(doc["pass"] | "");
#ifdef TOPIC_DATA
    config.setTopic(doc["topic"] | TOPIC_DATA);
#else
    config.setTopic(doc["topic"] | "luchtsensor/data");
#endif
    config.setMeasurementIntervalSec(doc["interval"] | "120");

    Logger::info("Loaded config.json contents:");
    Logger::info("  host: '%s'", doc["host"] | "");
    Logger::info("  port: '%s'", doc["port"] | "1883");
    Logger::info("  user: '%s'", doc["user"] | "");
    Logger::info("  pass: '%s'", doc["pass"] | "");
#ifdef TOPIC_DATA
    Logger::info("  topic: '%s'", doc["topic"] | TOPIC_DATA);
#else
    Logger::info("  topic: '%s'", doc["topic"] | "luchtsensor/data");
#endif
    Logger::info("  interval: '%s'", doc["interval"] | "120");
  }

  file.close();

} //  WifiManagerExt::loadFromFile()

//-- Save MQTT configuration to LittleFS JSON file
void WifiManagerExt::saveToFile(const MqttConfig& config)
{
  JsonDocument doc;
  doc["host"] = config.host();
  doc["port"] = config.port();
  doc["user"] = config.user();
  doc["pass"] = config.pass();
  doc["topic"] = config.topic();
  doc["interval"] = config.measurementIntervalSec();

  File file = LittleFS.open("/config.json", "w");

  if (!file)
  {
    Logger::error("Failed to open /config.json for writing");
    return;
  }

  serializeJson(doc, file);
  file.close();

} //  WifiManagerExt::saveToFile()
