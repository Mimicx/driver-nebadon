// net_wifi_mqtt.cpp
// ✅ Configurado para:
// 1) Intentar WiFi con credenciales guardadas en NVS (Preferences)
// 2) Si no hay o falla, queda “listo” para que BLE le mande SSID/PASS
// 3) Al recibir nuevas credenciales vía net_setWifiCredentials(): guarda (opcional),
//    reconecta WiFi, corre BOOTSTRAP y levanta MQTT con topics por DEVICE UUID.

#include <Arduino.h>
#include "net_wifi_mqtt.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include "esp_mac.h"
#include <Preferences.h>

// =======================
// Globals
// =======================
static NetConfig _cfg{};
static MqttCmdHandler _onCmd = nullptr;
static PublishAllFn _publishAllFn = nullptr;

// ✅ Device UUID obtenido del bootstrap (se usa en topics/payload)
static String _deviceId = "";

// Topics (por device UUID)
static String topicPub; // nebadondevice/<TENANT>/<DEVICE_UUID>/dt
static String topicSub; // nebadoncmd/<TENANT>/<DEVICE_UUID>/cmd

// MQTT clients (TCP + TLS)
static WiFiClient tcpClient;
static WiFiClientSecure tlsClient;

static PubSubClient mqttTcp(tcpClient);
static PubSubClient mqttTls(tlsClient);
static PubSubClient* mqtt = &mqttTcp;

// HTTP clients (evita objetos locales)
static WiFiClient httpClient;
static WiFiClientSecure httpsClient;

// Reconnect timers
static unsigned long lastWifiReconnectAttemptMs = 0;
static unsigned long lastBootstrapAttemptMs = 0;
static unsigned long lastMqttReconnectAttemptMs = 0;

// WiFi creds runtime (provienen de NVS o cfg o BLE)
static String _wifiSsid = "";
static String _wifiPass = "";

// =======================
// NVS (Preferences) WiFi storage
// =======================
static Preferences _prefs;
static const char* PREF_NS = "nebadon";
static const char* PREF_SSID = "ssid";
static const char* PREF_PASS = "pass";

static bool nvs_loadWifi(String &ssid, String &pass) {
  _prefs.begin(PREF_NS, true);
  ssid = _prefs.getString(PREF_SSID, "");
  pass = _prefs.getString(PREF_PASS, "");
  _prefs.end();
  return ssid.length() > 0;
}

static bool nvs_saveWifi(const String &ssid, const String &pass) {
  if (ssid.length() == 0) return false;
  _prefs.begin(PREF_NS, false);
  _prefs.putString(PREF_SSID, ssid);
  _prefs.putString(PREF_PASS, pass);
  _prefs.end();
  return true;
}

static void nvs_clearWifi() {
  _prefs.begin(PREF_NS, false);
  _prefs.remove(PREF_SSID);
  _prefs.remove(PREF_PASS);
  _prefs.end();
}

// =======================
// Helpers: MAC / CHIP
// =======================
static String getMacAddress() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

static String getChipModel() {
#if defined(CONFIG_IDF_TARGET_ESP32C6)
  return "ESP32-C6";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  return "ESP32-S3";
#elif defined(CONFIG_IDF_TARGET_ESP32)
  return "ESP32";
#else
  return "ESP32-UNKNOWN";
#endif
}

// =======================
// WiFi
// =======================
static bool setupWiFiWithCreds(const String& ssid, const String& pass, uint32_t timeoutMs) {
  if (ssid.length() == 0) {
    Serial.println("⚠️ setupWiFi: SSID vacío (esperando provisioning BLE)");
    return false;
  }

  Serial.print("📶 Conectando a WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(200);

  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > timeoutMs) {
      Serial.println("\n❌ WiFi timeout.");
      return false;
    }
  }

  Serial.println("\n✅ WiFi conectado");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool net_isWifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

// =======================
// NTP (opcional)
// =======================
static void setupTimeIfNeeded() {
  if (!_cfg.use_ntp) return;

  Serial.println("⏱ Configurando NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  struct tm timeinfo;
  Serial.print("Sincronizando hora");
  unsigned long start = millis();
  while (!getLocalTime(&timeinfo)) {
    Serial.print(".");
    delay(300);
    if (millis() - start > 12000) {
      Serial.println("\n⚠️ No se pudo sincronizar NTP (seguimos).");
      return;
    }
  }
  Serial.println("\n✅ Hora sincronizada");
}

// =======================
// Bootstrap device (HTTP/HTTPS POST)
// ✅ OBLIGATORIO: si falla -> no hay MQTT
// =======================
static bool bootstrapDevice(String &device_uuid_out) {
  device_uuid_out = "";

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ bootstrapDevice: WiFi no conectado");
    return false;
  }

  if (!_cfg.api_base || !_cfg.bootstrap_path) {
    Serial.println("❌ bootstrapDevice: api_base/bootstrap_path null");
    return false;
  }

  String url = String(_cfg.api_base) + _cfg.bootstrap_path;
  Serial.print("📨 BOOT url=");
  Serial.println(url);

  HTTPClient http;
  http.setTimeout(7000);
  http.setReuse(false);

  bool begun = false;
  if (url.startsWith("http://")) {
    httpClient.setTimeout(7000);
    begun = http.begin(httpClient, url);
    if (!begun) {
      Serial.println("❌ http.begin() falló (HTTP)");
      return false;
    }
  } else if (url.startsWith("https://")) {
    httpsClient.setTimeout(7000);
    if (_cfg.tls_insecure) httpsClient.setInsecure();
    begun = http.begin(httpsClient, url);
    if (!begun) {
      Serial.println("❌ https.begin() falló (HTTPS)");
      return false;
    }
  } else {
    Serial.println("❌ bootstrapDevice: URL inválida (sin http:// o https://)");
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  if (_cfg.apikey && strlen(_cfg.apikey) > 0)        http.addHeader("x-api-key", _cfg.apikey);
  if (_cfg.secretkey && strlen(_cfg.secretkey) > 0)  http.addHeader("x-api-secret", _cfg.secretkey);

  StaticJsonDocument<512> doc;
  doc["tenant_id"]   = _cfg.tenant_id;
  doc["project_id"]  = _cfg.project_id;
  doc["alias"]       = _cfg.alias;
  doc["mac_address"] = getMacAddress();
  doc["chip_model"]  = getChipModel();
  doc["fw_version"]  = "1.0.0";
  doc["ip"]          = WiFi.localIP().toString();
  doc["rssi"]        = WiFi.RSSI();
  doc["profile_id"]  = _cfg.profile_id;

  String body;
  serializeJson(doc, body);

  Serial.print("BODY: ");
  Serial.println(body);

  int code = http.POST(body);
  String resp = http.getString();

  Serial.print("HTTP ");
  Serial.println(code);
  Serial.print("RESP: ");
  Serial.println(resp);

  http.end();

  if (code < 200 || code >= 300) {
    Serial.println("❌ bootstrapDevice: HTTP no-2xx");
    return false;
  }

  StaticJsonDocument<768> rdoc;
  DeserializationError err = deserializeJson(rdoc, resp);
  if (err) {
    Serial.print("❌ bootstrapDevice: JSON resp parse error: ");
    Serial.println(err.c_str());
    return false;
  }

  bool ok = rdoc["ok"] | false;
  const char* did = rdoc["device_id"] | "";

  if (!ok || !did || strlen(did) == 0) {
    Serial.println("❌ bootstrapDevice: resp no trae ok/device_id válido");
    return false;
  }

  device_uuid_out = String(did);
  Serial.print("✅ bootstrap OK device_id(UUID)=");
  Serial.println(device_uuid_out);
  return true;
}

// =======================
// JSON helpers
// =======================
static bool extractVpinAndValue(JsonDocument &doc, String &vpinOut, int &valueOut) {
  const char* vpin = doc["vpin"] | doc["pin"];
  if (!vpin || String(vpin).length() == 0) return false;

  JsonVariant v = doc["value"];
  if (v.isNull()) return false;

  int valueInt = 0;
  if (v.is<bool>()) valueInt = v.as<bool>() ? 1 : 0;
  else if (v.is<long>() || v.is<int>()) valueInt = v.as<int>();
  else if (v.is<float>() || v.is<double>()) valueInt = (int) lroundf(v.as<float>());
  else if (v.is<const char*>()) valueInt = String(v.as<const char*>()).toInt();
  else valueInt = v.as<int>();

  vpinOut = String(vpin);
  valueOut = valueInt;
  return true;
}

// =======================
// MQTT callback
// =======================
static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.print("📥 MQTT recibido en topic: ");
  Serial.println(topic);

  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("❌ JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  const char* type = doc["type"] | "";
  if (strlen(type) > 0 && String(type) != "cmd") {
    Serial.print("⚠️ Ignorado por type=");
    Serial.println(type);
    return;
  }

  const char* t = doc["tenant_id"] | "";
  if (strlen(t) > 0 && String(t) != String(_cfg.tenant_id)) {
    Serial.println("⚠️ Ignorado por tenant_id distinto");
    return;
  }

  String vpin;
  int valueInt = 0;
  if (!extractVpinAndValue(doc, vpin, valueInt)) {
    Serial.println("⚠️ JSON sin vpin/pin o value");
    return;
  }

  Serial.print("✅ CMD vpin=");
  Serial.print(vpin);
  Serial.print(" value=");
  Serial.println(valueInt);

  if (_onCmd) _onCmd(vpin, valueInt);
}

// =======================
// MQTT connect / reconnect (no bloqueante)
// =======================
static bool ensureMqttConnected() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (_deviceId.length() == 0) return false;
  if (mqtt->connected()) return true;

  Serial.print("🔌 Conectando a MQTT... ");
  Serial.print(_cfg.mqtt_host);
  Serial.print(":");
  Serial.print(_cfg.mqtt_port);
  Serial.print(" ");

  // ✅ clientId usando UUID del device (más control)
  String clientId = _deviceId + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  bool ok = mqtt->connect(clientId.c_str(), _cfg.mqtt_user, _cfg.mqtt_pass);
  if (!ok) {
    Serial.print("❌ fallo MQTT state=");
    Serial.println(mqtt->state());
    return false;
  }

  Serial.println("✔ conectado.");

  bool subOk = mqtt->subscribe(topicSub.c_str());
  Serial.print(subOk ? "📡 Suscrito a: " : "❌ Falló subscribe: ");
  Serial.println(topicSub);

  if (_publishAllFn) _publishAllFn();
  return true;
}

// =======================
// Construir MQTT (TCP/TLS) + topics (requiere _deviceId)
// =======================
static void configureMqttAndTopics() {
  if (_deviceId.length() == 0) return;

  topicPub = String("nebadondevice/") + _cfg.tenant_id + "/" + _deviceId + "/dt";
  topicSub = String("nebadoncmd/")    + _cfg.tenant_id + "/" + _deviceId + "/cmd";

  bool useTls = (_cfg.mqtt_port == 8883);
  if (useTls) {
    if (_cfg.tls_insecure) tlsClient.setInsecure();
    tlsClient.setTimeout(5000);
    mqtt = &mqttTls;
    Serial.println("🔐 MQTT usando TLS (8883)");
  } else {
    mqtt = &mqttTcp;
    Serial.println("🌐 MQTT sin TLS (1883)");
  }

  mqtt->setServer(_cfg.mqtt_host, _cfg.mqtt_port);
  mqtt->setCallback(onMqttMessage);
  mqtt->setBufferSize(1024);

  Serial.print("✅ MQTT topics: PUB=");
  Serial.print(topicPub);
  Serial.print(" SUB=");
  Serial.println(topicSub);
}

// =======================
// API pública
// =======================
bool net_begin(const NetConfig& cfg, MqttCmdHandler onCmd) {
  _cfg = cfg;
  _onCmd = onCmd;
  _deviceId = "";
  topicPub = "";
  topicSub = "";

  // 1) Cargar WiFi de NVS primero (si existe)
  String savedSsid, savedPass;
  bool hasSaved = nvs_loadWifi(savedSsid, savedPass);

  if (hasSaved) {
    _wifiSsid = savedSsid;
    _wifiPass = savedPass;
    Serial.println("💾 WiFi cargado desde NVS.");
  } else {
    _wifiSsid = (_cfg.wifi_ssid ? String(_cfg.wifi_ssid) : "");
    _wifiPass = (_cfg.wifi_pass ? String(_cfg.wifi_pass) : "");
    Serial.println("ℹ️ WiFi usando credenciales del firmware (no hay NVS).");
  }

  // 2) Intentar WiFi (si hay SSID). Si falla, NO reventamos: dejamos BLE provision.
  bool wifiOk = setupWiFiWithCreds(_wifiSsid, _wifiPass, 20000);
  if (!wifiOk) {
    Serial.println("⚠️ WiFi no conectado. Esperando provisioning BLE o reconexión en net_loop().");
    return true; // ✅ importante para que tu main siga y BLE quede activo
  }

  // 3) NTP antes de HTTPS (si aplica)
  setupTimeIfNeeded();

  // 4) BOOTSTRAP obligatorio (si falla, nos quedamos sin MQTT pero seguimos vivos)
  if (!_cfg.api_base || !_cfg.bootstrap_path) {
    Serial.println("❌ api_base/bootstrap_path no definidos. MQTT no iniciará.");
    return true;
  }

  String deviceUUID;
  if (!bootstrapDevice(deviceUUID)) {
    Serial.println("❌ Bootstrap falló. MQTT no iniciará (reintentos en loop).");
    return true;
  }

  _deviceId = deviceUUID;
  configureMqttAndTopics();

  // intento inicial MQTT
  if (!ensureMqttConnected()) {
    Serial.println("⚠️ MQTT no conectó en intento inicial (reintentos en loop).");
  }

  return true;
}

void net_loop() {
  unsigned long now = millis();

  // 1) WiFi reconnection (si tenemos SSID)
  if (WiFi.status() != WL_CONNECTED) {
    if (_wifiSsid.length() == 0) return; // esperando BLE provisioning

    if (now - lastWifiReconnectAttemptMs > 3000) {
      lastWifiReconnectAttemptMs = now;
      Serial.println("🔁 Reintentando WiFi...");
      setupWiFiWithCreds(_wifiSsid, _wifiPass, 12000);

      // si reconectó, resetea intentos subsecuentes
      if (WiFi.status() == WL_CONNECTED) {
        setupTimeIfNeeded();
        lastBootstrapAttemptMs = 0;
        lastMqttReconnectAttemptMs = 0;
      }
    }
    return;
  }

  // 2) Si WiFi OK pero no hay bootstrap (deviceId), reintentar bootstrap
  if (_deviceId.length() == 0) {
    if (!_cfg.api_base || !_cfg.bootstrap_path) return;

    if (now - lastBootstrapAttemptMs > 5000) {
      lastBootstrapAttemptMs = now;
      Serial.println("🔁 Reintentando bootstrap...");
      String deviceUUID;
      if (bootstrapDevice(deviceUUID)) {
        _deviceId = deviceUUID;
        configureMqttAndTopics();
      } else {
        return;
      }
    } else {
      return;
    }
  }

  // 3) MQTT reconnect
  if (!mqtt->connected()) {
    if (now - lastMqttReconnectAttemptMs > 2000) {
      lastMqttReconnectAttemptMs = now;
      ensureMqttConnected();
    }
    return;
  }

  // 4) Mantener MQTT
  mqtt->loop();
}

bool net_isConnected() {
  return (WiFi.status() == WL_CONNECTED) && _deviceId.length() && mqtt->connected();
}

bool net_publishState(const String& vpin, int value) {
  if (!mqtt->connected()) return false;

  StaticJsonDocument<256> doc;
  doc["type"]      = "state";
  doc["tenant_id"] = _cfg.tenant_id;
  doc["device_id"] = _deviceId;     // ✅ UUID del device
  doc["vpin"]      = vpin;
  doc["value"]     = value;

  char out[256];
  size_t n = serializeJson(doc, out, sizeof(out));

  bool retained = false;
  bool ok = mqtt->publish(topicPub.c_str(), (uint8_t*)out, n, retained);

  Serial.print(ok ? "✅ State publicado: " : "❌ Falló publicar state: ");
  Serial.println(out);
  return ok;
}

void net_setPublishAllFn(PublishAllFn fn) {
  _publishAllFn = fn;
}

// =======================
// ✅ NUEVO: set WiFi creds desde BLE
// =======================
void net_setWifiCredentials(const String& ssid, const String& pass, bool persist) {
  if (ssid.length() == 0) return;

  Serial.println("📥 net_setWifiCredentials(): recibidas credenciales WiFi");
  Serial.print("SSID=");
  Serial.println(ssid);

  _wifiSsid = ssid;
  _wifiPass = pass;

  if (persist) {
    nvs_saveWifi(_wifiSsid, _wifiPass);
    Serial.println("💾 WiFi guardado en NVS.");
  }

  // Cortar MQTT actual si existía
  if (mqtt->connected()) mqtt->disconnect();
  _deviceId = ""; // obliga a re-bootstrap con nueva red
  topicPub = "";
  topicSub = "";

  // Conectar WiFi
  bool ok = setupWiFiWithCreds(_wifiSsid, _wifiPass, 20000);
  if (!ok) {
    Serial.println("❌ net_setWifiCredentials(): no se logró conectar WiFi.");
    return;
  }

  setupTimeIfNeeded();

  // Bootstrap + MQTT
  if (_cfg.api_base && _cfg.bootstrap_path) {
    String deviceUUID;
    if (!bootstrapDevice(deviceUUID)) {
      Serial.println("❌ net_setWifiCredentials(): bootstrap falló (sin MQTT).");
      return;
    }

    _deviceId = deviceUUID;
    configureMqttAndTopics();

    if (!ensureMqttConnected()) {
      Serial.println("⚠️ net_setWifiCredentials(): no conectó MQTT (reintentos en loop).");
    }
  } else {
    Serial.println("❌ net_setWifiCredentials(): api_base/bootstrap_path no definidos.");
  }
}

// (Opcional) si luego quieres exponerlo en header:
// void net_clearWifiCredentials() { nvs_clearWifi(); _wifiSsid=""; _wifiPass=""; }