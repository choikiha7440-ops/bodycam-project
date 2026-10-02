// [출력/통신] I2C 장치 찾기 - SDA A4, SCL A5
// 확장 쉴드 밑 "A <-> I2C" 스위치를 I2C 로
#include <Wire.h>

void setup() {
  Serial.begin(9600);
  Wire.begin();
  Serial.println("I2C 장치 찾는 중...");
  int found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    byte err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("찾음: 0x");
      Serial.println(addr, HEX);
      found++;
    }
  }
  if (found == 0) Serial.println("아무것도 없음 (스위치, 연결 확인)");
  Serial.println("끝. 다시 하려면 RESET 버튼");
}

void loop() {}
