// [출력] RGB LED 점검 - 빨강 D9, 초록 D10, 파랑 D11
const int R = 9, G = 10, B = 11;
const char* names[] = {"빨강", "초록", "파랑", "노랑", "하늘색", "보라", "흰색", "꺼짐"};
const bool colors[][3] = {{1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {0,1,1}, {1,0,1}, {1,1,1}, {0,0,0}};

void setup() {
  Serial.begin(9600);
  pinMode(R, OUTPUT);
  pinMode(G, OUTPUT);
  pinMode(B, OUTPUT);
  Serial.println("RGB LED 점검: 1초마다 색이 바뀝니다. 이름과 실제 색이 같은지 보세요");
}

void loop() {
  for (int i = 0; i < 8; i++) {
    digitalWrite(R, colors[i][0]);
    digitalWrite(G, colors[i][1]);
    digitalWrite(B, colors[i][2]);
    Serial.println(names[i]);
    delay(1000);
  }
}
