/*** Last Changed: 2026-02-20 - 12:27 ***/
#pragma once
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <functional>
#include <string>
#include "mqttConfig.h"

class MqttClient
{
public:
  explicit MqttClient(MqttConfig& config);

  bool connect(const std::string& clientId, int maxRetries = 10, std::function<bool()> shouldAbort = nullptr);
  bool reconnect(const std::string& clientId, int maxRetries = 10, std::function<bool()> shouldAbort = nullptr);
  bool isConnected();
  void publish(const std::string& topic, const std::string& message);
  void publishJson(const std::string& topic, const JsonDocument& doc);
  void subscribe(const std::string& topic, std::function<void(const JsonDocument&)> onMessage);
  void loop();

private:
  WiFiClient wifiClient;
  PubSubClient mqttClient;
  MqttConfig& config;
  std::function<void(const JsonDocument&)> messageHandler;
  std::string currentClientId;

  void internalCallback(char* topic, byte* payload, unsigned int length);
};
