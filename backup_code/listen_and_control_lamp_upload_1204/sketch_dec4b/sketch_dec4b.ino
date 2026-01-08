#include <WiFi.h>
#include <PubSubClient.h>

/* ================== LED 与串口 ================== */
const int ledPin = 4;  // 你指定的 GPIO4，LOW=灭，HIGH=亮

/* ================== WiFi 配置 ================== */
const char* ssid     = "Netcore-DB56EE";
const char* password = "12345678";
// const char* ssid     = "Redmi_103";
// const char* password = "103@ISEE";
/* ================== MQTT / ThingsBoard 配置 ================== */
const char* mqtt_server = "192.168.100.121";     // TB 服务器
const int   mqtt_port   = 1883;                  // 1883
const char* mqtt_topic  = "v1/devices/me/telemetry";
const char* TB_TOKEN    = "J3bX1e0VliJCo1vUhmzv"; // 设备 Token

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

/* ================== 状态变量 ================== */
uint8_t ledState = 0;        // 0=灭，1=亮（物理状态）
uint8_t lastSent = 255;      // 上次已上报的状态

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

// ================== 修复的 MQTT 逻辑（和第二个代码一致） ==================
void setupMqtt() {
  mqtt.setServer(mqtt_server, mqtt_port);  // 只在初始化时设置一次
  mqtt.setKeepAlive(60);                   // 可选：设置保活时间
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  mqtt.setServer(mqtt_server, mqtt_port);
  
  String clientId = "ESP32-LAMP-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to MQTT ... ");
  
  if (mqtt.connect(clientId.c_str(), TB_TOKEN, NULL)) {
    Serial.println("connected.");
  } else {
    Serial.print("failed, rc="); Serial.println(mqtt.state());
  }
}

// ================== 上报函数（和第二个代码一致模式） ==================
bool publishTeng() {
  ensureMqtt();
  if (!mqtt.connected()) {
    Serial.println("MQTT not connected, skip publish");
    return false;
  }

  // 关键修改：物理状态与teng值映射
  // 物理亮(1) -> teng=0 (Dashboard显示红点)
  // 物理灭(0) -> teng=1 (Dashboard显示灰点)
  uint8_t tengValue = (ledState == 0) ? 1 : 0;
  
  String payload = String("{\"TENG\":") + String((int)tengValue) + "}";
  
  Serial.print("Publish: "); Serial.println(payload);
  Serial.print("物理灯状态: "); Serial.print(ledState == 1 ? "亮" : "灭");
  Serial.print(" -> 发送TENG: "); Serial.println(tengValue);
  
  bool ok = mqtt.publish(mqtt_topic, payload.c_str());
  if (!ok) {
    Serial.println("MQTT publish failed");
    // 发布失败时断开连接，下次会重连（和第二个代码一样注释掉）
    // mqtt.disconnect();
  }
  return ok;
}

/* ================== Arduino 标准入口（修改setup） ================== */
void setup() {
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);     // 初始灯灭
  ledState = 0;

  Serial.begin(115200);
  Serial.println("ESP32 已启动，灯初始为灭");
  Serial.println("串口发 '1' 亮, '0' 灭");
  Serial.println("映射关系: 物理亮(1) -> teng=0, 物理灭(0) -> teng=1");

  setupWiFi();
  setupMqtt();  // 新增：单独初始化 MQTT（和第二个代码一样）
  ensureMqtt(); // 初始连接尝试

  // 上电后先上报一次初始状态
  publishTeng();
  lastSent = ledState;
}

/* ================== 主循环（改成和第二个代码一样） ================== */
void loop() {
  // 和第二个代码完全一样的MQTT处理逻辑
  if (!mqtt.connected()) {
    ensureMqtt();
  } else {
    mqtt.loop();  // 只有连接成功时才调用 loop()
  }

  // 读取串口：'1'->亮，'0'->灭
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

  // 状态变化立即上报 TENG
  if (ledState != lastSent) {
    if (publishTeng()) {
      lastSent = ledState;
    }
  }

  delay(5);
}