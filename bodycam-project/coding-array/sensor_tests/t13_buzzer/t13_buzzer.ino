// [출력] 수동 부저 점검 - D6 (도레미파솔라시도)
const int PIN = 6;
const int notes[] = {262, 294, 330, 349, 392, 440, 494, 523};
const char* names[] = {"도", "레", "미", "파", "솔", "라", "시", "도"};

void setup() {
  Serial.begin(9600);
  Serial.println("부저 점검: 도레미파솔라시도를 연주합니다");
}

void loop() {
  for (int i = 0; i < 8; i++) {
    Serial.print(names[i]);
    Serial.print(" (");
    Serial.print(notes[i]);
    Serial.println(" Hz)");
    tone(PIN, notes[i], 300);
    delay(400);
  }
  Serial.println("--- 3초 쉬고 다시 ---");
  delay(3000);
}
