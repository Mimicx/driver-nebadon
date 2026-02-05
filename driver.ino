#include <Arduino.h>
#include "ble_control.h"
#include "net_wifi_mqtt.h"

#define RELAY_PIN 11

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

static int relayLevel = LOW;

void applyRelay(int value01, const char* src) {
  bool on = (value01 > 0);
  relayLevel = on ? HIGH : LOW;
  digitalWrite(RELAY_PIN, relayLevel);

  Serial.print("[MAIN] PIN11 ");
  Serial.print(on ? "ON" : "OFF");
  Serial.print(" (src=");
  Serial.print(src);
  Serial.println(")");

  // publicar a MQTT como estado
  net_publishState("V0", on ? 1 : 0);

  // opcional: notificar por BLE (eco)
  if (ble_isConnected()) {
    ble_notify(String(on ? 1 : 0));
  }
}

// BLE write "1"/"0"
void onBleWrite(const String& value) {
  int state = value.toInt();
  if (state == 0 || state == 1) applyRelay(state, "BLE");
}

// MQTT cmd: vpin V0
void onMqttCmd(const String& vpin, int valueInt) {
  if (vpin == "V0") {
    applyRelay(valueInt > 0 ? 1 : 0, "MQTT");
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);


  // WiFi + MQTT
  NetConfig cfg;
  cfg.wifi_ssid = "INFINITUM533C";
  cfg.wifi_pass = "6fRbSmrARp";
  /* LOCAL 
  cfg.tenant_id = "ddd6f51a-e6f0-48e8-9ebc-4a04b0a5998f";
  cfg.project_id = "85586178-96ed-48ca-9fd4-0bbd7ec29a69";
  cfg.profile_id = "7d2b660c-2ca3-4570-b8b8-ed77e97d611d";
  cfg.alias = "esp32c6-205";
  */
  cfg.tenant_id = "77ec876c-b9f7-4170-a70a-647d85f58216";
  cfg.project_id = "9994133d-0064-45fc-a77b-1968b42e4a24";
  cfg.profile_id = "59b6770e-b6ce-4347-b343-e9d4e1cc86ac";
  cfg.alias = "esp32c6-215";

  cfg.env = "PROD"; // PROD

  // LOCAL //
  /*cfg.mqtt_host = "192.168.1.70"; 
  cfg.mqtt_port = 1883;
  cfg.mqtt_user = "neb_mqtt";
  cfg.mqtt_pass = "Mqtt2025!";
  cfg.tls_insecure = false;
  */
  // API END POINT

  //cfg.api_base = "http://192.168.1.70:8000";
  //cfg.bootstrap_path = "/devices/bootstrap";
  cfg.apikey = "3f6a4cd5a8f3d8930f988ba12b9b8dfa";
  cfg.secretkey = "9395237bcaf83c512451b51d4f1e998b4c304d01ab4b727fa4f8064e9e74e570";

  // PROD //
  cfg.mqtt_host = "mqtt.nebadon.cloud"; 
  cfg.mqtt_port = 8883;
  cfg.mqtt_user = "neb_mqtt";
  cfg.mqtt_pass = "Mqtt2025!";
  cfg.tls_insecure = true;
  
  // API END POINT
  cfg.api_base = "https://api.nebadon.cloud";
  cfg.bootstrap_path = "/devices/bootstrap";
  


  net_begin(cfg, onMqttCmd);

  delay(250);

    // BLE (NimBLE)
  ble_begin("ESP32-NEBADON2", SERVICE_UUID, CHARACTERISTIC_UUID, onBleWrite);


  Serial.println("✅ Ready: NimBLE + MQTT (V0) controla pin 11");
}

void loop() {
  ble_loop();
  net_loop();
  delay(5);
}