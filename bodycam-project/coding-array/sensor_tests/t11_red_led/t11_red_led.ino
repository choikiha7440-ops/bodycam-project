// [출력] Red LED 점검 - D13
// 시리얼 모니터에 1 을 보내면 켜짐, 0 을 보내면 꺼짐, b 를 보내면 깜빡임
const int PIN = 13;
char mode = 'b';

void setup() {
  Serial.begin(9600);
  pinMode(PIN, OUTPUT);
  Serial.println("Red LED 점검: 1 = 켜기, 0 = 끄기, b = 깜빡이기");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '1' || c == '0' || c == 'b') {
      mode = c;
      Serial.print("모드: ");
      Serial.println(mode);
    }
  }
  if (mode == '1') digitalWrite(PIN, HIGH);
  else if (mode == '0') digitalWrite(PIN, LOW);
  else digitalWrite(PIN, (millis() / 500) % 2);
}
