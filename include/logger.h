/*** Last Changed: 2026-02-20 - 12:27 ***/
#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <stdarg.h>
#include <string>

//-- Forward declaration
class TelnetServer;

//-- Log level definitions
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN 2
#define LOG_LEVEL_INFO 3
#define LOG_LEVEL_DEBUG 4

//-- Default to INFO level if not specified
#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

class Logger
{
public:
  static void info(const char* format, ...);
  static void warn(const char* format, ...);
  static void error(const char* format, ...);
  static void debug(const char* format, ...);

  static void setTelnetServer(TelnetServer* server);

private:
  static void printLog(const char* level, const char* format, va_list args);
  static TelnetServer* telnetServer;
};

#endif
