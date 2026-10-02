// [입력] 온습도 센서 DHT-11 점검 - D12 (라이브러리 없이 직접 읽기)
const int PIN = 12;

bool readDHT11(int &t, int &h) {
  uint8_t d[5] = {0, 0, 0, 0, 0};
  pinMode(PIN, OUTPUT);
  digitalWrite(PIN, LOW);
  delay(20);
  pinMode(PIN, INPUT_PULLUP);
  if (pulseIn(PIN, HIGH, 300) == 0) return false;
  for (int i = 0; i < 40; i++) {
    unsigned long w = pulseIn(PIN, HIGH, 200);
    if (w == 0) return false;
    d[i / 8] <<= 1;
    if (w > 45) d[i / 8] |= 1;
  }
  if ((uint8_t)(d[0] + d[1] + d[2] + d[3]) != d[4]) return false;
  h = d[0];
  t = d[2];
  return true;
}

void setup() {
  Serial.begin(9600);
  Serial.println("DHT-11 점검: 2초마다 온도와 습도를 읽습니다");
}

void loop() {
  int t, h;
  if (readDHT11(t, h)) {
    Serial.print("온도: ");
    Serial.print(t);
    Serial.print(" C   습도: ");
    Serial.print(h);
    Serial.println(" %");
  } else {
    Serial.println("읽기 실패 (DIP 12번, 연결 확인)");
  }
  delay(2000);
}
