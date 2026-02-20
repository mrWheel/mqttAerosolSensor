/*** Last Changed: 2026-02-20 - 12:27 ***/
#include "mqttConfig.h"

//-- Constructor: Initialize MQTT configuration with default values
MqttConfig::MqttConfig()
    : mqttHost(""),
      mqttPort("1883"),
      mqttUser(""),
      mqttPass(""),
      mqttMeasurementIntervalSec("120")
{
}

//-- Get MQTT host as C-string
const char* MqttConfig::host() const
{
  return mqttHost.c_str();
}

//-- Get MQTT port as C-string
const char* MqttConfig::port() const
{
  return mqttPort.c_str();
}

//-- Get MQTT username as C-string
const char* MqttConfig::user() const
{
  return mqttUser.c_str();
}

//-- Get MQTT password as C-string
const char* MqttConfig::pass() const
{
  return mqttPass.c_str();
}

//-- Get measurement interval in seconds as C-string
const char* MqttConfig::measurementIntervalSec() const
{
  return mqttMeasurementIntervalSec.c_str();
}

//-- Set MQTT host
void MqttConfig::setHost(const std::string& value)
{
  mqttHost = value;
}

//-- Set MQTT port
void MqttConfig::setPort(const std::string& value)
{
  mqttPort = value;
}

//-- Set MQTT username
void MqttConfig::setUser(const std::string& value)
{
  mqttUser = value;
}

//-- Set MQTT password
void MqttConfig::setPass(const std::string& value)
{
  mqttPass = value;
}

//-- Set measurement interval in seconds
void MqttConfig::setMeasurementIntervalSec(const std::string& value)
{
  mqttMeasurementIntervalSec = value;
}
