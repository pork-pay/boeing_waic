// ---------- ESP32-A (Client) ----------
// 作用：采集 BMP180 温度，每秒写入 BLE Server (ESP32-B)
// 适配版本：ESP32 Arduino Core 2.0.9 + BLE库 (1.0.x)

#include <Wire.h>
#include <Adafruit_BMP085.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEClient.h>

#define DEVICE_NAME "ESP32A_2"
#define PEER_NAME   "ESP32B_2"  // 目标外设名
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
      if (client) { 
        try { 
          client->disconnect(); 
        } catch(...) {} 
        client = nullptr; 
      }
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
        if (!ok) { 
          client->disconnect(); 
        }
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
  Wire.begin(4, 5);


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
    if (client) { 
      try { 
        client->disconnect(); 
      } catch(...) {} 
      client = nullptr; 
    }
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
    // 修改点：ESP32 Arduino Core 2.0.9 的 BLE 库中，
    // writeValue 返回 void，不能赋值给 bool
    pChar->writeValue((uint8_t*)msg.c_str(), msg.length(), /*response*/ true);
    
    // 简单延迟后检查连接状态，作为发送是否健康的参考
    delay(10);
    if (client && client->isConnected()) {
      Serial.println("Sent: " + msg);
    } else {
      Serial.println("Write may have failed or connection lost!");
      connected = false;
      if (client) { 
        try { 
          client->disconnect(); 
        } catch(...) {} 
        client = nullptr; 
      }
    }
  }

  delay(1000); // 每秒一次
}