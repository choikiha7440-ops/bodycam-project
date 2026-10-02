// [입력] 초음파 센서 점검 - Trig D5, Echo D4
const int TRIG = 5;
const int ECHO = 4;

void setup() {
  Serial.begin(9600);
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  Serial.println("초음파 점검: 센서 앞 10cm, 30cm에 책을 대 보세요");
}

void loop() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  unsigned long us = pulseIn(ECHO, HIGH, 30000UL);
  Serial.print("돌아온 시간: ");
  Serial.print(us);
  Serial.print(" us   거리: ");
  if (us == 0) Serial.println("측정 실패");
  else {
    Serial.print(us / 58);
    Serial.println(" cm");
  }
  delay(300);
}
