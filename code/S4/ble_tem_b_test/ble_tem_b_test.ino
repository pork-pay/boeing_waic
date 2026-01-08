#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_gap_ble_api.h"   // GAP RSSI API

/************ 控制参数 ************/
const int   controlPin     = 4;       // 风扇/继电器控制脚
const float tempThreshold  = 30.0f;   // 阈值

/************ 串口（发给 ESP32-B） ************/
// 仅用 TX=GPIO5 发出，RX 不用
// 注意：两板 GND 必须相连
#define UART_BAUD 115200
#define UART_TX_PIN 5
#define UART_RX_PIN -1  // 不用

/************ BLE 配置 ************/
#define DEVICE_NAME "ESP32B_2"
#define SERVICE_UUID        "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID "6e400004-b5a3-f393-e0a9-e50e24dcca9e"

BLECharacteristic* pCharacteristic;
BLEServer*         pServer;

/************ BLE 链接状态 & RSSI 读取 ************/
static esp_bd_addr_t g_peerAddr = {0};
static bool  g_bleConnected  = false;
static bool  g_hasPeer       = false;
unsigned long lastRssiPoll   = 0;
const unsigned long rssiPollIntervalMs = 5000;

/************ 数据：回调更新，定时合并发送 ************/
volatile float g_latestTemp = NAN;
volatile int   g_latestRssi = 127;   // 127=无效
volatile bool  g_flagTempUpdated = false;
volatile bool  g_flagRssiUpdated = false;

unsigned long  lastSendTs = 0;       // 发送节流
const unsigned long sendMinIntervalMs = 800;  // 至少间隔 0.8s 发一次

/************ 小工具 ************/
static void logf(const char* fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.println(buf);
}

void controlGPIO(float t) {
  if (!isnan(t) && t > tempThreshold) {
    digitalWrite(controlPin, LOW);
    Serial.println("[A] Fan ON (GPIO4 LOW)");
  } else {
    digitalWrite(controlPin, HIGH);
    Serial.println("[A] Fan OFF (GPIO4 HIGH)");
  }
}

/************ 向 ESP32-B 发送一行 "T:xx.xx,R:yy" ************/
void sendLine(float temp, int rssi) {
  // 节流：避免过于频繁
  unsigned long now = millis();
  if (now - lastSendTs < sendMinIntervalMs) return;
  lastSendTs = now;

  char line[64];
  bool hasT = !isnan(temp);
  bool hasR = (rssi < 127);

  if (hasT && hasR) {
    snprintf(line, sizeof(line), "T:%.2f,R:%d\n", temp, rssi);
  } else if (hasT) {
    snprintf(line, sizeof(line), "T:%.2f\n", temp);
  } else if (hasR) {
    snprintf(line, sizeof(line), "R:%d\n", rssi);
  } else {
    return;
  }
  Serial2.print(line);   // 发给 B 板
  Serial.print("[A->B] "); Serial.print(line);
}

/************ GAP 回调：只更新RSSI，不做网络 ************/
void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (event == ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT) {
    if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
      g_latestRssi = param->read_rssi_cmpl.rssi;
      g_flagRssiUpdated = true;
      logf("[A][BLE] RSSI: %d dBm", g_latestRssi);
    } else {
      logf("[A][BLE] RSSI read failed, status=%d", param->read_rssi_cmpl.status);
    }
  }
}

/************ GATT 写回调：收到温度，做本地控制 & 标记 ************/
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    std::string value = pChar->getValue();
    String s(value.c_str()); s.trim();
    float t = s.toFloat();
    g_latestTemp = t;
    g_flagTempUpdated = true;
    logf("[A][BLE] Temperature recv: %.2f", t);
    controlGPIO(t);
  }
};

/************ Server 连接回调：保持“粘住” ************/
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t *param) override {
    g_bleConnected = true;
    memcpy(g_peerAddr, param->connect.remote_bda, sizeof(esp_bd_addr_t));
    g_hasPeer = true;

    // 连接参数更宽容
    esp_ble_conn_update_params_t cp{};
    memcpy(cp.bda, g_peerAddr, sizeof(esp_bd_addr_t));
    cp.min_int = 24;    // 30ms
    cp.max_int = 40;    // 50ms
    cp.latency = 0;
    cp.timeout = 600;   // 6s
    esp_ble_gap_update_conn_params(&cp);

    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
            g_peerAddr[0], g_peerAddr[1], g_peerAddr[2],
            g_peerAddr[3], g_peerAddr[4], g_peerAddr[5]);
    logf("[A][BLE] Connected, peer=%s", macStr);

    if (g_hasPeer) esp_ble_gap_read_rssi(g_peerAddr);
  }

  void onDisconnect(BLEServer* server) override {
    g_bleConnected = false;
    logf("[A][BLE] Disconnected → advertising...");
    server->getAdvertising()->start();

    // 标注一个离线 RSSI 触发一次发送
    g_latestRssi = -86;
    g_flagRssiUpdated = true;
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("[A] Boot");

  pinMode(controlPin, OUTPUT);
  digitalWrite(controlPin, HIGH);

  // 串口：TX=GPIO5（发给 B）
  Serial2.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  // BLE
  BLEDevice::init(DEVICE_NAME);
  esp_ble_gap_register_callback(gapCallback);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ);
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("[A] BLE advertising started.");
}

void loop() {
  // 周期读 RSSI
  if (g_bleConnected && g_hasPeer && (millis() - lastRssiPoll > rssiPollIntervalMs)) {
    lastRssiPoll = millis();
    esp_ble_gap_read_rssi(g_peerAddr); // 回调里只更新
  }

  // 有新温度 / 新RSSI / 定时心跳 → 发送到 B
  static unsigned long lastHeart = 0;
  bool needSend = false;
  if (g_flagTempUpdated || g_flagRssiUpdated) needSend = true;
  else if (millis() - lastHeart > 15000) { needSend = true; lastHeart = millis(); }

  if (needSend) {
    sendLine(g_latestTemp, g_latestRssi);
    g_flagTempUpdated = g_flagRssiUpdated = false;
  }

  delay(2);
  yield();
}
