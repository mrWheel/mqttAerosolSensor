/*** Last Changed: 2026-02-20 - 12:27 ***/
#pragma once
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include "mqttConfig.h"

class WifiManagerExt
{
public:
  bool begin(MqttConfig& config, int resetButtonPin = -1);
  void reset();
  bool configExists();
  void startPortal(MqttConfig& config);
  const char* getMacSuffix() const;
  const char* getClientId() const;

private:
  void loadFromFile(MqttConfig& config);
  void saveToFile(const MqttConfig& config);
  void initMacAddress();
  void startMDNS();
  int resetPin;
  char macSuffix[9];
  char macSuffixShort[5];
  char clientId[12];
};
