/*** Last Changed: 2026-02-20 - 12:27 ***/
#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>
#include "PMS.h"

//-- Default PIN_PM_RESET to -1 if not defined (internal pull-up in sensor)
#ifndef PIN_PM_RESET
#define PIN_PM_RESET -1
#endif

class AirSensor
{
public:
  AirSensor(uint8_t rxPin, uint8_t txPin);

  void begin();
  void reset();
  void wakeUp();
  void sleep();
  bool read();
  float pm25() const;
  float pm10() const;
  PMS::DATA getLastData() const;

private:
  uint8_t rxPin;
  uint8_t txPin;
  HardwareSerial pmsSerial;
  PMS pms;
  PMS::DATA pmsData;
};
