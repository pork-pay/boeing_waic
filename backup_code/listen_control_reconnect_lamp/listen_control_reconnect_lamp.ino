#include <WiFi.h>
#include <PubSubClient.h>

/* ================== LED 与串口 ================== */
const int ledPin = 4;  // GPIO4，LOW=灭，HIGH=亮

/* ================== WiFi 配置 ================== */
const char* ssid     = "Netcore-DB56EE";
const char* password = "12345678";

/* ================== MQTT / ThingsBoard 配置 ================== */
const char* mqtt_server = "192.168.100.219";     // TB 服务器
const int   mqtt_port   = 1883;                  // 1883
const char* mqtt_topic  = "v1/devices/me/telemetry";
const char* TB_TOKEN    = "VG0qV4GynNvI3y7V7Ckx"; // 设备 Token
const char* mqtt_sub_topic = "v1/devices/me/rpc/request/+"; // 订阅控制主题

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

/* ================== 状态变量 ================== */
uint8_t ledState = 0;        // 0=灭，1=亮（物理状态）
uint8_t lastSent = 255;      // 上次已上报的状态
bool mqttNeedsReconnect = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_COOLDOWN = 5000;  // 重连冷却时间5秒

/* ================== 心跳控制 ================== */
unsigned long lastKeepaliveMs = 0;   
const unsigned long KEEPALIVE_INTERVAL_MS = 30000;  // 30s 心跳
unsigned long lastPublishTime = 0;
const unsigned long PUBLISH_INTERVAL = 1000;  // 1秒发布间隔（避免频繁发布）

/* ================== MQTT回调函数（接收云端控制）================== */
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  
  // 将payload转为字符串
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';
  
  Serial.println(message);
  
  // 解析JSON消息，查找"method"字段
  // 简单解析：查找"setLed"或"setValue"
  String msg = String(message);
  
  if (msg.indexOf("\"method\":\"setLed\"") >= 0 || 
      msg.indexOf("\"method\":\"setValue\"") >= 0) {
    
    // 查找"params"字段的值
    int paramsStart = msg.indexOf("\"params\":");
    if (paramsStart >= 0) {
      paramsStart += 9; // 移动到值的位置
      int paramsEnd = msg.indexOf("}", paramsStart);
      if (paramsEnd > paramsStart) {
        String paramValue = msg.substring(paramsStart, paramsEnd);
        paramValue.trim();
        
        Serial.print("Parsed param value: ");
        Serial.println(paramValue);
        
        // 根据参数值控制灯
        if (paramValue == "true" || paramValue == "1" || paramValue == "\"on\"") {
          digitalWrite(ledPin, HIGH);
          ledState = 1;
          Serial.println("云端控制 -> 灯亮");
        } else if (paramValue == "false" || paramValue == "0" || paramValue == "\"off\"") {
          digitalWrite(ledPin, LOW);
          ledState = 0;
          Serial.println("云端控制 -> 灯灭");
        }
      }
    }
  }
}

/* ================== 快速MQTT连接 ================== */
bool quickMQTTConnect() {
  if (mqtt.connected()) return true;
  
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  
  Serial.print("Connecting MQTT... ");
  
  String clientId = "ESP32-LAMP-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  
  bool connected = mqtt.connect(
    clientId.c_str(),
    TB_TOKEN,
    NULL,
    NULL,
    0,
    0,
    NULL,
    true
  );
  
  if (connected) {
    Serial.println("connected");
    
    // 订阅控制主题
    if (mqtt.subscribe(mqtt_sub_topic)) {
      Serial.print("Subscribed to: ");
      Serial.println(mqtt_sub_topic);
    }
    
    mqttNeedsReconnect = false;
    return true;
  } else {
    Serial.print("failed, rc=");
    Serial.println(mqtt.state());
    mqttNeedsReconnect = true;
    return false;
  }
}

/* ================== 带冷却的重连 ================== */
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

/* ================== WiFi连接 ================== */
void setupWiFi() {
  WiFi.mode(WIFI_STA);
  Serial.print("Connecting to WiFi ");
  Serial.print(ssid);
  Serial.println(" ...");
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println("\nWiFi connection failed");
  }
}

/* ================== 发布状态函数（带重连）================== */
bool publishTeng(uint8_t physicalState) {
  unsigned long now = millis();
  
  // 1. 检查发布间隔
  if (now - lastPublishTime < PUBLISH_INTERVAL) {
    return false;
  }
  
  // 2. 确保MQTT连接
  if (!quickMQTTConnect()) {
    mqttNeedsReconnect = true;
    Serial.println("MQTT not ready, skip publish");
    return false;
  }
  
  // 3. 关键修改：物理状态与teng值映射
  // 物理亮(1) -> teng=0 (Dashboard显示红点)
  // 物理灭(0) -> teng=1 (Dashboard显示灰点)
  uint8_t tengValue = (physicalState == 0) ? 1 : 0;
  
  // 4. 快速构建JSON
  char payload[50];
  snprintf(payload, sizeof(payload), "{\"TENG\":%d}", tengValue);
  
  Serial.print("物理灯状态: ");
  Serial.print(physicalState == 1 ? "亮" : "灭");
  Serial.print(" -> 发送TENG: ");
  Serial.print(tengValue);
  Serial.print(" -> ");
  
  // 5. 发布
  bool ok = mqtt.publish(mqtt_topic, payload);
  
  if (ok) {
    Serial.println("SUCCESS");
    lastPublishTime = now;
    return true;
  } else {
    Serial.println("FAILED");
    mqttNeedsReconnect = true;
    
    // 发布失败时立即断开
    if (mqtt.connected()) {
      mqtt.disconnect();
    }
    
    return false;
  }
}

/* ================== Arduino 标准入口 ================== */
void setup() {
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);     // 初始灯灭
  ledState = 0;

  Serial.begin(115200);
  delay(100);
  
  Serial.println("\n\n==================================");
  Serial.println("   ESP32 智能灯控系统 - 增强版");
  Serial.println("==================================");
  Serial.println("串口发 '1' 亮, '0' 灭");
  Serial.println("映射关系: 物理亮(1) -> teng=0, 物理灭(0) -> teng=1");
  Serial.println("支持云端控制和自动重连");
  Serial.println("==================================\n");

  // 初始化网络
  setupWiFi();
  
  // 初始MQTT连接
  if (quickMQTTConnect()) {
    Serial.println("MQTT initialized");
  }
  
  // 上电后先上报一次初始状态
  publishTeng(ledState);
  lastSent = ledState;
  lastKeepaliveMs = millis();
  lastPublishTime = millis();
}

/* ================== 主循环 ================== */
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
  
  // 3. 读取串口：'1'->亮，'0'->灭
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '1') {
      digitalWrite(ledPin, HIGH);
      ledState = 1;
      Serial.println("收到 1 -> 灯亮");
    } else if (c == '0') {
      digitalWrite(ledPin, LOW);
      ledState = 0;
      Serial.println("收到 0 -> 灯灭");
    } else if (c == 's') {
      // 手动触发状态上报
      Serial.println("手动触发状态上报");
      publishTeng(ledState);
    }
  }
  
  // 4. 状态变化立即上报 TENG
  if (ledState != lastSent) {
    if (publishTeng(ledState)) {
      lastSent = ledState;
    }
  }
  
  // 5. 周期性心跳上报
  if (now - lastKeepaliveMs >= KEEPALIVE_INTERVAL_MS) {
    Serial.println("心跳上报...");
    publishTeng(ledState);
    lastKeepaliveMs = now;
  }
  
  // 6. 系统状态显示（每分钟一次）
  static unsigned long lastStatusTime = 0;
  if (now - lastStatusTime > 60000) {
    lastStatusTime = now;
    
    Serial.println("\n=== 系统状态 ===");
    Serial.printf("WiFi: %s (RSSI: %d)\n", 
                  WiFi.status() == WL_CONNECTED ? "已连接" : "断开",
                  WiFi.RSSI());
    Serial.printf("MQTT: %s\n", 
                  mqtt.connected() ? "已连接" : "断开");
    Serial.printf("需要重连: %s\n", mqttNeedsReconnect ? "是" : "否");
    Serial.printf("灯状态: %s\n", ledState == 1 ? "亮" : "灭");
    Serial.printf("运行时间: %.1f 分钟\n", now / 60000.0);
    Serial.println("================\n");
  }
  
  // 7. 最小延迟
  delay(2);
}