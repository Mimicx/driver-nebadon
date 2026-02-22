#pragma once
#include <Arduino.h>

bool wifi_store_load(String &ssid, String &pass);
bool wifi_store_save(const String &ssid, const String &pass);
void wifi_store_clear();