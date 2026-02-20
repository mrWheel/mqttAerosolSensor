/*** Last Changed: 2026-02-20 - 13:57 ***/
#include "telnetServer.h"
#include <cstring>
#include <cstdarg>

//-- Constructor: Initialize telnet server on specified port
TelnetServer::TelnetServer(uint16_t port)
    : server(port), port(port), clientConnected(false)
{
  inputBuffer.reserve(INPUT_BUFFER_SIZE);
}

//-- Start telnet server and begin listening for connections
void TelnetServer::begin()
{
  server.begin();
  server.setNoDelay(true);
}

//-- Stop telnet server and disconnect any clients
void TelnetServer::stop()
{
  if (client && client.connected())
  {
    client.stop();
  }

  server.stop();
  clientConnected = false;
}

//-- Main loop: Handle client connections and data
void TelnetServer::loop()
{
  //-- Check for new client connections
  if (!clientConnected)
  {
    if (server.hasClient())
    {
      handleNewClient();
    }
  }
  else
  {
    //-- Check if client is still connected
    if (!client || !client.connected())
    {
      clientConnected = false;
      inputBuffer.clear();
      return;
    }

    //-- Check if there's a new client waiting (disconnect old one)
    if (server.hasClient())
    {
      handleNewClient();
      return;
    }

    //-- Process incoming data
    handleClientData();
  }
}

//-- Check if a client is currently connected
bool TelnetServer::hasClient()
{
  return clientConnected && client && client.connected();
}

//-- Write single byte to telnet client
void TelnetServer::write(uint8_t c)
{
  if (hasClient())
  {
    client.write(c);
  }
}

//-- Write buffer to telnet client
void TelnetServer::write(const uint8_t* buffer, size_t size)
{
  if (hasClient())
  {
    client.write(buffer, size);
  }
}

//-- Print string to telnet client
void TelnetServer::print(const char* message)
{
  if (hasClient())
  {
    client.print(message);
  }
}

//-- Print string with newline to telnet client
void TelnetServer::println(const char* message)
{
  if (hasClient())
  {
    client.println(message);
  }
}

//-- Printf-style formatted output to telnet client
void TelnetServer::printf(const char* format, ...)
{
  if (!hasClient())
  {
    return;
  }

  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  client.print(buffer);
}

//-- Set callback function for command handling
void TelnetServer::setCommandCallback(CommandCallback callback)
{
  commandCallback = callback;
}

//-- Handle new client connection
void TelnetServer::handleNewClient()
{
  //-- If already connected, disconnect old client and notify them
  if (clientConnected && client && client.connected())
  {
    //-- Send notification to old client
    client.println("\r\n");
    client.println("===================================");
    client.println("  New connection detected!");
    client.println("  Your session will be closed.");
    client.println("===================================");
    client.println("");
    client.flush();
    delay(100);

    //-- Close old connection
    client.stop();
    clientConnected = false;
    inputBuffer.clear();
  }

  //-- Accept new client
  client = server.available();

  if (client)
  {
    clientConnected = true;
    inputBuffer.clear();

    //-- Send welcome message
    sendWelcome();
  }
}

//-- Process incoming data from client
void TelnetServer::handleClientData()
{
  while (client.available())
  {
    char c = client.read();

    //-- Handle special telnet characters
    if (c == '\r')
    {
      continue;
    }
    else if (c == '\n')
    {
      //-- Process command
      if (!inputBuffer.empty())
      {
        processCommand(inputBuffer);
        inputBuffer.clear();
      }

      //-- Send prompt
      client.print("> ");
    }
    else if (c == 127 || c == 8)
    {
      //-- Backspace
      if (!inputBuffer.empty())
      {
        inputBuffer.pop_back();
        client.print("\b \b");
      }
    }
    else if (c >= 32 && c < 127)
    {
      //-- Printable character
      if (inputBuffer.length() < INPUT_BUFFER_SIZE - 1)
      {
        inputBuffer.push_back(c);
        client.write(c);
      }
    }
  }
}

//-- Send welcome message to newly connected client
void TelnetServer::sendWelcome()
{
  client.println("\r\n");
  client.println("===================================");
  client.println("  ESP32 Telnet Console");
  client.println("===================================");
  client.println("Type 'help' for available commands");
  client.println("");
  client.print("> ");
}

//-- Process received command
void TelnetServer::processCommand(const std::string& command)
{
  //-- Handle built-in commands
  if (command == "help")
  {
    client.println("\r\nAvailable commands:");
    client.println("  help       - Show this help message");
    client.println("  status     - Show device status");
    client.println("  config     - Show current MQTT configuration");
    client.println("  clear      - Clear screen");
    client.println("  clearauth  - Clear MQTT username/password (for anonymous connection)");
    client.println("  restart    - Restart the device");
    client.println("");
  }
  else if (command == "clear")
  {
    client.print("\033[2J\033[H");
  }
  else if (command == "status")
  {
    client.println("\r\nDevice Status:");
    client.printf("  Uptime: %lu ms\r\n", millis());
    client.printf("  Free heap: %u bytes\r\n", ESP.getFreeHeap());
    client.printf("  WiFi RSSI: %d dBm\r\n", WiFi.RSSI());
    client.println("");
  }
  else
  {
    //-- Call custom command callback if set
    if (commandCallback)
    {
      commandCallback(command);
    }
    else
    {
      client.printf("\r\nUnknown command: %s\r\n", command.c_str());
      client.println("Type 'help' for available commands\r\n");
    }
  }
}
