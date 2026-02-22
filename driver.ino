// driver.ino
// ✅ BLE + WiFi Provisioning (JSON o comandos simples) + Bootstrap API + MQTT + Relay

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "ble_control.h"
#include "net_wifi_mqtt.h"

// ⚠️ ESP32 clásico: NO uses GPIO 11 (flash). C6 sí puede.
// Portable:
#if defined(CONFIG_IDF_TARGET_ESP32C6)
  #define RELAY_PIN 11
#else
  #define RELAY_PIN 26
#endif

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

static int relayLevel = LOW;

// ======================
// Utils
// ======================

static bool parseWifiCmd(const String& value, String& ssidOut, String& passOut) {
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

static bool isJsonPayload(const String& s) {
  if (s.length() == 0) return false;
  int i = 0;
  while (i < (int)s.length() && isspace((unsigned char)s[i])) i++;
  return (i < (int)s.length() && s[i] == '{');
}

static void ble_ok(const String& msg) {
  if (ble_isConnected()) ble_notify(msg);
}

// ======================
// Relay
// ======================

void applyRelay(int value01, const char* src) {
  bool on = (value01 > 0);
  relayLevel = on ? HIGH : LOW;
  digitalWrite(RELAY_PIN, relayLevel);

  Serial.print("[MAIN] PIN");
  Serial.print(RELAY_PIN);
  Serial.print(" ");
  Serial.print(on ? "ON" : "OFF");
  Serial.print(" (src=");
  Serial.print(src);
  Serial.println(")");

  net_publishState("V0", on ? 1 : 0);

  if (ble_isConnected()) ble_notify(String(on ? 1 : 0));
}

// ======================
// Handlers
// ======================

static void handleRelay(int value) {
  if (value != 0 && value != 1) {
    Serial.println("❌ [MAIN] Relay value inválido");
    ble_ok("{\"ok\":false,\"err\":\"RELAY_VALUE_INVALID\"}");
    return;
  }
  applyRelay(value, "BLE");
  ble_ok(String("{\"ok\":true,\"type\":\"relay\",\"value\":") + value + "}");
}

static void handleWifi(const String& ssid, const String& pass, bool save) {
  if (ssid.length() == 0) {
    Serial.println("❌ [MAIN] SSID vacío");
    ble_ok("{\"ok\":false,\"err\":\"WIFI_SSID_EMPTY\"}");
    return;
  }

  Serial.println("========================================");
  Serial.println("🚀 [MAIN] WIFI provisioning recibido por BLE");
  Serial.print("SSID=");
  Serial.println(ssid);
  Serial.print("PASS_LEN=");
  Serial.println(pass.length());
  Serial.println("========================================");

  ble_ok("{\"ok\":true,\"type\":\"wifi\",\"status\":\"RECEIVED\"}");

  // Dispara WiFi -> Bootstrap -> MQTT
  net_setWifiCredentials(ssid, pass, save);

  // Feedback inmediato
  if (net_isWifiConnected()) ble_ok("{\"ok\":true,\"type\":\"wifi\",\"status\":\"WIFI_OK\"}");
  else {
    ble_ok("{\"ok\":false,\"type\":\"wifi\",\"status\":\"WIFI_FAIL\"}");
    return;
  }

  if (net_isConnected()) ble_ok("{\"ok\":true,\"type\":\"wifi\",\"status\":\"MQTT_OK\"}");
  else ble_ok("{\"ok\":true,\"type\":\"wifi\",\"status\":\"MQTT_PENDING\"}");
}

static void handleAction(const String& name) {
  if (name.equalsIgnoreCase("STATUS")) {
    String msg = String("{\"ok\":true,\"type\":\"status\",\"wifi\":") +
                 (net_isWifiConnected() ? "1" : "0") +
                 ",\"mqtt\":" +
                 (net_isConnected() ? "1" : "0") +
                 ",\"relay\":" +
                 (relayLevel == HIGH ? "1" : "0") +
                 "}";
    ble_ok(msg);
    return;
  }

  if (name.equalsIgnoreCase("INFO")) {
    String msg = String("{\"ok\":true,\"type\":\"info\",\"heap\":") +
                 ESP.getFreeHeap() +
                 ",\"rssi\":" +
                 (net_isWifiConnected() ? String(WiFi.RSSI()) : String(-999)) +
                 "}";
    ble_ok(msg);
    return;
  }

  if (name.equalsIgnoreCase("REBOOT")) {
    ble_ok("{\"ok\":true,\"type\":\"action\",\"name\":\"REBOOT\"}");
    delay(250);
    ESP.restart();
    return;
  }

  if (name.equalsIgnoreCase("CLEAR_WIFI")) {
    ble_ok("{\"ok\":true,\"type\":\"action\",\"name\":\"CLEAR_WIFI\"}");
    delay(250);
    ESP.restart();
    return;
  }

  ble_ok("{\"ok\":false,\"err\":\"UNKNOWN_ACTION\"}");
}

// ======================
// BLE RX
// ======================

void onBleWrite(const String& value) {
  Serial.print("[MAIN] BLE RX: ");
  Serial.println(value);

  // A) JSON
  if (isJsonPayload(value)) {
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, value);
    if (err) {
      Serial.print("❌ BLE JSON parse error: ");
      Serial.println(err.c_str());
      ble_ok("{\"ok\":false,\"err\":\"JSON_PARSE\"}");
      return;
    }

    // ✅ 1) Si viene "type", usamos el router normal
    String t = (const char*)(doc["type"] | "");

    // ✅ 2) Si NO viene type, inferimos:
    // - si trae "ssid" => wifi provisioning
    // - si trae "value" => relay
    if (t.length() == 0) {
      if (doc.containsKey("ssid")) {
        String ssid = (const char*)(doc["ssid"] | "");

        // acepta "pass" o "password"
        String pass = (const char*)(doc["pass"] | "");
        if (pass.length() == 0) pass = (const char*)(doc["password"] | "");

        bool save = doc["save"] | true;

        Serial.println("✅ [MAIN] JSON inferido como WIFI (sin type)");
        handleWifi(ssid, pass, save);
        return;
      }

      if (doc.containsKey("value")) {
        int v = doc["value"] | -1;
        Serial.println("✅ [MAIN] JSON inferido como RELAY (sin type)");
        handleRelay(v);
        return;
      }

      Serial.println("❌ [MAIN] JSON sin type y sin campos conocidos");
      ble_ok("{\"ok\":false,\"err\":\"JSON_NO_TYPE\"}");
      return;
    }

    // Router por type (como antes)
    if (t == "wifi") {
      String ssid = (const char*)(doc["ssid"] | "");
      String pass = (const char*)(doc["pass"] | "");
      if (pass.length() == 0) pass = (const char*)(doc["password"] | "");
      bool save = doc["save"] | true;

      Serial.println("✅ [MAIN] type=wifi");
      handleWifi(ssid, pass, save);
      return;
    }

    if (t == "relay") {
      int v = doc["value"] | -1;
      Serial.println("✅ [MAIN] type=relay");
      handleRelay(v);
      return;
    }

    if (t == "action") {
      String name = (const char*)(doc["name"] | "");
      if (name.length() == 0) {
        ble_ok("{\"ok\":false,\"err\":\"ACTION_NAME_EMPTY\"}");
        return;
      }
      Serial.println("✅ [MAIN] type=action");
      handleAction(name);
      return;
    }

    if (t == "cmd") {
      String cmd = (const char*)(doc["value"] | "");
      if (cmd.length() == 0) {
        ble_ok("{\"ok\":false,\"err\":\"CMD_EMPTY\"}");
        return;
      }
      Serial.println("✅ [MAIN] type=cmd");
      handleAction(cmd);
      return;
    }

    ble_ok("{\"ok\":false,\"err\":\"JSON_TYPE_UNKNOWN\"}");
    return;
  }

  // B) Legacy / simple
  String ssid, pass;
  if (parseWifiCmd(value, ssid, pass)) {
    Serial.println("✅ [MAIN] Legacy WIFI:ssid|pass");
    handleWifi(ssid, pass, true);
    return;
  }

  if (value == "1" || value == "0") {
    handleRelay(value.toInt());
    return;
  }

  if (value.equalsIgnoreCase("STATUS") ||
      value.equalsIgnoreCase("INFO") ||
      value.equalsIgnoreCase("REBOOT") ||
      value.equalsIgnoreCase("CLEAR_WIFI")) {
    handleAction(value);
    return;
  }

  ble_ok("{\"ok\":false,\"err\":\"UNKNOWN_CMD\"}");
}

// ======================
// MQTT cmd
// ======================

void onMqttCmd(const String& vpin, int valueInt) {
  if (vpin == "V0") applyRelay(valueInt > 0 ? 1 : 0, "MQTT");
}

// ======================
// Setup / Loop
// ======================

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  NetConfig cfg;
  cfg.wifi_ssid = "";
  cfg.wifi_pass = "";

  cfg.tenant_id  = "77ec876c-b9f7-4170-a70a-647d85f58216";
  cfg.project_id = "9994133d-0064-45fc-a77b-1968b42e4a24";
  cfg.profile_id = "59b6770e-b6ce-4347-b343-e9d4e1cc86ac";
  cfg.alias      = "esp32c6-215";
  cfg.env        = "PROD";

  cfg.apikey    = "3f6a4cd5a8f3d8930f988ba12b9b8dfa";
  cfg.secretkey = "9395237bcaf83c512451b51d4f1e998b4c304d01ab4b727fa4f8064e9e74e570";

  cfg.mqtt_host    = "mqtt.nebadon.cloud";
  cfg.mqtt_port    = 8883;
  cfg.mqtt_user    = "neb_mqtt";
  cfg.mqtt_pass    = "Mqtt2025!";
  cfg.tls_insecure = true;

  cfg.api_base       = "https://api.nebadon.cloud";
  cfg.bootstrap_path = "/devices/bootstrap";

  net_begin(cfg, onMqttCmd);

  ble_begin("ESP32-NEBADON2", SERVICE_UUID, CHARACTERISTIC_UUID, onBleWrite);

  Serial.println("✅ Ready: BLE(JSON+infer+cmd) + WiFi Provisioning + MQTT + Relay");
  Serial.print("Relay pin: ");
  Serial.println(RELAY_PIN);
}

void loop() {
  ble_loop();
  net_loop();
  delay(5);
}