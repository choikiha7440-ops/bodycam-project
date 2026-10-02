// [입력] 버튼 모듈 점검 - D8
const int PIN = 8;
int last = -1;
int count = 0;

void setup() {
  Serial.begin(9600);
  pinMode(PIN, INPUT);
  Serial.println("버튼 점검: 초록 버튼을 눌렀다 떼 보세요");
}

void loop() {
  int v = digitalRead(PIN);
  if (v != last) {
    if (v == HIGH) count++;
    Serial.print("상태: ");
    Serial.print(v == HIGH ? "눌림 (1)" : "떼짐 (0)");
    Serial.print("   누른 횟수: ");
    Serial.println(count);
    last = v;
  }
  delay(20);
}
