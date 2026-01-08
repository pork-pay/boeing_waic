#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// WiFi配置
const char* ssid     = "Netcore-DB56EE";
const char* password = "12345678";

// ThingsBoard配置
const char* tb_server = "192.168.100.219";  // 例如: "demo.thingsboard.io"
const int tb_port = 1883;
const char* tb_token = "8q7exc76jixr6vy2tdk9"; // 设备token

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastMsg = 0;
int requestId = 1;
bool attributesReceived = false;

void setup() {
  Serial.begin(115200);
  setup_wifi();
  client.setServer(tb_server, tb_port);
  client.setCallback(callback);
  
  pinMode(LED_BUILTIN, OUTPUT);
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("连接WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi连接成功");
  Serial.println("IP地址: ");
  Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("收到消息 [");
  Serial.print(topic);
  Serial.print("]: ");
  
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  // 处理属性响应
  if (String(topic).startsWith("v1/devices/me/attributes/response/")) {
    handleAttributesResponse(message);
  }
  
  // 处理属性更新
  if (String(topic) == "v1/devices/me/attributes") {
    handleAttributesUpdate(message);
  }
}

void handleAttributesResponse(const String& message) {
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, message);

  // 检查共享属性
  if (doc.containsKey("shared")) {
    JsonObject shared = doc["shared"];
    if (shared.containsKey("operationMode")) {
      int operationMode = shared["operationMode"];
      Serial.print("✅ 收到共享属性 operationMode: ");
      Serial.println(operationMode);
      attributesReceived = true;
      
      // 根据operationMode执行操作
      handleOperationMode(operationMode);
    }
  }
}

void handleAttributesUpdate(const String& message) {
  DynamicJsonDocument doc(1024);
  deserializeJson(doc, message);

  // 处理属性更新
  if (doc.containsKey("operationMode")) {
    int operationMode = doc["operationMode"];
    Serial.print("🔄 属性更新 operationMode: ");
    Serial.println(operationMode);
    
    handleOperationMode(operationMode);
  }
}

void handleOperationMode(int mode) {
  Serial.print("执行操作模式: ");
  Serial.println(mode);
  
  switch(mode) {
    case 1:
      Serial.println("模式1: 低速运行");
      digitalWrite(LED_BUILTIN, LOW);
      break;
    case 2:
      Serial.println("模式2: 中速运行");
      // 添加闪烁效果
      for(int i=0; i<3; i++) {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(200);
        digitalWrite(LED_BUILTIN, LOW);
        delay(200);
      }
      break;
    case 3:
      Serial.println("模式3: 高速运行");
      digitalWrite(LED_BUILTIN, HIGH);
      break;
    default:
      Serial.println("未知模式");
      digitalWrite(LED_BUILTIN, LOW);
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("尝试连接ThingsBoard...");
    
    if (client.connect("ESP32_Client", tb_token, NULL)) {
      Serial.println("✅ 连接成功!");
      
      // 订阅必要的主题
      client.subscribe("v1/devices/me/attributes");
      client.subscribe("v1/devices/me/attributes/response/+");
      Serial.println("✅ 主题订阅成功");
      
      // 连接成功后立即请求属性
      requestAttributes();
      
    } else {
      Serial.print("连接失败, rc=");
      Serial.print(client.state());
      Serial.println(" 5秒后重试...");
      delay(5000);
    }
  }
}

void requestAttributes() {
  // 请求共享属性
  DynamicJsonDocument doc(256);
  doc["sharedKeys"] = "operationMode";
  
  String output;
  serializeJson(doc, output);
  
  String topic = "v1/devices/me/attributes/request/" + String(requestId++);
  client.publish(topic.c_str(), output.c_str());
  
  Serial.println("📤 请求共享属性: " + output);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long now = millis();
  
  // 每30秒请求一次属性（用于测试）
  if (now - lastMsg > 30000) {
    lastMsg = now;
    
    if (!attributesReceived) {
      Serial.println("🔄 重新请求属性...");
      requestAttributes();
    }
    
    // 发送遥测数据（可选）
    sendTelemetry();
  }
}

void sendTelemetry() {
  DynamicJsonDocument doc(256);
  doc["temperature"] = random(200, 300) / 10.0;
  doc["humidity"] = random(400, 800) / 10.0;
  doc["operationStatus"] = attributesReceived ? "active" : "waiting_attributes";
  
  String output;
  serializeJson(doc, output);
  
  client.publish("v1/devices/me/telemetry", output.c_str());
  Serial.println("📤 发送遥测数据: " + output);
}