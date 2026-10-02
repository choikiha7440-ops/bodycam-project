// [입력] 터치 모듈 점검 - D7
const int PIN = 7;
int last = -1;

void setup() {
  Serial.begin(9600);
  pinMode(PIN, INPUT);
  Serial.println("터치 점검: 하얀 네모에 손가락을 대 보세요");
}

void loop() {
  int v = digitalRead(PIN);
  if (v != last) {
    Serial.println(v == HIGH ? "터치됨 (1)" : "손 뗌 (0)");
    last = v;
  }
  delay(20);
}
