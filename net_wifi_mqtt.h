#pragma once
#include <Arduino.h>

// Callback: cuando llega un cmd por MQTT (vpin + value)
typedef void (*MqttCmdHandler)(const String& vpin, int value);

// Config para el módulo de red
struct NetConfig {
  // WiFi
  const char* wifi_ssid;
  const char* wifi_pass;

  // API KEYS
  const char* apikey;           // API KEY
  const char* secretkey;       // SECRET KEY

  // API bootstrap
  const char* api_base;        // "https://api.nebadon.cloud"
  const char* bootstrap_path;  // "/device/bootstrap"
  const char* tenant_id;       // UUID tenant
  const char* project_id;      // UUID project
  const char* profile_id;      // UUID profile  
  const char* alias;     // alias: "esp32c6-200"
  bool tls_insecure;           // true para setInsecure

  // MQTT
  const char* mqtt_host;       // "mqtt.nebadon.cloud"
  uint16_t mqtt_port;          // 8883
  const char* mqtt_user;
  const char* mqtt_pass;
  
  // ENV
  const char* env; // PROD or DEV

  // NTP
  bool use_ntp = true;
};

bool net_begin(const NetConfig& cfg, MqttCmdHandler onCmd);
void net_loop();
bool net_isConnected();

// Publicar estado: vpin/value al topicPub calculado
bool net_publishState(const String& vpin, int value);

// Publica todos estados que el main le pase (útil al reconectar)
typedef void (*PublishAllFn)();
void net_setPublishAllFn(PublishAllFn fn);