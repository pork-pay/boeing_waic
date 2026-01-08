#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

// ================== 串口（接 A 板）==================
#define SENSOR_RX 4  // D2对应的GPIO4
SoftwareSerial SensorSerial;  // 使用软件串口

// ================== WiFi 配置 ==================
const char* ssid     = "zju2-wifi-7";
const char* password = "1234567890";

// ================== MQTT 配置 ==================
const char* mqtt_server = "192.168.100.121";
const int   mqtt_port   = 1883;
const char* mqtt_topic  = "v1/devices/me/telemetry";
const char* TB_TOKEN    = "rY3Ed88UJZuVI6MIa9Vi";

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ================== 最新值缓存 ==================
float latestTemp = NAN;
int   latestRssi = 127;

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

// ================== 修复的 MQTT 逻辑 ==================
void setupMqtt() {
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setKeepAlive(600);
}

void ensureMqtt() {
  if (mqtt.connected()) return;
  
  // ESP8266 使用不同的方式生成 ClientID
  String clientId = "ESP8266-" + String(ESP.getChipId(), HEX);
  
  Serial.print("Connecting to MQTT ... ");
  if (mqtt.connect(clientId.c_str(), TB_TOKEN, NULL)) {
    Serial.println("connected.");
  } else {
    Serial.print("failed, rc="); Serial.println(mqtt.state());
  }
}

// ================== 上报函数（保持不变）==================
bool publishTempRssi() {
  ensureMqtt();
  if (!mqtt.connected()) {
    Serial.println("MQTT not connected, skip publish");
    return false;
  }

  String payload = "{";
  bool first = true;

  if (!isnan(latestTemp)) {
    payload += "\"temperature\":" + String(latestTemp, 2);
    first = false;
  }
  if (latestRssi < 127) {
    if (!first) payload += ",";
    payload += "\"rssi\":" + String(latestRssi);
  }
  payload += "}";

  Serial.print("Publish: "); Serial.println(payload);
  bool ok = mqtt.publish(mqtt_topic, payload.c_str());
  if (!ok) {
    Serial.println("MQTT publish failed");
  }
  return ok;
}

// ================== 解析函数保持不变 ==================
void parseLine(const char* s) {
  const char* tptr = strstr(s, "T:");
  const char* rptr = strstr(s, "R:");

  bool updated = false;

  if (tptr) {
    latestTemp = atof(tptr + 2);
    updated = true;
  }
  if (rptr) {
    latestRssi = atoi(rptr + 2);
    updated = true;
  }

  Serial.print("[B] parsed: "); Serial.println(s);

  if (updated) {
    publishTempRssi();
  }
}

// ================== setup() ==================
void setup() {
  Serial.begin(115200);
  // 初始化软件串口：RX=D2, 不需要 TX
  SensorSerial.begin(115200, SWSERIAL_8N1, SENSOR_RX, -1, false, 256);

  setupWiFi();
  setupMqtt();
  ensureMqtt();

  Serial.println("ESP8266-B (UART RX@D2 -> MQTT temperature+rssi) Ready");
}

// ================== 主循环 ==================
void loop() {
  if (!mqtt.connected()) {
    ensureMqtt();
  } else {
    mqtt.loop();
  }

  // 串口数据处理
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
    } else {
      if (lineLen < (int)sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      } else {
        lineLen = 0;
      }
    }
  }

  delay(5);
}