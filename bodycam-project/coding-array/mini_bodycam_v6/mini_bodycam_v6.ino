/*
  미니 바디캠 v6 (코딩어레이 키트 연습용) - 키트의 거의 모든 모듈 사용

  [자동 녹화 조건] 하나라도 해당되면 녹화 시작, 모든 조건이 10초 동안 조용하면 자동 종료
    거리  초음파(D5/D4)   : 30cm 안에 1초쯤 머무름
    소리  마이크(A3)      : 평소보다 큰 소리
    온도  온습도(D12)     : 평소보다 2도 이상 오르거나 35도 이상
    밝기  조도센서(A1)    : 밝기가 갑자기 크게 바뀜 (불 켜짐/꺼짐, 손전등)
    자석  홀 센서(A2)     : 자석이 가까워지거나 멀어짐 (홀스터에서 꺼내는 것 흉내)
    불꽃  불꽃센서(A5)    : OLED를 안 쓸 때만 (USE_OLED 0)
    버튼(D8)으로 시작한 녹화는 버튼으로만 끈다

  [그 밖의 모듈]
    슬라이드(A0) = 배터리 전압,  터치(D7) = USB 충전 연결
    RGB LED(D9~11) = 상태,  Red LED(D13) = SD 저장 표시,  부저(D6) = 진동 대신
    서보(D3) = 평소 배터리 게이지 / 녹화 중 좌우 회전
    OLED(A4/A5, I2C) = 상태 화면,  써미스터(A4) = OLED를 안 쓸 때만 값 표시

  [준비]
  - 라이브러리: U8g2 (olikraus). 같은 폴더에 hangul.h
  - 확장 쉴드 밑 아두이노 보드의 "A <-> I2C" 스위치: OLED를 쓰면 I2C
  - DIP 스위치: 디지털 3~8, 12, 13 ON / 아날로그 1~4번(A0~A3) ON
    (OLED 사용 시 아날로그 5, 6번(A4, A5)은 OFF)
  - 시리얼 모니터에 d 를 보내면 모든 센서 값을 0.5초마다 출력 (기준값 맞출 때 사용)
*/

#define USE_OLED 1          // OLED를 안 쓰고 써미스터, 불꽃 센서를 쓰려면 0

#include <Servo.h>
#if USE_OLED
#include <U8g2lib.h>
#include <Wire.h>
#include "hangul.h"
U8G2_SSD1306_128X64_NONAME_1_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
// U8G2_SH1106_128X64_NONAME_1_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);   // 글자가 2칸 밀리면 이 줄 사용
#endif

// ---------------------------------------------------------------- 핀
const int PIN_SERVO  = 3;
const int PIN_ECHO   = 4;
const int PIN_TRIG   = 5;
const int PIN_BUZZER = 6;
const int PIN_TOUCH  = 7;
const int PIN_BUTTON = 8;
const int PIN_RED    = 9;
const int PIN_GREEN  = 10;
const int PIN_BLUE   = 11;
const int PIN_DHT    = 12;
const int PIN_SD_LED = 13;
const int PIN_BAT    = A0;
const int PIN_LIGHT  = A1;
const int PIN_HALL   = A2;
const int PIN_MIC    = A3;
const int PIN_THERM  = A4;         // OLED 안 쓸 때만
const int PIN_FLAME  = A5;         // OLED 안 쓸 때만

// ---------------------------------------------------------------- 설정 (d 로 값을 보면서 조절)
bool USE_DIST = true, USE_SOUND = true, USE_TEMP = true, USE_LIGHT = true, USE_HALL = true, USE_FLAME = true;
const int APPROACH_CM   = 30;      // 거리: 이보다 가까우면 "가까움"
const int NEAR_SCORE_ON = 6;       // 거리: 가까움 점수가 이만큼 쌓이면 녹화
const int SOUND_DELTA   = 80;      // 소리: 평소 소리 크기보다 이만큼 크면
const int TEMP_RISE     = 2;       // 온도: 평소보다 이만큼(도) 오르면
const int TEMP_LIMIT    = 35;      // 온도: 이 온도 이상이면
const int LIGHT_DELTA   = 120;     // 밝기: 평소 밝기에서 이만큼 바뀌면
const int HALL_DELTA    = 60;      // 자석: 평소 값에서 이만큼 바뀌면
const int FLAME_DELTA   = 100;     // 불꽃: 평소 값에서 이만큼 바뀌면
const int LOW_BATTERY   = 15;      // %
const unsigned long LEAVE_MS  = 10000;   // 모든 조건이 이만큼 조용하면 자동 종료
const unsigned long WARMUP_MS = 5000;    // 켜진 뒤 평소 값을 배우는 시간 (이동안 자동 녹화 안 함)

// ---------------------------------------------------------------- 상태
enum Reason { R_NONE, R_BUTTON, R_DIST, R_SOUND, R_TEMP, R_LIGHT, R_HALL, R_FLAME };
const char* reasonName(Reason r) {
  switch (r) {
    case R_BUTTON: return "button"; case R_DIST: return "distance"; case R_SOUND: return "sound";
    case R_TEMP: return "temp";     case R_LIGHT: return "light";   case R_HALL: return "magnet";
    case R_FLAME: return "flame";   default: return "none";
  }
}

bool recording = false;
Reason recReason = R_NONE;
unsigned long recStart = 0, lastTrigger = 0;
bool charging = false;
int batteryPct = 100;
float batteryV = 0;
bool debugOut = false;

int distanceCm = -1, nearScore = 0;
int soundLevel = 0;  float soundBase = -1;
int tempC = -99, humidity = -1;  float tempBase = -99;
int light = 0;  float lightBase = -1;
int hall = 0;   float hallBase = -1;
int flame = 0;  float flameBase = -1;
int therm = 0;
bool trig[8];                      // 지금 켜져 있는 녹화 조건
Servo servo;

bool warmedUp() { return millis() > WARMUP_MS; }
// 평소 값을 천천히 따라가는 기준값 (갑작스러운 변화만 잡아내기 위함)
void follow(float &base, int v, float k) { base = base < 0 ? v : base + (v - base) * k; }

// ---------------------------------------------------------------- LED / 부저
void led(bool r, bool g, bool b) { digitalWrite(PIN_RED, r); digitalWrite(PIN_GREEN, g); digitalWrite(PIN_BLUE, b); }

void updateLed() {
  unsigned long t = millis();
  if (recording && recReason != R_BUTTON) led(1, 0, (t / 300) % 2);   // 자동 녹화: 빨강/보라
  else if (recording)                     led(1, 0, 0);               // 버튼 녹화: 빨강
  else if (charging)                      led(0, 0, (t / 500) % 2);   // 충전: 파랑 깜빡임
  else if (batteryPct <= LOW_BATTERY)     led((t / 150) % 2, 0, 0);   // 배터리 부족
  else if (!warmedUp())                   led(1, 1, 0);               // 준비 중: 노랑
  else                                    led(0, (t % 3000) < 300, 0);// 평상시: 초록 잠깐
  digitalWrite(PIN_SD_LED, recording && (t / 250) % 2);
}

int beepPattern = 0;
unsigned long beepStart = 0;
void beep(int p) { beepPattern = p; beepStart = millis(); }
void updateBeep() {
  unsigned long t = millis() - beepStart;
  bool on = false;
  if (beepPattern == 1) on = t < 150;
  if (beepPattern == 2) on = t < 120 || (t > 250 && t < 370);
  if (beepPattern == 3) on = t < 80 || (t > 160 && t < 240) || (t > 320 && t < 400);
  if (on) tone(PIN_BUZZER, beepPattern == 3 ? 2600 : 2000); else noTone(PIN_BUZZER);
  if (beepPattern && t > 600) beepPattern = 0;
}

// ---------------------------------------------------------------- 녹화
void startRecord(Reason r) {
  recording = true;
  recReason = r;
  recStart = lastTrigger = millis();
  beep(r == R_BUTTON ? 1 : 3);
  Serial.print(F(">>> 녹화 시작, 이유: ")); Serial.println(reasonName(r));
}

void stopRecord(const __FlashStringHelper* why) {
  recording = false;
  beep(2);
  Serial.print(F(">>> 녹화 종료 (")); Serial.print(why);
  Serial.print(F("), 녹화 시간 ")); Serial.print((millis() - recStart) / 1000); Serial.println(F("초"));
  recReason = R_NONE;
}

void checkButton() {
  static bool stable = LOW, lastRead = LOW;
  static unsigned long changedAt = 0, lastToggle = 0;
  bool v = digitalRead(PIN_BUTTON);                  // 키트 버튼은 누르면 HIGH
  if (v != lastRead) { lastRead = v; changedAt = millis(); }
  if (millis() - changedAt > 40 && v != stable) {
    stable = v;
    if (stable == HIGH && millis() - lastToggle > 1500) {
      lastToggle = millis();
      if (recording) stopRecord(F("버튼"));
      else startRecord(R_BUTTON);
    }
  }
}

// ---------------------------------------------------------------- 센서: 초음파 (거리)
int measureOnce() {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(3);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, 15000UL);
  return us == 0 ? 999 : us / 58;
}

void updateDistance() {
  static unsigned long last = 0;
  if (millis() - last < 150) return;
  last = millis();
  int a = measureOnce(); delay(5);
  int b = measureOnce(); delay(5);
  int c = measureOnce();
  int mid = max(min(a, b), min(max(a, b), c));       // 세 값의 가운데 값
  distanceCm = mid >= 999 ? -1 : mid;
  bool near = distanceCm > 0 && distanceCm < APPROACH_CM;
  nearScore = near ? min(10, nearScore + 2) : max(0, nearScore - 1);
  trig[R_DIST] = USE_DIST && nearScore >= NEAR_SCORE_ON;
}

// ---------------------------------------------------------------- 센서: 마이크 (소리)
void updateSound() {
  static unsigned long last = 0;
  if (millis() - last < 100) return;
  last = millis();
  int mn = 1023, mx = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 20) {                       // 20ms 동안 가장 큰 값 - 가장 작은 값 = 소리 크기
    int v = analogRead(PIN_MIC);
    mn = min(mn, v); mx = max(mx, v);
  }
  soundLevel = mx - mn;
  bool loud = soundBase >= 0 && soundLevel > soundBase + SOUND_DELTA;
  if (!loud) follow(soundBase, soundLevel, 0.05);    // 큰 소리는 평소 값에 섞지 않음
  trig[R_SOUND] = USE_SOUND && loud && warmedUp();
}

// ---------------------------------------------------------------- 센서: 온습도 DHT-11 (온도)
bool readDHT11(int &t, int &h) {
  uint8_t d[5] = {0, 0, 0, 0, 0};
  pinMode(PIN_DHT, OUTPUT);
  digitalWrite(PIN_DHT, LOW);
  delay(20);                                         // "측정해 줘" 신호
  pinMode(PIN_DHT, INPUT_PULLUP);
  if (pulseIn(PIN_DHT, HIGH, 300) == 0) return false;   // 센서의 대답
  for (int i = 0; i < 40; i++) {                     // 40비트: 짧은 HIGH = 0, 긴 HIGH = 1
    unsigned long w = pulseIn(PIN_DHT, HIGH, 200);
    if (w == 0) return false;
    d[i / 8] <<= 1;
    if (w > 45) d[i / 8] |= 1;
  }
  if ((uint8_t)(d[0] + d[1] + d[2] + d[3]) != d[4]) return false;   // 검산
  h = d[0]; t = d[2];
  return true;
}

void updateTemp() {
  static unsigned long last = 0;
  if (millis() - last < 2000) return;                // DHT-11은 2초에 한 번만 잴 수 있음
  last = millis();
  int t, h;
  if (!readDHT11(t, h)) return;
  tempC = t; humidity = h;
  if (tempBase < -50) tempBase = t;
  bool hot = t - tempBase >= TEMP_RISE || t >= TEMP_LIMIT;
  if (!hot) tempBase += (t - tempBase) * 0.05;
  trig[R_TEMP] = USE_TEMP && hot && warmedUp();
}

// ---------------------------------------------------------------- 센서: 조도, 홀, 불꽃, 써미스터
void updateAnalogSensors() {
  static unsigned long last = 0;
  if (millis() - last < 200) return;
  last = millis();

  light = analogRead(PIN_LIGHT);
  bool lightChange = lightBase >= 0 && abs(light - lightBase) > LIGHT_DELTA;
  follow(lightBase, light, 0.02);                    // 약 10초 동안 천천히 새 밝기에 적응
  trig[R_LIGHT] = USE_LIGHT && lightChange && warmedUp();

  hall = analogRead(PIN_HALL);
  bool magnet = hallBase >= 0 && abs(hall - hallBase) > HALL_DELTA;
  follow(hallBase, hall, 0.02);
  trig[R_HALL] = USE_HALL && magnet && warmedUp();

#if !USE_OLED
  flame = analogRead(PIN_FLAME);
  bool fire = flameBase >= 0 && abs(flame - flameBase) > FLAME_DELTA;
  follow(flameBase, flame, 0.02);
  trig[R_FLAME] = USE_FLAME && fire && warmedUp();
  therm = analogRead(PIN_THERM);
#endif
}

// ---------------------------------------------------------------- 자동 녹화 판단
void updateTriggers() {
  Reason active = R_NONE;
  for (int r = R_DIST; r <= R_FLAME; r++) if (trig[r]) { active = (Reason)r; break; }
  if (active != R_NONE) {
    lastTrigger = millis();
    if (!recording) startRecord(active);
  }
  if (recording && recReason != R_BUTTON && millis() - lastTrigger > LEAVE_MS) stopRecord(F("자동: 조용해짐"));
}

// ---------------------------------------------------------------- 서보
void updateServo() {
  static unsigned long last = 0;
  static int angle = 90;
  if (millis() - last < 30) return;
  last = millis();
  int target;
  if (recording) {
    unsigned long t = (millis() - recStart) % 4000;
    target = t < 2000 ? map(t, 0, 2000, 30, 150) : map(t, 2000, 4000, 150, 30);
  } else {
    target = map(batteryPct, 0, 100, 0, 180);
  }
  angle += constrain(target - angle, -4, 4);
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

// ---------------------------------------------------------------- OLED 화면
#if USE_OLED
void k16(int x, int y, const unsigned char* g) { oled.drawXBMP(x, y, 16, 16, g); }
void k32(int x, int y, const unsigned char* g) { oled.drawXBMP(x, y, 32, 32, g); }

void drawBattery(int x, int y) {
  oled.drawFrame(x, y, 22, 10);
  oled.drawBox(x + 22, y + 3, 2, 4);
  oled.drawBox(x + 2, y + 2, map(batteryPct, 0, 100, 0, 18), 6);
  oled.setFont(u8g2_font_6x10_tr);
  oled.setCursor(x + 26, y + 9);
  oled.print(batteryPct); oled.print('%');
}

void drawReason(int x, int y, Reason r) {           // 녹화 이유 두 글자
  const unsigned char *a = K16_SU, *b = K16_DONG;   // 기본: 수동
  if (r == R_DIST)  { a = K16_GEO;  b = K16_RI; }
  if (r == R_SOUND) { a = K16_SO;   b = K16_RI; }
  if (r == R_TEMP)  { a = K16_ON;   b = K16_DO; }
  if (r == R_LIGHT) { a = K16_BAL;  b = K16_GI; }
  if (r == R_HALL)  { a = K16_JA;   b = K16_SEOK; }
  k16(x, y, a); k16(x + 16, y, b);
}

void drawScreen() {
  unsigned long t = millis();
  char buf[12];
  oled.setFont(u8g2_font_6x10_tr);
  if (recording) {
    if ((t / 500) % 2) oled.drawDisc(8, 16, 6);
    k32(18, 0, K32_CHAL); k32(52, 0, K32_YEONG); k32(86, 0, K32_JUNG);
    unsigned long sec = (t - recStart) / 1000;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", sec / 60, sec % 60);
    oled.setFont(u8g2_font_logisoso16_tn);
    oled.drawStr(2, 53, buf);
    drawReason(62, 37, recReason);
    drawBattery(74, 54);
    return;
  }
  // 제목: 대기 중 / 충전 중 / 배터리 부족
  if (charging) { k16(0, 0, K16_CHUNG); k16(16, 0, K16_JEON); k16(36, 0, K16_JUNG); }
  else if (batteryPct <= LOW_BATTERY) { k16(0, 0, K16_BAE); k16(16, 0, K16_TEO); k16(32, 0, K16_RI);
                                        k16(52, 0, K16_BU); k16(68, 0, K16_JOK); }
  else { k16(0, 0, K16_DAE); k16(16, 0, K16_GI); k16(36, 0, K16_JUNG); }
  if (batteryPct > LOW_BATTERY) drawBattery(84, 3);
  oled.drawHLine(0, 18, 128);
  // 센서 값 (영어 약자: D 거리, T 온도, S 소리, H 습도, L 밝기, M 자석)
  oled.setFont(u8g2_font_6x10_tr);
  if (distanceCm > 0) snprintf(buf, sizeof(buf), "D %dcm", distanceCm); else strcpy(buf, "D --");
  oled.drawStr(0, 29, buf);
  if (tempC > -50) snprintf(buf, sizeof(buf), "T %dC", tempC); else strcpy(buf, "T --");
  oled.drawStr(66, 29, buf);
  snprintf(buf, sizeof(buf), "S %d", soundLevel);  oled.drawStr(0, 40, buf);
  if (humidity >= 0) snprintf(buf, sizeof(buf), "H %d%%", humidity); else strcpy(buf, "H --");
  oled.drawStr(66, 40, buf);
  snprintf(buf, sizeof(buf), "L %d", light);        oled.drawStr(0, 51, buf);
  snprintf(buf, sizeof(buf), "M %d", hall);         oled.drawStr(66, 51, buf);
  // 거리 감지 막대 + 준비 표시
  oled.drawFrame(0, 55, 60, 8);
  oled.drawBox(2, 57, constrain(map(nearScore, 0, NEAR_SCORE_ON, 0, 56), 0, 56), 4);
  oled.drawStr(66, 63, warmedUp() ? "cam01" : "wait..");
}

void updateScreen() {
  static unsigned long last = 0;
  if (millis() - last < 250) return;
  last = millis();
  oled.firstPage();
  do { drawScreen(); } while (oled.nextPage());
}
#endif

// ---------------------------------------------------------------- 시리얼 (상태 보고, 디버그)
void report() {
  static unsigned long last = 0;
  if (millis() - last < 2000) return;
  last = millis();
  Serial.print(F("{\"battery\":")); Serial.print(batteryPct);
  Serial.print(F(",\"charging\":")); Serial.print(charging ? F("true") : F("false"));
  Serial.print(F(",\"mode\":\""));
  Serial.print(recording ? F("recording") : charging ? F("charging") : F("buffering"));
  Serial.print(F("\",\"reason\":\"")); Serial.print(reasonName(recReason));
  Serial.print(F("\",\"dist\":"));  Serial.print(distanceCm);
  Serial.print(F(",\"temp\":"));    Serial.print(tempC);
  Serial.print(F(",\"hum\":"));     Serial.print(humidity);
  Serial.print(F(",\"sound\":"));   Serial.print(soundLevel);
  Serial.print(F(",\"light\":"));   Serial.print(light);
  Serial.print(F(",\"hall\":"));    Serial.print(hall);
#if !USE_OLED
  Serial.print(F(",\"flame\":"));   Serial.print(flame);
  Serial.print(F(",\"therm\":"));   Serial.print(therm);
#endif
  Serial.println('}');
}

void debugPrint() {
  static unsigned long last = 0;
  if (!debugOut || millis() - last < 500) return;
  last = millis();
  Serial.print(F("거리 "));  Serial.print(distanceCm); Serial.print(F("(점수 ")); Serial.print(nearScore);
  Serial.print(F(")  소리 ")); Serial.print(soundLevel); Serial.print('/'); Serial.print((int)soundBase);
  Serial.print(F("  온도 ")); Serial.print(tempC); Serial.print('/'); Serial.print((int)tempBase);
  Serial.print(F("  밝기 ")); Serial.print(light); Serial.print('/'); Serial.print((int)lightBase);
  Serial.print(F("  자석 ")); Serial.print(hall); Serial.print('/'); Serial.print((int)hallBase);
#if !USE_OLED
  Serial.print(F("  불꽃 ")); Serial.print(flame); Serial.print('/'); Serial.print((int)flameBase);
#endif
  Serial.print(F("  조건 "));
  for (int r = R_DIST; r <= R_FLAME; r++) Serial.print(trig[r] ? '1' : '0');
  Serial.println();
}

void serialCommand() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd' || c == 'D') {
      debugOut = !debugOut;
      Serial.println(debugOut ? F("센서 값 출력 켬 (값/평소값)") : F("센서 값 출력 끔"));
    }
  }
}

// ----------------------------------------------------------------
void setup() {
  Serial.begin(9600);
  pinMode(PIN_BUTTON, INPUT);
  pinMode(PIN_TOUCH, INPUT);
  pinMode(PIN_RED, OUTPUT); pinMode(PIN_GREEN, OUTPUT); pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_SD_LED, OUTPUT);
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  led(1, 1, 1);
  Serial.println(F("미니 바디캠 v6 시작. 5초 동안 평소 값을 배웁니다 (d = 센서 값 보기)"));
#if USE_OLED
  oled.begin();
  oled.setBusClock(400000);
#endif
  servo.attach(PIN_SERVO);
  servo.write(90);
}

void loop() {
  serialCommand();
  checkButton();
  readPower();
  updateDistance();
  updateSound();
  updateTemp();
  updateAnalogSensors();
  updateTriggers();
  updateServo();
  updateLed();
  updateBeep();
#if USE_OLED
  updateScreen();
#endif
  report();
  debugPrint();
}
