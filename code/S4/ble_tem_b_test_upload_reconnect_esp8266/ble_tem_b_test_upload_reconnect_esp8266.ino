#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

// ================== 串口（接 A 板）==================
#define SENSOR_RX 4  // D2对应的GPIO4
SoftwareSerial SensorSerial;

// ================== WiFi 配置 ==================
const char* ssid     = "zju-test-wifi-5";
const char* password = "1234567890";

// ================== MQTT 配置 ==================
const char* mqtt_server = "192.168.100.219";
const int   mqtt_port   = 1883;
const char* mqtt_topic  = "v1/devices/me/telemetry";
const char* TB_TOKEN    = "uoutY77bCKx2Nh0QOFKE";

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ================== 数据结构 ==================
struct BleData {
  float temperature;
  int   rssi;
  unsigned long timestamp;
};

// ================== 上传控制 ==================
unsigned long lastPublishTime = 0;
const unsigned long PUBLISH_INTERVAL = 1000;  // 1秒上传间隔
bool mqttNeedsReconnect = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_COOLDOWN = 3000;  // 重连冷却时间3秒

// ================== 数据缓存 ==================
BleData latestData = {NAN, 127, 0};
BleData lastGoodData = {NAN, 127, 0};
bool hasGoodData = false;
int dataCount = 0;

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
  
  String clientId = "ESP8266-BLE-" + String(ESP.getChipId(), HEX);
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
    return true;
  }
  
  return false;
}

// ================== 快速发布函数（1秒间隔）==================
bool fastPublish(float temperature, int rssi) {
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
  
  // 快速构建JSON
  char payload[80];
  char tempStr[10];
  
  dtostrf(temperature, 4, 2, tempStr);
  
  snprintf(payload, sizeof(payload),
           "{\"temperature\":%s,\"rssi\":%d}",
           tempStr, rssi);
  
  // 发布
  bool ok = mqtt.publish(mqtt_topic, payload);
  
  if (ok) {
    lastPublishTime = now;
    // 显示成功信息（简洁格式）
    Serial.printf("[OK] T=%s R=%d\n", tempStr, rssi);
  } else {
    Serial.println("Publish FAILED");
    mqttNeedsReconnect = true;
    
    // 发布失败时立即断开
    if (mqtt.connected()) {
      mqtt.disconnect();
    }
  }
  
  return ok;
}

// ================== 解析蓝牙数据 ==================
void parseBleData(const char* line) {
  const char* tptr = strstr(line, "T:");
  const char* rptr = strstr(line, "R:");

  float temp = NAN;
  int rssi = 127;
  bool updated = false;

  if (tptr) {
    temp = atof(tptr + 2);
    updated = true;
  }
  
  if (rptr) {
    rssi = atoi(rptr + 2);
    updated = true;
  }

  if (updated) {
    latestData.temperature = temp;
    latestData.rssi = rssi;
    latestData.timestamp = millis();
    
    // 保存最后的好数据
    if (!isnan(temp) || rssi < 127) {
      lastGoodData = latestData;
      hasGoodData = true;
      dataCount++;
    }
    
    // 显示解析结果（每10次显示一次）
    if (dataCount % 10 == 1) {
      if (!isnan(temp) && rssi < 127) {
        Serial.printf("[BLE#%d] T=%.2f R=%d\n", dataCount, temp, rssi);
      } else if (!isnan(temp)) {
        Serial.printf("[BLE#%d] T=%.2f\n", dataCount, temp);
      } else if (rssi < 127) {
        Serial.printf("[BLE#%d] R=%d\n", dataCount, rssi);
      }
    }
    
    // 立即尝试1秒上传
    fastPublish(temp, rssi);
  }
}

// ================== setup() ==================
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println("\n\n==================================");
  Serial.println("   ESP8266 蓝牙数据1秒上传");
  Serial.println("==================================");
  
  // 初始化软件串口
  SensorSerial.begin(115200, SWSERIAL_8N1, SENSOR_RX, -1, false, 256);
  
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
  
  // 3. 处理蓝牙串口数据
  static char lineBuffer[64];
  static int linePos = 0;
  static int lineCount = 0;

  while (SensorSerial.available()) {
    char c = (char)SensorSerial.read();
    
    if (c == '\n' || c == '\r') {
      if (linePos > 0) {
        lineBuffer[linePos] = '\0';
        lineCount++;
        
        // 解析数据
        parseBleData(lineBuffer);
        linePos = 0;
      }
    } else {
      if (linePos < (int)sizeof(lineBuffer) - 1) {
        lineBuffer[linePos++] = c;
      } else {
        linePos = 0;  // 缓冲区溢出，重置
      }
    }
  }
  
  // 4. 定时上传（确保1秒节奏）
  static unsigned long lastTickTime = 0;
  if (now - lastTickTime > 1000) {
    lastTickTime = now;
    
    // 用最后的好数据尝试上传
    if (hasGoodData && (!isnan(lastGoodData.temperature) || lastGoodData.rssi < 127)) {
      fastPublish(lastGoodData.temperature, lastGoodData.rssi);
    }
  }
  
  // 5. 状态报告（每分钟一次）
  static unsigned long lastStatusTime = 0;
  if (now - lastStatusTime > 60000) {
    lastStatusTime = now;
    
    Serial.println("\n=== 状态报告 ===");
    Serial.printf("WiFi: %s (RSSI: %d)\n", 
                  WiFi.status() == WL_CONNECTED ? "已连接" : "断开",
                  WiFi.RSSI());
    Serial.printf("MQTT: %s\n", 
                  mqtt.connected() ? "已连接" : "断开");
    Serial.printf("需要重连: %s\n", mqttNeedsReconnect ? "是" : "否");
    Serial.printf("数据总数: %d\n", dataCount);
    Serial.printf("最后数据: ");
    
    if (!isnan(lastGoodData.temperature) && lastGoodData.rssi < 127) {
      Serial.printf("T=%.2f R=%d", lastGoodData.temperature, lastGoodData.rssi);
    } else if (!isnan(lastGoodData.temperature)) {
      Serial.printf("T=%.2f", lastGoodData.temperature);
    } else if (lastGoodData.rssi < 127) {
      Serial.printf("R=%d", lastGoodData.rssi);
    } else {
      Serial.printf("无有效数据");
    }
    
    Serial.printf(" (%d秒前)\n", lastGoodData.timestamp > 0 ? (now - lastGoodData.timestamp) / 1000 : -1);
    Serial.printf("运行时间: %.1f 分钟\n", now / 60000.0);
    Serial.println("================\n");
  }
  
  // 6. 最小延迟
  delay(1);
}