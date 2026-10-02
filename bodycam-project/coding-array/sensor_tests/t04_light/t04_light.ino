// [입력] 조도센서 점검 - A1
const int PIN = A1;
int minV = 1023, maxV = 0;

void setup() {
  Serial.begin(9600);
  Serial.println("조도센서 점검: 손으로 가렸다 떼고, 휴대폰 손전등도 비춰 보세요");
}

void loop() {
  int v = analogRead(PIN);
  if (v < minV) minV = v;
  if (v > maxV) maxV = v;
  Serial.print("밝기 값: ");
  Serial.print(v);
  Serial.print("   (지금까지 최소 ");
  Serial.print(minV);
  Serial.print(", 최대 ");
  Serial.print(maxV);
  Serial.println(")");
  delay(200);
}
