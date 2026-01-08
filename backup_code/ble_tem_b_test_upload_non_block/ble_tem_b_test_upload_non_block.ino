#include <WiFi.h>
#include <PubSubClient.h>

// ================== 串口（接 A 板） ==================
#define SENSOR_RX 5
HardwareSerial SensorSerial(2);

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

// ================== 最新值缓存 ==================
float latestTemp = NAN;
int   latestRssi = 127;

// ================== 状态变量 ==================
unsigned long lastWifiCheck = 0;
unsigned long lastMqttPing = 0;
unsigned long lastDataTime = 0;
bool wifiWasConnected = false;
const unsigned long WIFI_CHECK_INTERVAL = 2000;    // 2秒检查一次WiFi
const unsigned long MQTT_PING_INTERVAL = 30000;    // 30秒检查一次MQTT
const unsigned long DATA_TIMEOUT = 5000;          // 5秒无数据重置解析

// ================== 稳定的 WiFi 连接 ==================
void setupWiFi() {
  Serial.print("Connecting to WiFi "); Serial.print(ssid); Serial.println(" ...");
  WiFi.begin(ssid, password);
  
  // 启用WiFi自动重连
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startTime < 15000)) {
    delay(400);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP Address: "); Serial.println(WiFi.localIP());
    wifiWasConnected = true;
  } else {
    Serial.println("\nWiFi connection timeout");
  }
}

// ================== 改进的 MQTT 逻辑 ==================
void setupMqtt() {
  mqtt.setServer(mqtt_server, mqtt_port);  // 只在初始化时设置一次
  mqtt.setKeepAlive(60);                    // 缩短为60秒，响应更快
  mqtt.setSocketTimeout(5);                 // 设置5秒socket超时
}

bool connectMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot connect MQTT");
    return false;
  }
  
  String clientId = "ESP32-" + String(random(0xffff), HEX);
  Serial.print("Connecting to MQTT ... ");
  
  // 设置连接超时
  unsigned long startTime = millis();
  bool connected = mqtt.connect(clientId.c_str(), TB_TOKEN, NULL);
  
  if (connected) {
    Serial.println("connected.");
    lastMqttPing = millis();
    return true;
  } else {
    Serial.print("failed, rc="); Serial.println(mqtt.state());
    return false;
  }
}

// ================== 改进的上报函数（非阻塞） ==================
bool publishTempRssi() {
  // 检查WiFi连接
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  
  // 检查MQTT连接，不自动重连（避免阻塞）
  if (!mqtt.connected()) {
    return false;
  }

  // 构建payload
  String payload = "{";
  bool hasData = false;

  if (!isnan(latestTemp)) {
    payload += "\"temperature\":" + String(latestTemp, 2);
    hasData = true;
  }
  if (latestRssi < 127) {
    if (hasData) payload += ",";
    payload += "\"rssi\":" + String(latestRssi);
    hasData = true;
  }
  
  if (!hasData) {
    return false;  // 没有有效数据
  }
  
  payload += "}";

  Serial.print("Publish: "); Serial.println(payload);
  
  // 非阻塞发布，不等待ACK
  bool ok = mqtt.publish(mqtt_topic, payload.c_str());
  
  if (!ok) {
    Serial.println("MQTT publish failed (non-blocking)");
    // 不立即断开，等待下次检查
  }
  
  return ok;
}

// ================== 改进的解析函数 ==================
void parseLine(const char* s) {
  // 检查数据有效性
  if (strlen(s) == 0 || strlen(s) > 50) {
    Serial.println("Invalid line length");
    return;
  }
  
  // 检查是否包含乱码（只接受可打印ASCII）
  for (int i = 0; s[i] != '\0'; i++) {
    if (s[i] < 32 || s[i] > 126) {
      Serial.println("Invalid character in line");
      return;
    }
  }
  
  const char* tptr = strstr(s, "T:");
  const char* rptr = strstr(s, "R:");

  bool updated = false;

  if (tptr) {
    float temp = atof(tptr + 2);
    if (temp > -50 && temp < 150) {  // 合理的温度范围
      latestTemp = temp;
      updated = true;
    }
  }
  
  if (rptr) {
    int rssi = atoi(rptr + 2);
    if (rssi > -200 && rssi < 200) {  // 合理的RSSI范围
      latestRssi = rssi;
      updated = true;
    }
  }

  if (updated) {
    Serial.print("[B] parsed: ");
    if (!isnan(latestTemp)) {
      Serial.print("T="); Serial.print(latestTemp, 2);
    }
    if (latestRssi < 127) {
      Serial.print(" R="); Serial.print(latestRssi);
    }
    Serial.println();
    
    // 立即尝试发布，但不阻塞
    publishTempRssi();
  } else {
    Serial.print("[B] invalid: "); Serial.println(s);
  }
}

// ================== 连接状态管理 ==================
void checkConnections() {
  unsigned long now = millis();
  
  // 检查WiFi状态
  if (now - lastWifiCheck > WIFI_CHECK_INTERVAL) {
    lastWifiCheck = now;
    
    bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    
    if (wifiConnected != wifiWasConnected) {
      if (wifiConnected) {
        Serial.println("WiFi reconnected");
        // WiFi恢复，尝试连接MQTT
        if (!mqtt.connected()) {
          connectMqtt();
        }
      } else {
        Serial.println("WiFi disconnected");
        // WiFi断开，标记MQTT为未连接
        if (mqtt.connected()) {
          mqtt.disconnect();
          Serial.println("MQTT force disconnected");
        }
      }
      wifiWasConnected = wifiConnected;
    }
  }
  
  // 定期检查MQTT心跳
  if (mqtt.connected() && (now - lastMqttPing > MQTT_PING_INTERVAL)) {
    lastMqttPing = now;
    // 发送一个ping保持连接
    mqtt.loop();
  }
}

// ================== 修复的 setup() ==================
void setup() {
  Serial.begin(115200);
  delay(1000);  // 等待串口稳定
  
  Serial.println("\n\nESP32-B Starting...");
  
  SensorSerial.begin(115200, SERIAL_8N1, SENSOR_RX, -1);
  
  // 初始化随机种子
  randomSeed(analogRead(0));
  
  setupWiFi();
  setupMqtt();
  
  // 不立即连接MQTT，等待有数据时再连接
  Serial.println("ESP32-B Ready (MQTT will connect when data arrives)");
  Serial.println("==================================================");
}

// ================== 改进的主循环（非阻塞） ==================
void loop() {
  unsigned long loopStart = millis();
  
  // 1. 检查连接状态（非阻塞）
  checkConnections();
  
  // 2. 处理MQTT消息（非阻塞）
  if (mqtt.connected()) {
    mqtt.loop();
  } else if (WiFi.status() == WL_CONNECTED) {
    // WiFi已连接但MQTT未连接，定期尝试连接
    static unsigned long lastMqttRetry = 0;
    if (millis() - lastMqttRetry > 10000) {  // 每10秒尝试一次
      lastMqttRetry = millis();
      connectMqtt();
    }
  }
  
  // 3. 非阻塞串口读取（限制每次读取量）
  static char lineBuf[64];
  static int lineLen = 0;
  static unsigned long lastCharTime = 0;
  
  // 限制每次最多读取20个字符，避免阻塞
  int maxRead = 20;
  while (SensorSerial.available() && maxRead-- > 0) {
    char c = (char)SensorSerial.read();
    lastCharTime = millis();
    
    // 只处理换行符作为结束
    if (c == '\n') {
      if (lineLen > 0) {
        lineBuf[lineLen] = '\0';
        parseLine(lineBuf);
        lineLen = 0;
      }
    } 
    // 忽略回车符
    else if (c != '\r') {
      if (lineLen < (int)sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      } else {
        // 缓冲区溢出，可能是错误数据
        Serial.println("Line buffer overflow, resetting");
        lineLen = 0;
      }
    }
  }
  
  // 4. 检查数据超时（防止半包数据）
  if (lineLen > 0 && (millis() - lastCharTime > DATA_TIMEOUT)) {
    Serial.println("Data timeout, resetting buffer");
    lineLen = 0;
  }
  
  // 5. 控制循环频率
  unsigned long loopTime = millis() - loopStart;
  if (loopTime < 10) {
    delay(10 - loopTime);  // 保持约100Hz频率
  } else if (loopTime > 100) {
    Serial.print("Long loop time: "); Serial.print(loopTime); Serial.println("ms");
  }
}

// ================== WiFi事件回调（可选，增强稳定性） ==================
/*
// 在setup()中注册：
WiFi.onEvent(WiFiEvent);

void WiFiEvent(WiFiEvent_t event) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("WiFi STA Started");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("WiFi Connected");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("WiFi Got IP: ");
      Serial.println(WiFi.localIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi Disconnected");
      break;
  }
}
*/
