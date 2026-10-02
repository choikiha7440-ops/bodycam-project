// [입력] 마이크 모듈 점검 - A3
const int PIN = A3;

void setup() {
  Serial.begin(9600);
  Serial.println("마이크 점검: 조용히 있다가 박수를 쳐 보세요");
}

void loop() {
  int mn = 1023, mx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 50) {
    int v = analogRead(PIN);
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  int level = mx - mn;
  Serial.print("소리 크기: ");
  Serial.print(level);
  Serial.print("   ");
  for (int i = 0; i < level / 10; i++) Serial.print("|");
  Serial.println();
}
