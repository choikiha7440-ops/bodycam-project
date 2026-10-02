// [입력] 홀 센서 점검 - A2
const int PIN = A2;
int base;

void setup() {
  Serial.begin(9600);
  delay(200);
  base = analogRead(PIN);
  Serial.print("홀 센서 점검: 평소 값 = ");
  Serial.println(base);
  Serial.println("자석의 N극, S극을 번갈아 가까이 대 보세요");
}

void loop() {
  int v = analogRead(PIN);
  int diff = v - base;
  Serial.print("값: ");
  Serial.print(v);
  Serial.print("   평소와 차이: ");
  Serial.print(diff);
  if (abs(diff) > 60) Serial.print("   <- 자석 감지!");
  Serial.println();
  delay(200);
}
