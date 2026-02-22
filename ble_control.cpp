#include "ble_control.h"
#include <NimBLEDevice.h>

#if __has_include(<NimBLEConnInfo.h>)
  #include <NimBLEConnInfo.h>
  #define HAS_NIMBLE_CONNINFO 1
#else
  #define HAS_NIMBLE_CONNINFO 0
#endif

static NimBLEServer*          g_server = nullptr;
static NimBLECharacteristic*  g_char   = nullptr;

static volatile bool g_connected = false;
static bool g_oldConnected = false;

static BleOnWriteFn g_onWrite = nullptr;

// para reintentar advertising si algo lo tumba
static unsigned long g_lastAdvKickMs = 0;

static void ble_tx_notify(const char* msg) {
  if (!g_char) return;
  g_char->setValue((uint8_t*)msg, strlen(msg));
  g_char->notify();
}

class ServerCallbacks : public NimBLEServerCallbacks {
public:
#if HAS_NIMBLE_CONNINFO
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    (void)s; (void)connInfo;
    g_connected = true;
    Serial.println("[BLE] Cliente conectado");

    // ✅ Señal a tu app
    if (g_char) {
      ble_tx_notify("READY");
      Serial.println("[BLE] TX notify: READY");
    }
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    (void)s; (void)connInfo; (void)reason;
    g_connected = false;
    Serial.println("[BLE] Cliente desconectado");
    NimBLEDevice::startAdvertising();
  }
#else
  void onConnect(NimBLEServer* s) override {
    (void)s;
    g_connected = true;
    Serial.println("[BLE] Cliente conectado");

    if (g_char) {
      ble_tx_notify("READY");
      Serial.println("[BLE] TX notify: READY");
    }
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
    value.trim();

    Serial.print("[BLE] RX: ");
    Serial.println(value);

    // 1) Tu callback app-level
    if (g_onWrite) g_onWrite(value);

    // 2) Ping-pong simple
    if (value.equalsIgnoreCase("PING")) {
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
    value.trim();

    Serial.print("[BLE] RX: ");
    Serial.println(value);

    if (g_onWrite) g_onWrite(value);

    if (value.equalsIgnoreCase("PING")) {
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

  // ✅ MTU más grande para payloads (WiFi provisioning, JSON, etc.)
  // (No rompe si el peer no lo soporta; se negocia)
  NimBLEDevice::setMTU(185);

  // potencia alta para que sea visible en scan
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

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

  // -------- Advertising (con NAME + UUID) --------
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();

  NimBLEAdvertisementData ad;
  ad.setFlags(0x06); // LE General Discoverable + BR/EDR not supported
  ad.setName(deviceName);
  ad.addServiceUUID(NimBLEUUID(serviceUUID));
  adv->setAdvertisementData(ad);

  NimBLEAdvertisementData sd;
  sd.setName(deviceName);
  adv->setScanResponseData(sd);

  adv->setMinInterval(0x20);
  adv->setMaxInterval(0x40);

  adv->start();

  Serial.println("[BLE] Advertising iniciado (con Name + ScanResponse)");
  g_lastAdvKickMs = millis();
  return true;
}

void ble_loop() {
  // heartbeat cada 3s
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

  // si algo tumbó el advertising (WiFi/TLS), lo “kickeamos” cada 5s si no hay conexión
  if (!g_connected) {
    unsigned long now = millis();
    if (now - g_lastAdvKickMs > 5000) {
      g_lastAdvKickMs = now;
      NimBLEDevice::startAdvertising();
      Serial.println("[BLE] Advertising kick (keep-alive)");
    }
  }

  // lógica old/new
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