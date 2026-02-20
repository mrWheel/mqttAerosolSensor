/*** Last Changed: 2026-02-20 - 12:27 ***/
#include "logger.h"
#include "telnetServer.h"

//-- Static member initialization
TelnetServer* Logger::telnetServer = nullptr;

//-- Set telnet server for dual output
void Logger::setTelnetServer(TelnetServer* server)
{
  telnetServer = server;
}

//-- Internal helper function to format and print log messages with timestamp and level
void Logger::printLog(const char* level, const char* format, va_list args)
{
  char buffer[256];
  vsnprintf(buffer, sizeof(buffer), format, args);

  //-- Calculate time components with 24-hour rollover
  unsigned long totalMs = millis() % 86400000UL;
  unsigned long hours = totalMs / 3600000UL;
  unsigned long minutes = (totalMs % 3600000UL) / 60000UL;
  unsigned long seconds = (totalMs % 60000UL) / 1000UL;
  unsigned long centiseconds = (totalMs % 1000UL) / 10UL;

  //-- Output to Serial
  Serial.printf("[%2lu:%02lu:%02lu.%02lu] [%s] %s\n",
                hours, minutes, seconds, centiseconds, level, buffer);

  //-- Output to telnet if available
  if (telnetServer != nullptr && telnetServer->hasClient())
  {
    telnetServer->printf("[%2lu:%02lu:%02lu.%02lu] [%s] %s\r\n",
                         hours, minutes, seconds, centiseconds, level, buffer);
  }
}

//-- Log an informational message
void Logger::info(const char* format, ...)
{
#if LOG_LEVEL >= LOG_LEVEL_INFO
  va_list args;
  va_start(args, format);
  printLog("INFO", format, args);
  va_end(args);
#endif
}

//-- Log a warning message
void Logger::warn(const char* format, ...)
{
#if LOG_LEVEL >= LOG_LEVEL_WARN
  va_list args;
  va_start(args, format);
  printLog("WARN", format, args);
  va_end(args);
#endif
}

//-- Log an error message
void Logger::error(const char* format, ...)
{
#if LOG_LEVEL >= LOG_LEVEL_ERROR
  va_list args;
  va_start(args, format);
  printLog("ERROR", format, args);
  va_end(args);
#endif
}

//-- Log a debug message
void Logger::debug(const char* format, ...)
{
#if LOG_LEVEL >= LOG_LEVEL_DEBUG
  va_list args;
  va_start(args, format);
  printLog("DEBUG", format, args);
  va_end(args);
#endif
}
