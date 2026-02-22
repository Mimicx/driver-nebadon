#include "wifi_store.h"
#include <Preferences.h>

static Preferences prefs;
static const char* NS = "nebadon";

bool wifi_store_load(String &ssid, String &pass) {
  prefs.begin(NS, true); // read-only
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

bool wifi_store_save(const String &ssid, const String &pass) {
  if (ssid.length() == 0) return false;
  prefs.begin(NS, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  return true;
}

void wifi_store_clear() {
  prefs.begin(NS, false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}