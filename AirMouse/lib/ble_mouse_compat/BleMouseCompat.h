// Simple compatibility wrapper to adapt ESP32 BLE Mouse lib (std::string) to
// current Arduino BLE core expecting Arduino String API.
#pragma once
#include <BleMouse.h>

class BleMouseCompat : public BleMouse {
public:
  BleMouseCompat(const char* name, const char* manufacturer, uint8_t battery = 100)
  : BleMouse(std::string(name), std::string(manufacturer), battery) {}
};
