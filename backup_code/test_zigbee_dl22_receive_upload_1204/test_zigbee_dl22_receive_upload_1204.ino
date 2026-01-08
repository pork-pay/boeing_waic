#include <WiFi.h>
#include <PubSubClient.h>

// ================== 引脚与串口 ==================
#define SENSOR_RX 17   // 传感器 TX -> ESP32 GPIO17 (UART1 RX)
HardwareSerial SensorSerial(1);   // UART1：只接收传感器

// ================== WiFi 配置 ==================
const char* ssid     = "zju-test-wifi-7";
const char* password = "1234567890";
// const char* ssid     = "Netcore-DB56EE";
// const char* password = "12345678";

// ================== MQTT / ThingsBoard 配置 ==================
const char* mqtt_server = "192.168.100.219";     // 你的 TB 服务器
const int   mqtt_port   = 1883;                  // 常见 1883/1884
const char* mqtt_topic  = "v1/devices/me/telemetry";
const char* TB_TOKEN    = "VG0qV4GynNvI3y7V7Ckx"; // 设备 Token（用户名即 Token）

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ================== 数据结构 ==================
struct SensorData {
  float    temperature;   // ℃
  float    humidity;      // %RH
  uint32_t pressure;      // Pa
  uint16_t pm25;          // µg/m³
};

// ================== 稳定的 WiFi 连接 ==================
void setupWiFi() {
  Serial.print("Connecting to WiFi "); Serial.print(ssid); Serial.println(" ...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());
}

// ================== MQTT 逻辑（和参考代码一致） ==================
void setupMqtt() {
  mqtt.setServer(mqtt_server, mqtt_port);  // 只在初始化时设置一次
  mqtt.setKeepAlive(60);                   // 可选：设置保活时间
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  mqtt.setServer(mqtt_server, mqtt_port);
  
  String clientId = "ESP32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to MQTT ... ");
  
  if (mqtt.connect(clientId.c_str(), TB_TOKEN, NULL)) {
    Serial.println("connected.");
  } else {
    Serial.print("failed, rc="); Serial.println(mqtt.state());
  }
}

// ================== 上报函数（和参考代码一致模式） ==================
bool publishTelemetry(float temperature, float humidity, uint32_t pressure, uint16_t pm25) {
  ensureMqtt();  // 直接调用 ensureMqtt()
  if (!mqtt.connected()) {
    Serial.println("MQTT not connected, skip publish");
    return false;
  }
  
  String payload = "{";
  bool first = true;

  if (!isnan(temperature)) {
    payload += "\"temperature\":" + String(temperature, 1);
    first = false;
  }
  if (!isnan(humidity)) {
    if (!first) payload += ",";
    payload += "\"humidity\":" + String(humidity, 1);
    first = false;
  }
  if (pm25 > 0) {
    if (!first) payload += ",";
    payload += "\"pm25\":" + String(pm25);
    first = false;
  }
  if (pressure > 0) {
    if (!first) payload += ",";
    payload += "\"air_pressure\":" + String(pressure);
  }
  payload += "}";

  Serial.print("Publish: "); Serial.println(payload);
  bool ok = mqtt.publish(mqtt_topic, payload.c_str());
  if (!ok) {
    Serial.println("MQTT publish failed");
  }
  return ok;
}
// ================== 解析函数（保持不变） ==================
bool parseSensorFrame(const uint8_t* data, SensorData* out) {
  if (!data || !out) return false;
  if (data[0] != 0x01 || data[1] != 0x03) return false;

  float     temp = (data[13] * 256 + data[14]) / 100.0f; // 0.01→°C
  float     humi = (data[11] * 256 + data[12]) / 100.0f; // 0.01→%RH
  uint16_t  pm   =  (uint16_t)(data[9]  * 256 + data[10]);
  uint32_t  p    =  ((uint32_t)data[23] << 24) |
                    ((uint32_t)data[24] << 16) |
                    ((uint32_t)data[25] << 8 ) |
                     (uint32_t)data[26];       // Pa（大端）

  out->temperature = roundf(temp * 10.0f) / 10.0f; // 保留1位小数
  out->humidity    = roundf(humi * 10.0f) / 10.0f;
  out->pm25        = pm;
  out->pressure    = p;
  return true;
}

// ================== Arduino 入口（和参考代码一致） ==================
void setup() {
  Serial.begin(115200);
  // 只接收：RX=GPIO17，TX=-1
  SensorSerial.begin(9600, SERIAL_8N1, SENSOR_RX, -1);

  setupWiFi();
  setupMqtt();  // 新增：单独初始化 MQTT
  ensureMqtt(); // 初始连接尝试

  Serial.println("ESP32 Sensor→Decode→MQTT Ready");
  Serial.println("数据格式: 温度/湿度/PM2.5/气压");
}

// ================== 主循环（和参考代码完全一致） ==================
void loop() {
  // 和参考代码完全一样的MQTT处理逻辑
  if (!mqtt.connected()) {
    ensureMqtt();
  } else {
    mqtt.loop();  // 只有连接成功时才调用 loop()
  }

  // 串口数据读取和处理
  static uint8_t buffer[29];
  static int pos = 0;
  static unsigned long lastUploadTime = 0;
  static int uploadCount = 0;

  while (SensorSerial.available()) {
    uint8_t b = (uint8_t)SensorSerial.read();

    // 简易帧同步：确保前两字节是 0x01 0x03
    if (pos == 0 && b != 0x01) continue;
    if (pos == 1 && b != 0x03) { 
      pos = 0; 
      continue; 
    }

    buffer[pos++] = b;

    if (pos >= 29) {
      SensorData d;
      if (parseSensorFrame(buffer, &d)) {
        Serial.printf("Parsed → T=%.1f°C, H=%.1f%%, PM2.5=%u µg/m³, P=%lu Pa\n",
                      d.temperature, d.humidity, d.pm25, d.pressure);
        
        // 立即发布数据
        publishTelemetry(d.temperature, d.humidity, d.pressure, d.pm25);
        
        // 调试信息：显示上传频率
        uploadCount++;
        unsigned long currentTime = millis();
        if (lastUploadTime > 0) {
          unsigned long interval = currentTime - lastUploadTime;
          Serial.printf("[DEBUG] 上传间隔: %lu ms, 总上传次数: %d\n", interval, uploadCount);
        }
        lastUploadTime = currentTime;
      } else {
        Serial.println("⚠️ Invalid frame header or layout.");
      }

      pos = 0; // 重置缓冲
    }
  }

  // 小的延迟，让其他任务有机会运行
  delay(5);
}