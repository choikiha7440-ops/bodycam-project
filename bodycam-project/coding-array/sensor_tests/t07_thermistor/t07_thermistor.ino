// [입력] 써미스터 모듈 점검 - A4
// 확장 쉴드 밑 "A <-> I2C" 스위치를 A 로, 아날로그 DIP 5번 ON
const int PIN = A4;

void setup() {
  Serial.begin(9600);
  Serial.println("써미스터 점검: 검은 부품을 손가락으로 감싸 데워 보세요");
}

void loop() {
  int v = analogRead(PIN);
  float r = 10000.0 * v / (1023.0 - v);
  float k = 1.0 / (1.0 / 298.15 + log(r / 10000.0) / 3950.0);
  float c = k - 273.15;
  Serial.print("값: ");
  Serial.print(v);
  Serial.print("   대략 온도: ");
  Serial.print(c, 1);
  Serial.println(" C");
  delay(500);
}
