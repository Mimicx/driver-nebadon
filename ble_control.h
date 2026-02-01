#pragma once
#include <Arduino.h>

typedef void (*BleOnWriteFn)(const String& value);

bool ble_begin(const char* deviceName,
               const char* serviceUUID,
               const char* characteristicUUID,
               BleOnWriteFn onWrite);

void ble_loop();
void ble_notify(const String& msg);
bool ble_isConnected();