#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

// ================== 串口配置 ==================
#define SENSOR_RX 13  // GPIO13/D7
SoftwareSerial SensorSerial;

// ================== WiFi 配置 ==================
const char* ssid     = "zju2-wifi-2";
const char* password = "1234567890";

// ================== MQTT 配置 ==================
const char* mqtt_server = "192.168.100.121";
const int   mqtt_port   = 1883;
const char* mqtt_topic  = "v1/devices/me/telemetry";
const char* TB_TOKEN    = "J3bX1e0VliJCo1vUhmzv";

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ================== 数据结构 ==================
struct SensorData {
  float    temperature;
  float    humidity;
  uint32_t pressure;
  uint16_t pm25;
};

// ================== 上传控制 ==================
unsigned long lastPublishTime = 0;
const unsigned long PUBLISH_INTERVAL = 1000;  // 1秒上传间隔
bool mqttNeedsReconnect = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_COOLDOWN = 3000;  // 重连冷却时间3秒

// ================== WiFi 连接 ==================
void setupWiFi() {
  Serial.print("Connecting to WiFi ");
  Serial.print(ssid);
  Serial.println(" ...");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed");
  }
}

// ================== 快速MQTT连接 ==================
bool quickMQTTConnect() {
  if (mqtt.connected()) return true;
  
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  
  String clientId = "ESP8266-" + String(ESP.getChipId(), HEX);
  mqtt.setServer(mqtt_server, mqtt_port);
  
  return mqtt.connect(clientId.c_str(), TB_TOKEN, NULL);
}

// ================== 带冷却的重连 ==================
bool reconnectIfNeeded() {
  if (!mqttNeedsReconnect) return mqtt.connected();
  
  unsigned long now = millis();
  if (now - lastReconnectAttempt < RECONNECT_COOLDOWN) {
    return mqtt.connected();
  }
  
  lastReconnectAttempt = now;
  
  if (quickMQTTConnect()) {
    mqttNeedsReconnect = false;
    Serial.println("MQTT reconnected");
    return true;
  }
  
  return false;
}

// ================== 快速发布函数（1秒间隔）==================
bool fastPublish(float temperature, float humidity, uint32_t pressure, uint16_t pm25) {
  unsigned long now = millis();
  
  // 严格1秒间隔控制
  if (now - lastPublishTime < PUBLISH_INTERVAL) {
    return false;
  }
  
  // 快速连接检查
  if (!mqtt.connected()) {
    if (!quickMQTTConnect()) {
      mqttNeedsReconnect = true;
      return false;
    }
  }
  
  // 快速构建JSON（最小化字符串操作）
  char payload[80];
  char tempStr[10], humiStr[10];
  
  dtostrf(temperature, 4, 1, tempStr);
  dtostrf(humidity, 4, 1, humiStr);
  
  snprintf(payload, sizeof(payload),
           "{\"temperature\":%s,\"humidity\":%s,\"pm25\":%u,\"air_pressure\":%lu}",
           tempStr, humiStr, pm25, pressure);
  
  // 发布并计时
  unsigned long publishStart = millis();
  bool ok = mqtt.publish(mqtt_topic, payload);
  unsigned long publishTime = millis() - publishStart;
  
  if (ok) {
    lastPublishTime = now;
    Serial.printf("[OK +%lums] T=%s H=%s PM=%u\n", 
                  now - lastPublishTime, tempStr, humiStr, pm25);
    
    // 如果发布耗时过长，警告
    if (publishTime > 100) {
      Serial.printf("Warning: Publish took %lums\n", publishTime);
    }
  } else {
    Serial.println("Publish FAILED");
    mqttNeedsReconnect = true;
    
    // 发布失败时立即断开，避免下次使用无效连接
    if (mqtt.connected()) {
      mqtt.disconnect();
    }
  }
  
  return ok;
}

// ================== 解析函数 ==================
bool parseSensorFrame(const uint8_t* data, SensorData* out) {
  if (!data || !out) return false;
  if (data[0] != 0x01 || data[1] != 0x03) return false;

  float     temp = (data[13] * 256 + data[14]) / 100.0f;
  float     humi = (data[11] * 256 + data[12]) / 100.0f;
  uint16_t  pm   =  (uint16_t)(data[9]  * 256 + data[10]);
  uint32_t  p    =  ((uint32_t)data[23] << 24) |
                    ((uint32_t)data[24] << 16) |
                    ((uint32_t)data[25] << 8 ) |
                     (uint32_t)data[26];

  out->temperature = roundf(temp * 10.0f) / 10.0f;
  out->humidity    = roundf(humi * 10.0f) / 10.0f;
  out->pm25        = pm;
  out->pressure    = p;
  
  return true;
}

// ================== setup() ==================
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println("\n\n==================================");
  Serial.println("   ESP8266 1秒快速上传环境传感器");
  Serial.println("==================================");
  
  // 初始化软件串口
  SensorSerial.begin(9600, SWSERIAL_8N1, SENSOR_RX, -1, false, 256);
  
  // 初始化网络
  setupWiFi();
  
  // 初始MQTT连接
  if (quickMQTTConnect()) {
    Serial.println("MQTT connected");
  }
  
  Serial.println("\n系统就绪，开始1秒/次数据上传");
  Serial.println("==================================\n");
  
  // 记录初始时间
  lastPublishTime = millis();
}

// ================== 主循环 ==================
void loop() {
  unsigned long now = millis();
  
  // 1. MQTT处理（快速）
  if (mqtt.connected()) {
    mqtt.loop();
  }
  
  // 2. 重连检查（每秒一次）
  static unsigned long lastReconnectCheck = 0;
  if (now - lastReconnectCheck > 1000) {
    lastReconnectCheck = now;
    reconnectIfNeeded();
  }
  
  // 3. 处理传感器数据（最高优先级）
  static uint8_t buffer[29];
  static int pos = 0;
  static int frameCount = 0;
  static unsigned long lastFrameTime = 0;
  static SensorData lastGoodData = {NAN, NAN, 0, 0};

  while (SensorSerial.available()) {
    uint8_t b = SensorSerial.read();
    
    // 快速帧同步
    if (pos == 0 && b != 0x01) continue;
    if (pos == 1 && b != 0x03) { 
      pos = 0; 
      continue; 
    }
    
    buffer[pos++] = b;
    
    // 完整帧
    if (pos >= 29) {
      frameCount++;
      
      SensorData d;
      if (parseSensorFrame(buffer, &d)) {
        // 计算帧间隔
        unsigned long frameInterval = (lastFrameTime > 0) ? now - lastFrameTime : 0;
        lastFrameTime = now;
        lastGoodData = d;  // 保存最后的好数据
        
        // 显示帧信息（减少输出频率）
        if (frameCount % 20 == 1) {
          Serial.printf("[Frame#%d +%lums]\n", frameCount, frameInterval);
        }
        
        // 立即尝试1秒上传
        fastPublish(d.temperature, d.humidity, d.pressure, d.pm25);
      }
      
      pos = 0;
    }
  }
  
  // 4. 定时上传（确保1秒节奏）
  static unsigned long lastTickTime = 0;
  if (now - lastTickTime > 1000) {
    lastTickTime = now;
    
    // 用最后的好数据尝试上传
    if (!isnan(lastGoodData.temperature)) {
      fastPublish(lastGoodData.temperature, lastGoodData.humidity, 
                  lastGoodData.pressure, lastGoodData.pm25);
    }
  }
  
  // 5. 最小延迟
  delay(1);
}