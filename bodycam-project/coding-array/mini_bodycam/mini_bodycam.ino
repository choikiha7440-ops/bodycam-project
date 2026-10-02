/*
  미니 바디캠 (코딩어레이 키트 연습용)
  실제 바디캠 펌웨어와 같은 구조로 만들었습니다.

  [키트 모듈]           [바디캠에서 맡는 역할]
  버튼 D8              녹화 스위치 (누를 때마다 켜기/끄기)
  RGB LED D9 D10 D11   상태 LED
  부저 D6              진동모터 (짧게 1번 = 시작, 짧게 2번 = 종료)
  슬라이드 A0           배터리 전압 (밀면 배터리 잔량이 바뀜)
  터치 D7              USB 충전 연결 (손을 대고 있으면 충전 중)
  Red LED D13          SD카드 저장 중 표시

  [LED 색]
  초록 천천히 깜빡임 = 평상시 자체녹화
  빨강 켜짐          = 녹화 중
  파랑 깜빡임        = 충전 중
  빨강 빠른 깜빡임    = 배터리 부족
*/

// ---- 핀 번호 ----
const int PIN_BUTTON = 8;
const int PIN_TOUCH  = 7;
const int PIN_BUZZER = 6;
const int PIN_RED    = 9;    // 색이 다르게 나오면 이 세 줄의 숫자만 바꾸세요
const int PIN_GREEN  = 10;
const int PIN_BLUE   = 11;
const int PIN_SD_LED = 13;
const int PIN_BAT    = A0;

const int LOW_BATTERY = 15;  // % 이하이면 배터리 부족

// ---- 상태 ----
bool recording = false;
bool charging = false;
int batteryPct = 100;
unsigned long recStart = 0;

// ---------------------------------------------------------------- LED
void led(bool r, bool g, bool b) {
  digitalWrite(PIN_RED, r);
  digitalWrite(PIN_GREEN, g);
  digitalWrite(PIN_BLUE, b);
}

void updateLed() {
  unsigned long t = millis();
  if (recording) {
    led(1, 0, 0);                                   // 빨강 켜짐
  } else if (charging) {
    led(0, 0, (t / 500) % 2);                       // 파랑 깜빡임
  } else if (batteryPct <= LOW_BATTERY) {
    led((t / 150) % 2, 0, 0);                       // 빨강 빠른 깜빡임
  } else {
    led(0, (t % 3000) < 300, 0);                    // 초록 3초마다 잠깐
  }
  digitalWrite(PIN_SD_LED, recording && (t / 250) % 2);  // 녹화 중 SD 기록 표시
}

// ---------------------------------------------------------------- 부저 (진동모터 대신)
int beepPattern = 0;             // 1 = 짧게 1번, 2 = 짧게 2번
unsigned long beepStart = 0;

void beep(int pattern) { beepPattern = pattern; beepStart = millis(); }

void updateBeep() {
  unsigned long t = millis() - beepStart;
  bool on = false;
  if (beepPattern == 1) on = t < 150;
  if (beepPattern == 2) on = t < 120 || (t > 250 && t < 370);
  if (on) tone(PIN_BUZZER, 2000); else noTone(PIN_BUZZER);
  if (beepPattern && t > 500) beepPattern = 0;
}

// ---------------------------------------------------------------- 버튼
void startRecord() {
  recording = true;
  recStart = millis();
  beep(1);
  Serial.println(">>> 녹화 시작 (10초 전 장면부터 서버로 전송)");
}

void stopRecord() {
  recording = false;
  beep(2);
  Serial.print(">>> 녹화 종료, 녹화 시간 ");
  Serial.print((millis() - recStart) / 1000);
  Serial.println("초");
}

void checkButton() {
  static bool stable = LOW, lastRead = LOW;
  static unsigned long changedAt = 0, lastToggle = 0;
  bool v = digitalRead(PIN_BUTTON);          // 이 키트는 누르면 HIGH
  if (v != lastRead) { lastRead = v; changedAt = millis(); }
  if (millis() - changedAt > 40 && v != stable) {       // 40ms 동안 같으면 진짜 입력 (디바운스)
    stable = v;
    if (stable == HIGH && millis() - lastToggle > 1500) {  // 1.5초 안의 연타는 무시
      lastToggle = millis();
      if (recording) stopRecord(); else startRecord();
    }
  }
}

// ---------------------------------------------------------------- 배터리 / 충전
int voltToPercent(float v) {     // 실제 펌웨어와 같은 LiPo 전압표
  const float tbl[][2] = {{4.20, 100}, {4.10, 90}, {4.00, 80}, {3.90, 65}, {3.80, 50},
                          {3.70, 30}, {3.60, 15}, {3.50, 7}, {3.30, 0}};
  if (v >= tbl[0][0]) return 100;
  for (int i = 1; i < 9; i++)
    if (v >= tbl[i][0])
      return tbl[i][1] + (v - tbl[i][0]) / (tbl[i - 1][0] - tbl[i][0]) * (tbl[i - 1][1] - tbl[i][1]);
  return 0;
}

float batteryV = 0;

void readPower() {
  static unsigned long last = 0;
  if (millis() - last < 200) return;
  last = millis();
  // 슬라이드 0~1023 을 배터리 전압 3.3~4.2V 로 바꿔서 흉내 낸다
  batteryV = 3.3 + analogRead(PIN_BAT) * (4.2 - 3.3) / 1023.0;
  batteryPct = voltToPercent(batteryV);
  charging = digitalRead(PIN_TOUCH) == HIGH;
}

// ---------------------------------------------------------------- 상태 보고 (MQTT 대신 시리얼)
void report() {
  static unsigned long last = 0;
  if (millis() - last < 2000) return;
  last = millis();
  const char* mode = recording ? "recording" : charging ? "charging" : "buffering";
  Serial.print("{\"battery\":"); Serial.print(batteryPct);
  Serial.print(",\"volt\":");     Serial.print(batteryV, 2);
  Serial.print(",\"charging\":"); Serial.print(charging ? "true" : "false");
  Serial.print(",\"mode\":\"");   Serial.print(mode);
  Serial.println("\"}");
}

// ----------------------------------------------------------------
void setup() {
  Serial.begin(9600);
  pinMode(PIN_BUTTON, INPUT);
  pinMode(PIN_TOUCH, INPUT);
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_SD_LED, OUTPUT);
  led(1, 1, 1);                    // 부팅 중: 흰색
  delay(500);
  Serial.println("미니 바디캠 시작");
}

void loop() {
  checkButton();
  readPower();
  updateLed();
  updateBeep();
  report();
}
