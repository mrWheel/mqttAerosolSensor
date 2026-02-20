/*** Last Changed: 2026-02-20 - 13:57 ***/
#include "pms5003AirSensor.h"
#include "logger.h"

//-- Constructor: Initialize air sensor with RX/TX pins for PM sensor communication
AirSensor::AirSensor(uint8_t rxPin, uint8_t txPin)
    : rxPin(rxPin),
      txPin(txPin),
      pmsSerial(2),
      pms(pmsSerial)
{
}

//-- Initialize the PM sensor UART and library
void AirSensor::begin()
{
  //-- Setup RESET pin if defined (otherwise sensor has internal pull-up)
  if (PIN_PM_RESET >= 0)
  {
    pinMode(PIN_PM_RESET, OUTPUT);
    digitalWrite(PIN_PM_RESET, HIGH);
    Logger::info("AirSensor RESET pin %d configured (HIGH)", PIN_PM_RESET);
  }

  pmsSerial.begin(9600, SERIAL_8N1, rxPin, txPin);
  Logger::info("AirSensor initialized on RX=%d, TX=%d", rxPin, txPin);
}

//-- Reset the PM sensor
void AirSensor::reset()
{
  if (PIN_PM_RESET >= 0)
  {
    digitalWrite(PIN_PM_RESET, LOW);
    vTaskDelay(250 / portTICK_PERIOD_MS);
    digitalWrite(PIN_PM_RESET, HIGH);
    Logger::info("AirSensor reset");
  }
  else
  {
    Logger::warn("AirSensor reset skipped (PIN_PM_RESET not defined)");
  }
}
//-- Wake up the PM sensor from sleep mode
void AirSensor::wakeUp()
{
  pms.wakeUp();
  Logger::info("AirSensor woken up");
}

//-- Put the PM sensor into sleep mode for power saving
void AirSensor::sleep()
{
  pms.sleep();
  Logger::info("AirSensor put to sleep");
}

//-- Read PM values from sensor using PMS library (blocking with high-priority polling)
bool AirSensor::read()
{
  const unsigned long timeout = 5000;
  unsigned long startTime = millis();

  //-- Poll sensor until data received or timeout
  //-- High task priority ensures this gets sufficient CPU time
  while (millis() - startTime < timeout)
  {
    if (pms.read(pmsData))
    {
      Logger::info("AirSensor: PM1.0=%u, PM2.5=%u, PM10=%u", pmsData.PM_AE_UG_2_5, pmsData.PM_AE_UG_10_0);
      return true;
    }

    //-- Brief RTOS-friendly delay to yield to other tasks if needed
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  Logger::warn("AirSensor: read() timeout after %lu ms", timeout);
  return false;
}

//-- Get last read PM1.0 value (atmospheric environment, µg/m³)
float AirSensor::pm1() const
{
  return (float)pmsData.PM_AE_UG_1_0;
}

//-- Get last read PM2.5 value (atmospheric environment, µg/m³)
float AirSensor::pm25() const
{
  return (float)pmsData.PM_AE_UG_2_5;
}

//-- Get last read PM10 value (atmospheric environment, µg/m³)
float AirSensor::pm10() const
{
  return (float)pmsData.PM_AE_UG_10_0;
}

//-- Get complete last reading data structure
PMS::DATA AirSensor::getLastData() const
{
  return pmsData;
}
