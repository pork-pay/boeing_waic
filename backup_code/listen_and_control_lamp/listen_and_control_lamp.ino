int ledPin = 4; // 你指定的 GPIO4


void setup() {
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);   // 初始状态设为 0 -> 灯灭

  Serial.begin(115200);
  Serial.println("ESP32 已启动，灯初始为灭，等待串口数据...");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '1') {
      digitalWrite(ledPin, HIGH);  // 灯亮
      Serial.println("收到 1 -> 灯亮");
    } else if (c == '0') {
      digitalWrite(ledPin, LOW);   // 灯灭
      Serial.println("收到 0 -> 灯灭");
    }
  }
}
