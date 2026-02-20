/*** Last Changed: 2026-02-20 - 12:27 ***/
#ifndef TELNET_SERVER_H
#define TELNET_SERVER_H

#include <WiFi.h>
#include <functional>

//-- Telnet server class for remote console access via stream-based I/O
class TelnetServer
{
public:
  //-- Callback type for command handling
  using CommandCallback = std::function<void(const std::string&)>;

  TelnetServer(uint16_t port = 23);

  void begin();
  void stop();
  void loop();

  bool hasClient();

  void write(uint8_t c);
  void write(const uint8_t* buffer, size_t size);
  void print(const char* message);
  void println(const char* message);
  void printf(const char* format, ...);

  void setCommandCallback(CommandCallback callback);

private:
  void handleNewClient();
  void handleClientData();
  void sendWelcome();
  void processCommand(const std::string& command);

  WiFiServer server;
  WiFiClient client;
  uint16_t port;
  bool clientConnected;
  std::string inputBuffer;
  CommandCallback commandCallback;

  static const size_t INPUT_BUFFER_SIZE = 256;
};

#endif
