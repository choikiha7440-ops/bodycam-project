// [입력] 슬라이드 가변저항 점검 - A0
const int PIN = A0;

void setup() {
  Serial.begin(9600);
  Serial.println("슬라이드 점검: 노란 손잡이를 끝에서 끝까지 밀어 보세요");
}

void loop() {
  int v = analogRead(PIN);
  float volt = v * 5.0 / 1023.0;
  Serial.print("값: ");
  Serial.print(v);
  Serial.print("   전압: ");
  Serial.print(volt, 2);
  Serial.print("V   ");
  for (int i = 0; i < v / 50; i++) Serial.print("#");
  Serial.println();
  delay(200);
}
