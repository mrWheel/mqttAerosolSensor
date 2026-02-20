/*** Last Changed: 2026-02-20 - 12:27 ***/
#include "mqttClient.h"
#include "logger.h"

//-- Constructor: Initialize MQTT client with configuration and set up callback
MqttClient::MqttClient(MqttConfig& config)
    : mqttClient(wifiClient),
      config(config)
{
  mqttClient.setServer(config.host(), atoi(config.port()));
  mqttClient.setCallback(
      [this](char* topic, byte* payload, unsigned int length)
      {
        this->internalCallback(topic, payload, length);
      });
}

//-- Connect to MQTT broker with limited retries and abort callback
bool MqttClient::connect(const std::string& clientId, int maxRetries, std::function<bool()> shouldAbort)
{
  currentClientId = clientId;
  int attempts = 0;

  while (!mqttClient.connected() && attempts < maxRetries)
  {
    //-- Check if abort requested (e.g., button pressed)
    if (shouldAbort && shouldAbort())
    {
      Logger::warn("MQTT connection aborted by user request");
      return false;
    }

    attempts++;
    Logger::info("MQTT connection attempt %d/%d", attempts, maxRetries);
    Logger::info("      host: [%s] on [%s]", config.host(), config.port());
    Logger::info("  clientId: [%s]", clientId.c_str());
    Logger::info("      user: [%s]", config.user());

    bool connected;

    //-- Check if username is empty - use anonymous connection if so
    if (config.user() == nullptr || strlen(config.user()) == 0)
    {
      Logger::info("Using anonymous MQTT connection (no username)");
      connected = mqttClient.connect(clientId.c_str());
    }
    else
    {
      connected = mqttClient.connect(
          clientId.c_str(),
          config.user(),
          config.pass());
    }

    if (connected)
    {
      Logger::info("MQTT connected successfully");
      return true;
    }
    else
    {
      int errorCode = mqttClient.state();
      const char* errorMsg = "";

      switch (errorCode)
      {
      case -4:
        errorMsg = "MQTT_CONNECTION_TIMEOUT - server didn't respond within keepalive time";
        break;
      case -3:
        errorMsg = "MQTT_CONNECTION_LOST - network connection was broken";
        break;
      case -2:
        errorMsg = "MQTT_CONNECT_FAILED - network connection failed";
        break;
      case -1:
        errorMsg = "MQTT_DISCONNECTED - client is disconnected cleanly";
        break;
      case 1:
        errorMsg = "MQTT_CONNECT_BAD_PROTOCOL - server doesn't support requested protocol version";
        break;
      case 2:
        errorMsg = "MQTT_CONNECT_BAD_CLIENT_ID - server rejected the client identifier";
        break;
      case 3:
        errorMsg = "MQTT_CONNECT_UNAVAILABLE - server was unable to accept the connection";
        break;
      case 4:
        errorMsg = "MQTT_CONNECT_BAD_CREDENTIALS - username/password were rejected";
        break;
      case 5:
        errorMsg = "MQTT_CONNECT_UNAUTHORIZED - client was not authorized to connect";
        break;
      default:
        errorMsg = "UNKNOWN_ERROR";
        break;
      }

      Logger::warn("MQTT connect failed (attempt %d/%d): error %d - %s", attempts, maxRetries, errorCode, errorMsg);

      if (attempts < maxRetries)
      {
        delay(1000);
      }
    }
  }

  if (!mqttClient.connected())
  {
    Logger::error("MQTT connection failed after %d attempts, continuing without MQTT", maxRetries);
    return false;
  }

  return true;
} //  connect()

//-- Reconnect to MQTT broker (for periodic retry attempts)
bool MqttClient::reconnect(const std::string& clientId, int maxRetries, std::function<bool()> shouldAbort)
{
  if (mqttClient.connected())
  {
    return true;
  }

  return connect(clientId, maxRetries, shouldAbort);
} //  reconnect()

//-- Check if MQTT client is connected
bool MqttClient::isConnected()
{
  return mqttClient.connected();
} //  isConnected()

//-- Publish a plain text message to an MQTT topic
void MqttClient::publish(const std::string& topic, const std::string& message)
{
  mqttClient.publish(topic.c_str(), message.c_str());
}

//-- Publish a JSON document to an MQTT topic
void MqttClient::publishJson(const std::string& topic, const JsonDocument& doc)
{
  std::string payload;
  serializeJson(doc, payload);
  mqttClient.publish(topic.c_str(), payload.c_str());
}

//-- Subscribe to an MQTT topic and set up message handler callback
void MqttClient::subscribe(const std::string& topic, std::function<void(const JsonDocument&)> onMessage)
{
  messageHandler = onMessage;
  mqttClient.subscribe(topic.c_str());
}

//-- Internal callback to handle incoming MQTT messages and parse JSON
void MqttClient::internalCallback(char* topic, byte* payload, unsigned int length)
{
  std::string jsonPayload;

  for (unsigned int index = 0; index < length; index++)
  {
    jsonPayload += static_cast<char>(payload[index]);
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonPayload);

  if (error)
  {
    Logger::error("Failed to parse MQTT JSON payload");
    return;
  }

  if (messageHandler)
  {
    messageHandler(doc);
  }
}

//-- Process MQTT client loop to maintain connection and handle messages
void MqttClient::loop()
{
  mqttClient.loop();
}
