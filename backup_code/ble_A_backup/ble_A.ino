// ---------- ESP32-A (Client) ----------
// 作用：采集 BMP180 温度，每秒写入 BLE Server (ESP32-B)

#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEScan.h>

#define DEVICE_NAME "ESP32A"
#define PEER_NAME   "ESP32B"  // 目标外设名
#define SERVICE_UUID        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID "6e400004-b5a3-f393-e0a9-e50e24dcca9e"

Adafruit_BMP085 bmp;

BLEClient* client = nullptr;
BLERemoteCharacteristic* pChar = nullptr;
bool connected = false;

bool connectToPeer() {
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  BLEScanResults results = scan->start(5, false);

  bool ok = false;
  for (int i = 0; i < results.getCount(); ++i) {
    BLEAdvertisedDevice d = results.getDevice(i);
    if (d.getName() == PEER_NAME) {
      Serial.println("Found peer, connecting...");
      if (client) { try { client->disconnect(); } catch(...) {} BLEDevice::deleteClient(client); client = nullptr; }
      client = BLEDevice::createClient();

      if (client->connect(&d)) {
        BLERemoteService* svc = client->getService(SERVICE_UUID);
        if (svc) {
          pChar = svc->getCharacteristic(CHARACTERISTIC_UUID);
          if (pChar && pChar->canWrite()) {
            connected = true;
            ok = true;
            Serial.println("BLE connected & characteristic ready.");
          }
        }
        if (!ok) { client->disconnect(); }
      }
      break;
    }
  }
  scan->clearResults();
  return ok;
}

void setup() {
  Serial.begin(115200);
  // 建议使用默认 I2C (GPIO21/22)，你的板子若固定 4/5 就保留：
  // Wire.begin(4, 5);
  Wire.begin();

  if (!bmp.begin()) {
    Serial.println("BMP180 init failed!");
    while (1) delay(1000);
  }

  BLEDevice::init(DEVICE_NAME);
  Serial.println("BLE client initialized");

  // 首次尝试连接
  connectToPeer();
}

void loop() {
  // 断线自愈
  if (connected && client && !client->isConnected()) {
    Serial.println("Peer lost. Reconnecting...");
    connected = false;
    pChar = nullptr;
    if (client) { try { client->disconnect(); } catch(...) {} BLEDevice::deleteClient(client); client = nullptr; }
  }
  if (!connected) {
    connectToPeer();
    delay(500);
    return;
  }

  // 采集 & 写入（带响应，保证稳定）
  float temp = bmp.readTemperature();  // °C
  String msg = String(temp, 2);        // "25.43"
  if (pChar) {
    bool ok = pChar->writeValue((uint8_t*)msg.c_str(), msg.length(), /*response*/ true);
    Serial.println(ok ? ("Sent: " + msg) : "Write failed!");
    if (!ok) {
      // 写失败也触发重连
      connected = false;
      try { client->disconnect(); } catch(...) {}
      BLEDevice::deleteClient(client); client = nullptr;
    }
  }

  delay(1000); // 每秒一次
}
