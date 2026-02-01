#include "ble_control.h"
#include <NimBLEDevice.h>

// Detectar si tu NimBLE trae NimBLEConnInfo.h (muchas versiones nuevas sí)
#if __has_include(<NimBLEConnInfo.h>)
  #include <NimBLEConnInfo.h>
  #define HAS_NIMBLE_CONNINFO 1
#else
  #define HAS_NIMBLE_CONNINFO 0
#endif

// Globals
static NimBLEServer*          g_server = nullptr;
static NimBLECharacteristic*  g_char   = nullptr;

static volatile bool g_connected = false;
static bool g_oldConnected = false;

static BleOnWriteFn g_onWrite = nullptr;

// =======================
// Callbacks
// =======================
class ServerCallbacks : public NimBLEServerCallbacks {
public:
#if HAS_NIMBLE_CONNINFO
  // Variantes nuevas (con ConnInfo)
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    (void)s; (void)connInfo;
    g_connected = true;
    Serial.println("[BLE] Cliente conectado");
  }

  // Algunas versiones traen reason
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    (void)s; (void)connInfo; (void)reason;
    g_connected = false;
    Serial.println("[BLE] Cliente desconectado");
    NimBLEDevice::startAdvertising();
  }
#else
  // Variantes antiguas (simples)
  void onConnect(NimBLEServer* s) override {
    (void)s;
    g_connected = true;
    Serial.println("[BLE] Cliente conectado");
  }

  void onDisconnect(NimBLEServer* s) override {
    (void)s;
    g_connected = false;
    Serial.println("[BLE] Cliente desconectado");
    NimBLEDevice::startAdvertising();
  }
#endif
};

class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
public:
#if HAS_NIMBLE_CONNINFO
  void onWrite(NimBLECharacteristic* ch, NimBLEConnInfo& connInfo) override {
    (void)connInfo;

    std::string v = ch->getValue();
    if (v.empty()) return;

    String value(v.c_str());
    Serial.print("[BLE] RX: ");
    Serial.println(value);

    if (g_onWrite) g_onWrite(value);

    if (value == "PING") {
      const char* resp = "PONG";
      ch->setValue((uint8_t*)resp, strlen(resp));
      ch->notify();
      Serial.println("[BLE] TX notify: PONG");
    }
  }
#else
  void onWrite(NimBLECharacteristic* ch) override {
    std::string v = ch->getValue();
    if (v.empty()) return;

    String value(v.c_str());
    Serial.print("[BLE] RX: ");
    Serial.println(value);

    if (g_onWrite) g_onWrite(value);

    if (value == "PING") {
      const char* resp = "PONG";
      ch->setValue((uint8_t*)resp, strlen(resp));
      ch->notify();
      Serial.println("[BLE] TX notify: PONG");
    }
  }
#endif
};

bool ble_begin(const char* deviceName,
               const char* serviceUUID,
               const char* characteristicUUID,
               BleOnWriteFn onWrite) {
  g_onWrite = onWrite;

  NimBLEDevice::init(deviceName);

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = g_server->createService(serviceUUID);

  g_char = svc->createCharacteristic(
    characteristicUUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::NOTIFY
  );

  g_char->setCallbacks(new CharacteristicCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(serviceUUID);
  // En algunas versiones no existe setScanResponse(true), así que no lo usamos.
  adv->start();

  Serial.println("[BLE] Advertising iniciado");
  return true;
}

void ble_loop() {
  // Heartbeat cada 3s sin delay bloqueante
  static uint32_t counter = 0;
  static unsigned long lastHbMs = 0;

  if (g_connected && g_char) {
    unsigned long now = millis();
    if (now - lastHbMs >= 3000) {
      lastHbMs = now;

      char msg[64];
      snprintf(msg, sizeof(msg), "{\"heartbeat\":%lu}", (unsigned long)counter++);

      g_char->setValue((uint8_t*)msg, strlen(msg));
      g_char->notify();

      Serial.print("[BLE] Notify: ");
      Serial.println(msg);
    }
  }

  // Mantener lógica old/new
  if (!g_connected && g_oldConnected) {
    delay(50);
    NimBLEDevice::startAdvertising();
    Serial.println("[BLE] Restart advertising");
    g_oldConnected = g_connected;
  }

  if (g_connected && !g_oldConnected) {
    g_oldConnected = g_connected;
  }
}

void ble_notify(const String& msg) {
  if (!g_connected || !g_char) return;
  g_char->setValue((uint8_t*)msg.c_str(), msg.length());
  g_char->notify();
}

bool ble_isConnected() {
  return g_connected;
}