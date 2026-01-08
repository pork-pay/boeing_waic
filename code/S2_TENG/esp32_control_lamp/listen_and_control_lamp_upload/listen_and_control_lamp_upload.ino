#include <WiFi.h>
#include <PubSubClient.h>

/* ================== LED 与串口 ================== */
const int ledPin = 4;  // 你指定的 GPIO4，LOW=灭，HIGH=亮（保持原逻辑）

/* ================== WiFi 配置 ================== */
// const char* ssid     = "zju-test-wifi-3";
// const char* password = "1234567890";
const char* ssid     = "Netcore-DB56EE";
const char* password = "12345678";
/* ================== MQTT / ThingsBoard 配置 ================== */
const char* mqtt_server = "192.168.100.219";     // TB 服务器
const int   mqtt_port   = 1883;                  // 1883
const char* mqtt_topic  = "v1/devices/me/telemetry";
const char* TB_TOKEN    = "VG0qV4GynNvI3y7V7Ckx"; // 设备 Token（用户名就是 Token）

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

/* ================== 状态变量 ================== */
uint8_t ledState = 0;        // 0=灭，1=亮（TENG 上传取此值）
uint8_t lastSent = 255;      // 上次已上报的状态，255 表示尚未上报
unsigned long lastKeepaliveMs = 0;   // 周期性心跳上报
const unsigned long KEEPALIVE_INTERVAL_MS = 30000;  // 30s 发一次当前状态心跳

/* ================== WiFi / MQTT 工具 ================== */
void setupWiFi() {
  WiFi.mode(WIFI_STA);
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

  // 使用芯片 MAC 生成客户端 ID
  String clientId = "ESP32-LAMP-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to MQTT ... ");
  // 用户名用 Token，密码留空
  if (mqtt.connect(clientId.c_str(), TB_TOKEN, NULL)) {
    Serial.println("connected.");
  } else {
    Serial.print("failed, rc="); Serial.println(mqtt.state());
  }
}

bool publishTeng(uint8_t v) {
  ensureMqtt();
  if (!mqtt.connected()) return false;

  // TB 要求 JSON 的 key 加引号
  String payload = String("{\"TENG\":") + String((int)v) + "}";
  bool ok = mqtt.publish(mqtt_topic, payload.c_str());
  Serial.print("Publish TENG: "); Serial.println(payload);
  if (!ok) Serial.println("MQTT publish failed");
  return ok;
}

/* ================== Arduino 标准入口 ================== */
void setup() {
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);     // 初始灯灭
  ledState = 0;

  Serial.begin(115200);
  Serial.println("ESP32 已启动，灯初始为灭，等待串口数据... (串口发 '1' 亮, '0' 灭)");

  setupWiFi();
  ensureMqtt();

  // 上电后先上报一次初始状态
  publishTeng(ledState);
  lastSent = ledState;
  lastKeepaliveMs = millis();
}

void loop() {
  // 维持 MQTT 连接与收发
  if (!mqtt.connected()) ensureMqtt();
  mqtt.loop();

  // 读取串口：'1'->亮，'0'->灭（保持你的原逻辑不变）
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
    }
  }

  // 状态变化立即上报 TENG（亮=1，不亮=0）
  if (ledState != lastSent) {
    if (publishTeng(ledState)) lastSent = ledState;
  }

  // 周期性心跳上报，防止 Dashboard 长时间无数据
  unsigned long now = millis();
  if (now - lastKeepaliveMs >= KEEPALIVE_INTERVAL_MS) {
    publishTeng(ledState);
    lastKeepaliveMs = now;
  }

  delay(5);
}
