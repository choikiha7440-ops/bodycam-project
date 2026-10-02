/*
  미니 바디캠 v5 (코딩어레이 키트 연습용)

  v5 변경: 초음파 자동 녹화를 더 잘 감지하도록 개선
  - 한 번 잰 값 대신 3번 재서 가운데 값(중앙값)을 사용 -> 튀는 값 제거
  - "1초 동안 한 번도 안 놓치고 가까이" 대신 "가까이 점수"를 쌓는 방식
    (가끔 측정이 실패해도 녹화가 시작됨)
  - 화면에 거리 막대와 감지 점수 표시, 시리얼에 'd' 를 보내면 거리 값을 계속 출력

  v3 추가: OLED 화면에 "촬영중", 녹화 시간, 배터리, 거리 표시
  - 라이브러리 매니저에서 "U8g2" (olikraus) 설치 필요
  - OLED는 A4(SDA), A5(SCL)를 쓰므로 써미스터(A4), 불꽃 센서(A5)의 DIP 스위치는 OFF
  - 같은 폴더에 hangul.h 파일이 있어야 합니다

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
#include <U8g2lib.h>
#include <Wire.h>
#include "hangul.h"

// ---- OLED 화면 ----
// 화면이 안 나오거나 글자가 옆으로 2칸 밀려 보이면 아래 줄 대신 SH1106 줄을 쓰세요
U8G2_SSD1306_128X64_NONAME_1_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
// U8G2_SH1106_128X64_NONAME_1_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
// 화면이 거꾸로 보이면 U8G2_R0 을 U8G2_R2 로 바꾸세요

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
const int APPROACH_CM    = 30;     // 이 거리 안으로 오면 "가까움"
const int NEAR_SCORE_MAX = 10;     // 가까이 점수 최대값
const int NEAR_SCORE_ON  = 6;      // 점수가 이만큼 쌓이면 자동 녹화 (약 1초)
const unsigned long LEAVE_MS = 10000;     // 마지막으로 가까웠던 뒤 이만큼 지나면 자동 종료

// ---- 상태 ----
bool recording = false;
bool autoRec = false;              // 초음파로 자동 시작한 녹화인지
bool charging = false;
int batteryPct = 100;
float batteryV = 0;
unsigned long recStart = 0;
int distanceCm = -1;               // -1 = 측정 안 됨
int nearScore = 0;                 // 가까이 점수 (0 ~ NEAR_SCORE_MAX)
bool debugDistance = false;        // 시리얼에 'd' 를 보내면 켜짐
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
  if (byAuto) Serial.println(F(">>> 자동 녹화 시작 (누군가 다가옴)"));
  else Serial.println(F(">>> 녹화 시작 (버튼)"));
}

void stopRecord(const char* why) {
  recording = false;
  autoRec = false;
  beep(2);
  Serial.print(F(">>> 녹화 종료 ("));
  Serial.print(why);
  Serial.print(F("), 녹화 시간 "));
  Serial.print((millis() - recStart) / 1000);
  Serial.println(F("초"));
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
// trig 핀으로 10us 신호를 보내고, echo 핀에 소리가 돌아온 시간으로 거리를 잰다
// 소리는 1cm 가는 데 약 29us, 왕복이므로 58로 나눈다
int measureOnce() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 15000UL);   // 최대 15ms (약 2.5m까지)
  if (us == 0) return -1;                                // 반사가 안 돌아옴
  return us / 58;
}

// 3번 재서 가운데 값을 쓴다. 한 번씩 튀는 값을 없애는 간단한 방법
int measureCm() {
  int v[3];
  for (int i = 0; i < 3; i++) {
    v[i] = measureOnce();
    if (v[i] < 0) v[i] = 999;          // 실패는 "아주 멂"으로 취급
    delay(5);                          // 이전 소리의 메아리가 사라질 시간
  }
  // 세 값 정렬해서 가운데 값
  if (v[0] > v[1]) { int t = v[0]; v[0] = v[1]; v[1] = t; }
  if (v[1] > v[2]) { int t = v[1]; v[1] = v[2]; v[2] = t; }
  if (v[0] > v[1]) { int t = v[0]; v[0] = v[1]; v[1] = t; }
  return v[1] >= 999 ? -1 : v[1];
}

void updateDistance() {
  static unsigned long lastMeasure = 0, lastNear = 0;
  if (millis() - lastMeasure < 150) return;
  lastMeasure = millis();
  distanceCm = measureCm();
  bool near = distanceCm > 0 && distanceCm < APPROACH_CM;

  // 가까우면 점수 +2, 아니면 -1 (가끔 놓쳐도 계속 쌓이도록)
  if (near) { nearScore = min(NEAR_SCORE_MAX, nearScore + 2); lastNear = millis(); }
  else      { nearScore = max(0, nearScore - 1); }

  if (debugDistance) {
    Serial.print(F("거리 ")); Serial.print(distanceCm);
    Serial.print(F("cm  점수 ")); Serial.println(nearScore);
  }

  if (!recording && nearScore >= NEAR_SCORE_ON) startRecord(true);
  if (recording && autoRec && millis() - lastNear > LEAVE_MS) stopRecord("자동: 멀어짐");
}

void serialCommand() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd' || c == 'D') {
      debugDistance = !debugDistance;
      Serial.println(debugDistance ? F("거리 출력 켬") : F("거리 출력 끔"));
    }
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

// ---------------------------------------------------------------- OLED 화면
void k16(int x, int y, const unsigned char* g) { oled.drawXBMP(x, y, 16, 16, g); }   // 작은 한글
void k32(int x, int y, const unsigned char* g) { oled.drawXBMP(x, y, 32, 32, g); }   // 큰 한글

void drawBattery(int x, int y) {                  // 작은 배터리 그림 + %
  oled.drawFrame(x, y, 22, 10);
  oled.drawBox(x + 22, y + 3, 2, 4);
  oled.drawBox(x + 2, y + 2, map(batteryPct, 0, 100, 0, 18), 6);
  oled.setFont(u8g2_font_6x10_tr);
  oled.setCursor(x + 27, y + 9);
  oled.print(batteryPct);
  oled.print('%');
}

void drawScreen() {
  unsigned long t = millis();
  if (recording) {
    // ● 촬영중 (크게), 녹화 시간, 자동/수동
    if ((t / 500) % 2) oled.drawDisc(8, 16, 6);                 // 깜빡이는 빨간 점 대신 동그라미
    k32(18, 0, K32_CHAL); k32(52, 0, K32_YEONG); k32(86, 0, K32_JUNG);
    unsigned long sec = (t - recStart) / 1000;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", sec / 60, sec % 60);
    oled.setFont(u8g2_font_logisoso16_tn);
    oled.drawStr(2, 53, buf);
    if (autoRec) { k16(62, 37, K16_JA); k16(78, 37, K16_DONG); }
    else         { k16(62, 37, K16_SU); k16(78, 37, K16_DONG); }
    drawBattery(74, 54);
  } else {
    // 상태 제목
    if (charging)      { k16(0, 0, K16_CHUNG); k16(16, 0, K16_JEON); k16(36, 0, K16_JUNG); }
    else if (batteryPct <= LOW_BATTERY) { k16(0, 0, K16_BAE); k16(16, 0, K16_TEO); k16(32, 0, K16_RI);
                                          k16(52, 0, K16_BU); k16(68, 0, K16_JOK); }
    else               { k16(0, 0, K16_DAE); k16(16, 0, K16_GI); k16(36, 0, K16_JUNG); }
    oled.drawHLine(0, 18, 128);
    // 배터리 크게
    char buf[6];
    snprintf(buf, sizeof(buf), "%d", batteryPct);
    oled.setFont(u8g2_font_logisoso20_tn);
    oled.drawStr(0, 46, buf);
    int w = oled.getStrWidth(buf);                // 숫자 폭만큼 오른쪽에 % 붙이기
    oled.setFont(u8g2_font_6x10_tr);
    oled.drawStr(w + 3, 46, "%");
    oled.setCursor(70, 32); oled.print(batteryV, 2); oled.print('V');
    // 거리와 감지 막대 (막대가 가득 차면 자동 녹화)
    oled.setCursor(70, 46);
    if (distanceCm > 0) { oled.print(distanceCm); oled.print(F("cm")); }
    else oled.print(F("--"));
    oled.drawFrame(0, 52, 64, 8);
    oled.drawBox(2, 54, map(nearScore, 0, NEAR_SCORE_ON, 0, 60) > 60 ? 60 : map(nearScore, 0, NEAR_SCORE_ON, 0, 60), 4);
    oled.drawStr(70, 62, "cam01");
  }
}

void updateScreen() {
  static unsigned long last = 0;
  if (millis() - last < 250) return;              // 0.25초마다 다시 그림
  last = millis();
  oled.firstPage();                               // 메모리를 아끼려고 8조각으로 나눠 그림
  do { drawScreen(); } while (oled.nextPage());
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
  Serial.print(F("{\"battery\":")); Serial.print(batteryPct);
  Serial.print(F(",\"volt\":"));     Serial.print(batteryV, 2);
  Serial.print(F(",\"charging\":")); Serial.print(charging ? F("true") : F("false"));
  Serial.print(F(",\"distance\":")); Serial.print(distanceCm);
  Serial.print(F(",\"near\":"));     Serial.print(nearScore);
  Serial.print(F(",\"mode\":\""));   Serial.print(mode);
  Serial.println(F("\"}"));
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
  Serial.println(F("미니 바디캠 v5 시작 (시리얼에 d 를 보내면 거리 값 출력)"));
  oled.begin();
  oled.setBusClock(400000);        // I2C 빠르게

  servo.attach(PIN_SERVO);
  servo.write(90);
  delay(300);
}

void loop() {
  serialCommand();
  checkButton();
  readPower();
  updateDistance();
  updateServo();
  updateLed();
  updateBeep();
  updateScreen();
  report();
}
