#include <WiFi.h>
#include <Arduino.h>
#include "ble_control.h"
#include "net_wifi_mqtt.h"


#define RELAY_PIN 11

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

static int relayLevel = LOW;

// ======================================================
// Utils
// ======================================================

static bool parseWifiCmd(const String& value, String& ssidOut, String& passOut) {
  // Formato esperado: WIFI:ssid|password
  if (!value.startsWith("WIFI:")) return false;

  String payload = value.substring(5);
  int sep = payload.indexOf('|');

  if (sep < 0) {
    ssidOut = payload;
    passOut = "";
  } else {
    ssidOut = payload.substring(0, sep);
    passOut = payload.substring(sep + 1);
  }

  ssidOut.trim();
  passOut.trim();

  return ssidOut.length() > 0;
}

// ======================================================
// Relay logic
// ======================================================

void applyRelay(int value01, const char* src) {
  bool on = (value01 > 0);
  relayLevel = on ? HIGH : LOW;
  digitalWrite(RELAY_PIN, relayLevel);

  Serial.print("[MAIN] PIN11 ");
  Serial.print(on ? "ON" : "OFF");
  Serial.print(" (src=");
  Serial.print(src);
  Serial.println(")");

  net_publishState("V0", on ? 1 : 0);

  if (ble_isConnected()) {
    ble_notify(String(on ? 1 : 0));
  }
}

// ======================================================
// BLE WRITE HANDLER (INTELIGENTE)
// ======================================================

void onBleWrite(const String& value) {

  Serial.print("[MAIN] BLE CMD: ");
  Serial.println(value);

  // ---------- 1️⃣ WIFI Provisioning ----------
  String ssid, pass;
  if (parseWifiCmd(value, ssid, pass)) {

    if (net_isWifiConnected()) {
      ble_notify("WIFI:DENY_CONNECTED");
      Serial.println("⚠️ WiFi ya conectado, provisioning rechazado.");
      return;
    }

    ble_notify("WIFI:RECEIVED");
    net_setWifiCredentials(ssid, pass, true);

    delay(1000);

    if (net_isWifiConnected()) {
      ble_notify("WIFI:OK");
    } else {
      ble_notify("WIFI:FAIL");
    }

    return;
  }

  // ---------- 2️⃣ Relay simple ----------
  if (value == "1" || value == "0") {
    applyRelay(value.toInt(), "BLE");
    return;
  }

  // ---------- 3️⃣ STATUS ----------
  if (value.equalsIgnoreCase("STATUS")) {
    String msg = String("{\"wifi\":") +
                 (net_isWifiConnected() ? "1" : "0") +
                 ",\"relay\":" +
                 (relayLevel == HIGH ? "1" : "0") +
                 "}";
    ble_notify(msg);
    return;
  }

  // ---------- 4️⃣ CLEAR WIFI ----------
  if (value.equalsIgnoreCase("CLEAR_WIFI")) {
    ble_notify("WIFI:CLEARED");
    // Si expusiste esta función en header puedes usarla:
    // net_clearWifiCredentials();
    ESP.restart();
    return;
  }

  // ---------- 5️⃣ REBOOT ----------
  if (value.equalsIgnoreCase("REBOOT")) {
    ble_notify("REBOOTING");
    delay(300);
    ESP.restart();
    return;
  }

  // ---------- 6️⃣ INFO ----------
  if (value.equalsIgnoreCase("INFO")) {
    String msg = String("{\"heap\":") +
                 ESP.getFreeHeap() +
                 ",\"rssi\":" +
                 (net_isWifiConnected() ? WiFi.RSSI() : -999) +
                 "}";
    ble_notify(msg);
    return;
  }

  // ---------- 7️⃣ Comando desconocido ----------
  ble_notify("ERR:UNKNOWN_CMD");
}

// ======================================================
// MQTT COMMAND
// ======================================================

void onMqttCmd(const String& vpin, int valueInt) {
  if (vpin == "V0") {
    applyRelay(valueInt > 0 ? 1 : 0, "MQTT");
  }
}

// ======================================================
// SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // ========================
  // WiFi + MQTT Config
  // ========================
  NetConfig cfg;

  // ⚠️ Mejor dejar vacío si usarás provisioning BLE
  cfg.wifi_ssid = "";
  cfg.wifi_pass = "";

  cfg.tenant_id  = "77ec876c-b9f7-4170-a70a-647d85f58216";
  cfg.project_id = "9994133d-0064-45fc-a77b-1968b42e4a24";
  cfg.profile_id = "59b6770e-b6ce-4347-b343-e9d4e1cc86ac";
  cfg.alias      = "esp32c6-215";
  cfg.env        = "PROD";

  cfg.apikey    = "3f6a4cd5a8f3d8930f988ba12b9b8dfa";
  cfg.secretkey = "9395237bcaf83c512451b51d4f1e998b4c304d01ab4b727fa4f8064e9e74e570";

  cfg.mqtt_host     = "mqtt.nebadon.cloud";
  cfg.mqtt_port     = 8883;
  cfg.mqtt_user     = "neb_mqtt";
  cfg.mqtt_pass     = "Mqtt2025!";
  cfg.tls_insecure  = true;

  cfg.api_base       = "https://api.nebadon.cloud";
  cfg.bootstrap_path = "/devices/bootstrap";

  net_begin(cfg, onMqttCmd);

  // ========================
  // BLE
  // ========================
  ble_begin("ESP32-NEBADON2", SERVICE_UUID, CHARACTERISTIC_UUID, onBleWrite);

  Serial.println("✅ Ready: BLE + WiFi Provisioning + MQTT + Relay");
}

// ======================================================
// LOOP
// ======================================================

void loop() {
  ble_loop();
  net_loop();
  delay(5);
}