/*
  미니 바디캠 v2 (코딩어레이 키트 연습용)

  v1 기능: 버튼 녹화, 상태 LED, 부저 알림, 배터리(슬라이드), 충전(터치)
  v2 추가:
  - 초음파 센서: 사람이 30cm 안으로 1초 이상 다가오면 자동으로 녹화 시작,
                 멀어진 뒤 10초가 지나면 자동 종료 (실제 바디캠의 "자동 녹화 트리거" 개념)
                 버튼으로 시작한 녹화는 버튼으로만 끈다
  - 서보모터: 평상시에는 배터리 게이지 바늘 (0도 = 0%, 180도 = 100%)
              녹화 중에는 좌우로 천천히 회전 (카메라 회전대 흉내)

  [핀]
  버튼 D8, 터치 D7, 부저 D6, RGB D9 D10 D11, Red LED D13, 슬라이드 A0
  초음파 Trig D5 (보내기), Echo D4 (받기)
  서보 D3
*/

#include <Servo.h>

// ---- 핀 번호 ----
const int PIN_BUTTON = 8;
const int PIN_TOUCH  = 7;
const int PIN_BUZZER = 6;
const int PIN_RED    = 9;
const int PIN_GREEN  = 10;
const int PIN_BLUE   = 11;
const int PIN_SD_LED = 13;
const int PIN_BAT    = A0;
const int PIN_SERVO  = 3;
const int PIN_TRIG   = 5;          // 초음파 보내기
const int PIN_ECHO   = 4;          // 초음파 받기

// ---- 설정 ----
const int LOW_BATTERY    = 15;     // %
const int APPROACH_CM    = 30;     // 이 거리 안으로 오면 "다가옴"
const unsigned long APPROACH_MS = 1000;   // 이만큼 머물러야 자동 녹화 (지나가는 것 무시)
const unsigned long LEAVE_MS    = 10000;  // 멀어진 뒤 이만큼 지나면 자동 종료

// ---- 상태 ----
bool recording = false;
bool autoRec = false;              // 초음파로 자동 시작한 녹화인지
bool charging = false;
int batteryPct = 100;
float batteryV = 0;
unsigned long recStart = 0;
int distanceCm = -1;               // -1 = 측정 안 됨
Servo servo;

// ---------------------------------------------------------------- LED
void led(bool r, bool g, bool b) {
  digitalWrite(PIN_RED, r);
  digitalWrite(PIN_GREEN, g);
  digitalWrite(PIN_BLUE, b);
}

void updateLed() {
  unsigned long t = millis();
  if (recording && autoRec)   led(1, 0, (t / 300) % 2);      // 자동 녹화: 빨강 + 보라 번갈아
  else if (recording)         led(1, 0, 0);                  // 버튼 녹화: 빨강 켜짐
  else if (charging)          led(0, 0, (t / 500) % 2);      // 충전 중: 파랑 깜빡임
  else if (batteryPct <= LOW_BATTERY) led((t / 150) % 2, 0, 0);  // 배터리 부족
  else                        led(0, (t % 3000) < 300, 0);   // 평상시: 초록 잠깐
  digitalWrite(PIN_SD_LED, recording && (t / 250) % 2);
}

// ---------------------------------------------------------------- 부저
int beepPattern = 0;               // 1 = 시작, 2 = 종료, 3 = 자동 시작 (짧게 3번)
unsigned long beepStart = 0;

void beep(int pattern) { beepPattern = pattern; beepStart = millis(); }

void updateBeep() {
  unsigned long t = millis() - beepStart;
  bool on = false;
  if (beepPattern == 1) on = t < 150;
  if (beepPattern == 2) on = t < 120 || (t > 250 && t < 370);
  if (beepPattern == 3) on = (t < 80) || (t > 160 && t < 240) || (t > 320 && t < 400);
  if (on) tone(PIN_BUZZER, beepPattern == 3 ? 2600 : 2000); else noTone(PIN_BUZZER);
  if (beepPattern && t > 600) beepPattern = 0;
}

// ---------------------------------------------------------------- 녹화
void startRecord(bool byAuto) {
  recording = true;
  autoRec = byAuto;
  recStart = millis();
  beep(byAuto ? 3 : 1);
  Serial.println(byAuto ? ">>> 자동 녹화 시작 (누군가 다가옴)" : ">>> 녹화 시작 (버튼)");
}

void stopRecord(const char* why) {
  recording = false;
  autoRec = false;
  beep(2);
  Serial.print(">>> 녹화 종료 (");
  Serial.print(why);
  Serial.print("), 녹화 시간 ");
  Serial.print((millis() - recStart) / 1000);
  Serial.println("초");
}

void checkButton() {
  static bool stable = LOW, lastRead = LOW;
  static unsigned long changedAt = 0, lastToggle = 0;
  bool v = digitalRead(PIN_BUTTON);
  if (v != lastRead) { lastRead = v; changedAt = millis(); }
  if (millis() - changedAt > 40 && v != stable) {
    stable = v;
    if (stable == HIGH && millis() - lastToggle > 1500) {
      lastToggle = millis();
      if (recording) stopRecord("버튼");
      else startRecord(false);
    }
  }
}

// ---------------------------------------------------------------- 초음파 센서
// trig 핀으로 10us 신호를 보내고, echo 핀에 돌아온 시간으로 거리를 잰다
// 소리는 1cm 가는 데 약 29us, 왕복이므로 58로 나눈다
int measureCm() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 25000UL);   // 최대 25ms 기다림 (약 4m)
  if (us == 0) return -1;                                // 반사가 안 돌아옴
  return us / 58;
}

void updateDistance() {
  static unsigned long lastMeasure = 0, nearSince = 0, farSince = 0;
  if (millis() - lastMeasure < 100) return;
  lastMeasure = millis();
  distanceCm = measureCm();
  bool near = distanceCm > 0 && distanceCm < APPROACH_CM;

  if (near) {
    farSince = 0;
    if (!nearSince) nearSince = millis();
    if (!recording && millis() - nearSince > APPROACH_MS) startRecord(true);
  } else {
    nearSince = 0;
    if (!farSince) farSince = millis();
    if (recording && autoRec && millis() - farSince > LEAVE_MS) stopRecord("자동: 멀어짐");
  }
}

// ---------------------------------------------------------------- 서보모터
void updateServo() {
  static unsigned long last = 0;
  static int angle = 90;
  if (millis() - last < 30) return;
  last = millis();
  int target;
  if (recording) {
    // 녹화 중: 4초에 한 번 왕복하며 좌우 회전 (30도 ~ 150도)
    unsigned long t = (millis() - recStart) % 4000;
    target = t < 2000 ? map(t, 0, 2000, 30, 150) : map(t, 2000, 4000, 150, 30);
  } else {
    target = map(batteryPct, 0, 100, 0, 180);       // 배터리 게이지
  }
  // 한 번에 최대 4도씩만 움직여서 덜컹거리지 않게
  if (target > angle + 4) angle += 4;
  else if (target < angle - 4) angle -= 4;
  else angle = target;
  servo.write(angle);
}

// ---------------------------------------------------------------- 배터리 / 충전
int voltToPercent(float v) {
  const float tbl[][2] = {{4.20, 100}, {4.10, 90}, {4.00, 80}, {3.90, 65}, {3.80, 50},
                          {3.70, 30}, {3.60, 15}, {3.50, 7}, {3.30, 0}};
  if (v >= tbl[0][0]) return 100;
  for (int i = 1; i < 9; i++)
    if (v >= tbl[i][0])
      return tbl[i][1] + (v - tbl[i][0]) / (tbl[i - 1][0] - tbl[i][0]) * (tbl[i - 1][1] - tbl[i][1]);
  return 0;
}

void readPower() {
  static unsigned long last = 0;
  if (millis() - last < 200) return;
  last = millis();
  batteryV = 3.3 + analogRead(PIN_BAT) * (4.2 - 3.3) / 1023.0;
  batteryPct = voltToPercent(batteryV);
  charging = digitalRead(PIN_TOUCH) == HIGH;
}

// ---------------------------------------------------------------- 상태 보고
void report() {
  static unsigned long last = 0;
  if (millis() - last < 2000) return;
  last = millis();
  const char* mode = recording ? (autoRec ? "auto_recording" : "recording") : charging ? "charging" : "buffering";
  Serial.print("{\"battery\":"); Serial.print(batteryPct);
  Serial.print(",\"volt\":");     Serial.print(batteryV, 2);
  Serial.print(",\"charging\":"); Serial.print(charging ? "true" : "false");
  Serial.print(",\"distance\":"); Serial.print(distanceCm);
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
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  led(1, 1, 1);                     // 부팅 중: 흰색
  Serial.println("미니 바디캠 v2 시작");

  servo.attach(PIN_SERVO);
  servo.write(90);
  delay(300);
}

void loop() {
  checkButton();
  readPower();
  updateDistance();
  updateServo();
  updateLed();
  updateBeep();
  report();
}
