/*************************************************************
 *  ESP32 A + B 功能合并版
 *  A：BLE 收温度 + 读 RSSI + 控制风扇 + UART 发给 B
 *  B：UART 收数据 + MQTT 上传 ThingsBoard
 *************************************************************/

#include <WiFi.h>
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_gap_ble_api.h"

/*****************************************************
 *=============== WiFi & MQTT 配置（B 板功能） ==============
 *****************************************************/
const char* ssid     = "Netcore-DB56EE";
const char* password = "12345678";
// const char* ssid = "zju-test-wifi-2"; 
// const char* password = "1234567890";
const char* mqtt_server = "192.168.100.219";
const int   mqtt_port   = 1883;
const char* mqtt_topic  = "v1/devices/me/telemetry";
const char* TB_TOKEN    = "uoutY77bCKx2Nh0QOFKE";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

/*****************************************************
 *=============== 全局共享数据（A + B 公用） ==============
 *****************************************************/
float g_latestTemp = NAN;
int   g_latestRssi = 127;

volatile bool g_flagTempUpdated = false;
volatile bool g_flagRssiUpdated = false;

/*****************************************************
 *=============== A 板功能：GPIO 控制风扇 ==============
 *****************************************************/
const int controlPin = 4;
const float tempThreshold = 30.0f;

void controlGPIO(float t) {
  if (!isnan(t) && t > tempThreshold) {
    digitalWrite(controlPin, LOW);
    Serial.println("[A] Fan ON (GPIO4 LOW)");
  } else {
    digitalWrite(controlPin, HIGH);
    Serial.println("[A] Fan OFF (GPIO4 HIGH)");
  }
}

/*****************************************************
 *=============== B 板功能：MQTT 连接函数 ==============
 *****************************************************/
void setupWiFi() {
  Serial.print("Connecting to WiFi "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.print("\nWiFi connected, IP=");
  Serial.println(WiFi.localIP());
}

void setupMqtt() {
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setKeepAlive(60);
}

void ensureMqtt() {
  if (mqtt.connected()) return;

  String clientId = "ESP32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to MQTT ... ");

  if (mqtt.connect(clientId.c_str(), TB_TOKEN, NULL)) {
    Serial.println("connected.");
  } else {
    Serial.print("failed, rc="); Serial.println(mqtt.state());
  }
}

/*****************************************************
 *=============== B 板功能：MQTT 上报 ==============
 *****************************************************/
bool publishTempRssi() {
  ensureMqtt();
  if (!mqtt.connected()) return false;

  String payload = "{";
  bool first = true;

  if (!isnan(g_latestTemp)) {
    payload += "\"temperature\":" + String(g_latestTemp, 2);
    first = false;
  }
  if (g_latestRssi < 127) {
    if (!first) payload += ",";
    payload += "\"rssi\":" + String(g_latestRssi);
  }
  payload += "}";

  Serial.print("Publish: "); Serial.println(payload);
  return mqtt.publish(mqtt_topic, payload.c_str());
}

/*****************************************************
 *=============== UART (A->B) ==============
 *****************************************************/
#define UART_TX_PIN 5
#define UART_RX_PIN -1
#define UART_BAUD   115200

HardwareSerial SensorSerial(2);

/*****************************************************
 *=============== A 板 BLE 回调 ==============
 *****************************************************/
static esp_bd_addr_t g_peerAddr = {0};
static bool g_bleConnected = false;
static bool g_hasPeer = false;
unsigned long lastRssiPoll = 0;
const unsigned long rssiPollIntervalMs = 5000;

void gapCallback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (event == ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT) {
    if (param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
      g_latestRssi = param->read_rssi_cmpl.rssi;
      g_flagRssiUpdated = true;
      Serial.printf("[A][BLE] RSSI: %d dBm\n", g_latestRssi);
    }
  }
}

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String s(pChar->getValue().c_str());
    s.trim();
    g_latestTemp = s.toFloat();
    g_flagTempUpdated = true;
    Serial.printf("[A][BLE] Temp recv: %.2f\n", g_latestTemp);
    controlGPIO(g_latestTemp);
  }
};

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t *param) override {
    g_bleConnected = true;
    memcpy(g_peerAddr, param->connect.remote_bda, sizeof(esp_bd_addr_t));
    g_hasPeer = true;
    Serial.println("[A][BLE] Connected");
    esp_ble_gap_read_rssi(g_peerAddr);
  }
  void onDisconnect(BLEServer* server) override {
    g_bleConnected = false;
    Serial.println("[A][BLE] Disconnected, advertising...");
    server->getAdvertising()->start();
    g_latestRssi = -86;      // 离线值
    g_flagRssiUpdated = true;
  }
};

/*****************************************************
 *=============== A 板 → B 板：串口发送 ==============
 *****************************************************/
unsigned long lastSendTs = 0;
const unsigned long sendMinIntervalMs = 800;

void sendLine(float temp, int rssi) {
  unsigned long now = millis();
  if (now - lastSendTs < sendMinIntervalMs) return;
  lastSendTs = now;

  char line[64];
  if (!isnan(temp) && (rssi < 127))
    sprintf(line, "T:%.2f,R:%d\n", temp, rssi);
  else if (!isnan(temp))
    sprintf(line, "T:%.2f\n", temp);
  else if (rssi < 127)
    sprintf(line, "R:%d\n", rssi);
  else return;

  SensorSerial.print(line);
  Serial.print("[A->B] "); Serial.print(line);
}

/*****************************************************
 *=============== B 板：解析 UART 行数据 ==============
 *****************************************************/
void parseLine(const char* s) {
  const char* tptr = strstr(s, "T:");
  const char* rptr = strstr(s, "R:");

  if (tptr) g_latestTemp = atof(tptr + 2);
  if (rptr) g_latestRssi = atoi(rptr + 2);

  Serial.print("[B] Parsed: "); Serial.println(s);

  publishTempRssi();
}

/*****************************************************
 *=============== setup() 整合 ==============
 *****************************************************/
BLECharacteristic* pCharacteristic;
BLEServer* pServer;

void setup() {
  Serial.begin(115200);

  /******** A 板部分 ********/
  pinMode(controlPin, OUTPUT);
  digitalWrite(controlPin, HIGH);

  SensorSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  BLEDevice::init("ESP32B");
  esp_ble_gap_register_callback(gapCallback);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService("6e400003-b5a3-f393-e0a9-e50e24dcca9e");
  pCharacteristic = pService->createCharacteristic(
      "6e400004-b5a3-f393-e0a9-e50e24dcca9e",
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ);
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  BLEDevice::getAdvertising()->start();

  /******** B 板部分 ********/
  setupWiFi();
  setupMqtt();
  ensureMqtt();

  Serial.println("System Ready (A + B merged)");
}

/*****************************************************
 *=============== loop：A + B 双逻辑 ==============
 *****************************************************/
void loop() {

  /*********** MQTT 部分 ***********/
  if (!mqtt.connected()) ensureMqtt();
  else mqtt.loop();

  /*********** BLE 周期 RSSI ***********/
  if (g_bleConnected && g_hasPeer && millis() - lastRssiPoll > rssiPollIntervalMs) {
    lastRssiPoll = millis();
    esp_ble_gap_read_rssi(g_peerAddr);
  }

  /*********** A→B 发送（有更新或心跳） ***********/
  static unsigned long lastHeart = 0;
  if (g_flagTempUpdated || g_flagRssiUpdated || millis() - lastHeart > 15000) {
    sendLine(g_latestTemp, g_latestRssi);
    g_flagTempUpdated = g_flagRssiUpdated = false;
    lastHeart = millis();
  }

  /*********** B 部分：解析 UART ***********/
  static char lineBuf[64];
  static int lineLen = 0;

  while (SensorSerial.available()) {
    char c = (char)SensorSerial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        parseLine(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < 63) {
      lineBuf[lineLen++] = c;
    }
  }

  delay(5);
}
