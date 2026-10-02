// [출력] 서보모터 점검 - D3
// 시리얼 모니터에 0 ~ 180 숫자를 보내면 그 각도로 움직임
#include <Servo.h>
const int PIN = 3;
Servo servo;

void setup() {
  Serial.begin(9600);
  servo.attach(PIN);
  servo.write(90);
  Serial.println("서보 점검: 0 ~ 180 사이 숫자를 입력하고 엔터 (지금 90도)");
}

void loop() {
  if (Serial.available()) {
    int angle = Serial.parseInt();
    while (Serial.available()) Serial.read();
    if (angle >= 0 && angle <= 180) {
      servo.write(angle);
      Serial.print("각도: ");
      Serial.println(angle);
    }
  }
}
