/*** Last Changed: 2026-02-20 - 12:27 ***/
#pragma once
#include <Arduino.h>
#include <string>

class MqttConfig
{
public:
  MqttConfig();

  const char* host() const;
  const char* port() const;
  const char* user() const;
  const char* pass() const;
  const char* measurementIntervalSec() const;

  void setHost(const std::string& value);
  void setPort(const std::string& value);
  void setUser(const std::string& value);
  void setPass(const std::string& value);
  void setMeasurementIntervalSec(const std::string& value);

private:
  std::string mqttHost;
  std::string mqttPort;
  std::string mqttUser;
  std::string mqttPass;
  std::string mqttMeasurementIntervalSec;
};
