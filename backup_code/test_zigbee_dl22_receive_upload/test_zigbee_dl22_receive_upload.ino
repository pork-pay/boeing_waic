#include <WiFi.h>
#include <PubSubClient.h>

// ================== 引脚与串口 ==================
#define SENSOR_RX 17   // 传感器 TX -> ESP32 GPIO17 (UART1 RX)
HardwareSerial SensorSerial(1);   // UART1：只接收传感器

// ================== WiFi 配置 ==================
// const char* ssid     = "zju-test-wifi-6";
// const char* password = "1234567890";
// const char* ssid     = "Netcore-DB56EE";
// const char* password = "12345678";
const char* ssid     = "Redmi_103";
const char* password = "103@ISEE";
/* ================== MQTT / ThingsBoard 配置 ================== */
const char* mqtt_server = "192.168.31.104";     // TB 服务器
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

// ================== WiFi / MQTT 工具 ==================
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

bool publishTelemetry(const SensorData& d) {
  ensureMqtt();
  if (!mqtt.connected()) return false;

  String payload = "{";
  payload += "\"temperature\":" + String(d.temperature, 2) + ",";
  payload += "\"humidity\":"    + String(d.humidity, 2)    + ",";
  payload += "\"air_pressure\":"    + String((uint32_t)d.pressure) + ",";
  payload += "\"pm25\":"       + String((uint16_t)d.pm25);
  payload += "}";

  Serial.print("Publish: "); Serial.println(payload);
  bool ok = mqtt.publish(mqtt_topic, payload.c_str());
  if (!ok) Serial.println("MQTT publish failed");
  return ok;
}

// ================== 解析函数（按你给的下标） ==================
// 输入：data[0..28] 共 29 字节，要求 data[0]==0x01 && data[1]==0x03
// 注：未做 CRC/校验（与示例一致）
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

// ================== Arduino 入口 ==================
void setup() {
  Serial.begin(115200);
  // 只接收：RX=GPIO17，TX=-1
  SensorSerial.begin(9600, SERIAL_8N1, SENSOR_RX, -1);

  setupWiFi();
  ensureMqtt();

  Serial.println("ESP32 Sensor→Decode→MQTT Ready (no Zigbee forward)");
}

// ================== 主循环 ==================
void loop() {
  if (!mqtt.connected()) ensureMqtt();
  mqtt.loop();

  static uint8_t buffer[29];
  static int pos = 0;

  while (SensorSerial.available()) {
    uint8_t b = (uint8_t)SensorSerial.read();

    // 简易帧同步：确保前两字节是 0x01 0x03
    if (pos == 0 && b != 0x01) continue;
    if (pos == 1 && b != 0x03) { pos = 0; continue; }

    buffer[pos++] = b;

    if (pos >= 29) {
      // 显示原始帧（可按需注释）
      Serial.println("\n📥 Raw Sensor Data (29B):");
      for (int i = 0; i < 29; i++) {
        Serial.printf("%02X ", buffer[i]);
        if ((i + 1) % 16 == 0) Serial.println();
      }
      Serial.println();

      SensorData d;
      if (parseSensorFrame(buffer, &d)) {
        Serial.printf("Parsed → T=%.1f°C, H=%.1f%%, PM2.5=%u µg/m³, P=%lu Pa\n",
                      d.temperature, d.humidity, d.pm25, d.pressure);
        publishTelemetry(d);
      } else {
        Serial.println("⚠️ Invalid frame header or layout.");
      }

      pos = 0; // 重置缓冲
    }
  }

  delay(5);
}
