// [입력] 불꽃 센서 점검 - A5
// 확장 쉴드 밑 "A <-> I2C" 스위치를 A 로, 아날로그 DIP 6번 ON
const int PIN = A5;
int base;

void setup() {
  Serial.begin(9600);
  delay(200);
  base = analogRead(PIN);
  Serial.print("불꽃 센서 점검: 평소 값 = ");
  Serial.println(base);
  Serial.println("TV 리모컨 버튼을 센서에 대고 눌러 보세요 (적외선에 반응)");
}

void loop() {
  int v = analogRead(PIN);
  Serial.print("값: ");
  Serial.print(v);
  Serial.print("   평소와 차이: ");
  Serial.println(v - base);
  delay(100);
}
