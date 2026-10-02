/*
  바디캠 펌웨어 - Seeed Studio XIAO ESP32S3 Sense

  [동작]
  - 평상시: 영상(10fps)과 음성을 PSRAM 링버퍼(16초)에 계속 덮어쓰고,
            SD카드에는 1분 단위 파일로 계속 저장하면서 오래된 것부터 지운다 (블랙박스)
  - 버튼 ON: 누르기 10초 전 분량부터 서버로 실시간 업로드 + SD에 별도 보호 저장
  - 버튼 OFF: 업로드를 마무리하고 평상시로 돌아간다
  - 와이파이가 끊겨도 녹화는 SD에 계속 저장되고, 다시 연결되면 못 보낸 부분을 자동 전송
  - RGB LED로 상태 표시, 5초마다 MQTT로 배터리/신호/상태 보고

  [아두이노 IDE 설정]
  - ESP32 보드 패키지 3.x (Espressif Systems)
  - 보드: XIAO_ESP32S3,  PSRAM: "OPI PSRAM" (필수),  USB CDC On Boot: Enabled
  - Partition Scheme: "Default with spiffs (3MB APP/1.5MB SPIFFS)" (기본값)
  - 라이브러리: PubSubClient (Nick O'Leary), WebSockets (Markus Sattler)

  [서버와 약속한 형식]
  - MQTT  bodycam/<id>/status, bodycam/<id>/event
  - WS    /ws/upload/<id>?session=<sid>
          바이너리 [1바이트 종류][8바이트 ms 타임스탬프][데이터], 종류 1=JPEG 2=PCM
          끝: {"cmd":"end"}
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <PubSubClient.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <sys/time.h>
#include "esp_camera.h"
#include "ESP_I2S.h"
#include "config.h"

// ---- 카메라 핀 (XIAO ESP32S3 Sense 고정값) ----
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y9_GPIO_NUM     48
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     16
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     17
#define Y2_GPIO_NUM     15
#define VSYNC_GPIO_NUM  38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13
// ---- 내장 PDM 마이크 ----
#define MIC_CLK_PIN     42
#define MIC_DATA_PIN    41

static const uint8_t K_JPEG = 1, K_PCM = 2;
static const uint32_t BUF_CAP = 160 * 1024;          // 프레임 하나 최대 크기
static const uint32_t AUDIO_CHUNK = AUDIO_RATE / 10 * 2;   // 100ms, 16bit mono

// =====================================================================
//  공용 상태
// =====================================================================
String camId;
Preferences prefs;
SemaphoreHandle_t sdMutex;
bool sdOk = false, camOk = false, micOk = false;

volatile int64_t epochOffsetMs = 0;   // epoch(ms) = 부팅후(ms) + offset
volatile bool timeSynced = false;
volatile bool wifiUp = false, mqttUp = false, liveUp = false;
volatile bool lagging = false;
volatile int pendingCount = 0;
volatile uint32_t frameCount = 0;

int batteryPct = -1;
float batteryV = 0;
bool usbPower = false;

int64_t toEpochMs(int64_t tsUs) { return tsUs / 1000 + epochOffsetMs; }

// ---- 녹화 상태 (버튼) ----
portMUX_TYPE stMux = portMUX_INITIALIZER_UNLOCKED;
// RecState 구조체는 config.h 에 있습니다 (아두이노가 함수 선언을 자동으로 앞에 끼워 넣기 때문에
// 직접 만든 구조체는 헤더에 정의해야 "does not name a type" 에러가 나지 않습니다)
RecState rec;
volatile bool evStart = false, evStop = false;

RecState getRec() {
  RecState r;
  portENTER_CRITICAL(&stMux);
  r = rec;
  portEXIT_CRITICAL(&stMux);
  return r;
}

// =====================================================================
//  링버퍼 (PSRAM). 영상과 음성을 순서번호(seq)로 관리
// =====================================================================
struct Item { uint32_t seq; uint8_t kind; int64_t tsUs; uint32_t len; uint8_t* data; };

class Ring {
 public:
  bool begin(size_t capacity) {
    cap = capacity;
    items = (Item*)ps_calloc(cap, sizeof(Item));
    mtx = xSemaphoreCreateMutex();
    return items && mtx;
  }
  void push(uint8_t kind, int64_t tsUs, const uint8_t* src, uint32_t len) {
    uint8_t* copy = (uint8_t*)ps_malloc(len);
    if (!copy) return;                      // 메모리 부족 시 이 프레임만 버림
    memcpy(copy, src, len);
    xSemaphoreTake(mtx, portMAX_DELAY);
    Item& slot = items[next % cap];
    uint8_t* old = slot.data;
    slot = {next, kind, tsUs, len, copy};
    next++;
    xSemaphoreGive(mtx);
    if (old) free(old);
  }
  uint32_t nextSeq() { return next; }
  uint32_t oldest() { uint32_t n = next; return n > cap ? n - cap : 0; }
  // 해당 시각 이후의 첫 순서번호
  uint32_t seqAtTime(int64_t tsUs) {
    xSemaphoreTake(mtx, portMAX_DELAY);
    uint32_t s = oldest();
    for (; s < next; s++) if (items[s % cap].tsUs >= tsUs) break;
    xSemaphoreGive(mtx);
    return s;
  }
  // seq 항목을 dst로 복사. 없거나 덮어써졌으면 false
  bool copy(uint32_t seq, uint8_t& kind, int64_t& tsUs, uint8_t* dst, uint32_t dstCap, uint32_t& len) {
    bool ok = false;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (seq >= oldest() && seq < next) {
      Item& it = items[seq % cap];
      if (it.seq == seq && it.data && it.len <= dstCap) {
        memcpy(dst, it.data, it.len);
        kind = it.kind; tsUs = it.tsUs; len = it.len; ok = true;
      }
    }
    xSemaphoreGive(mtx);
    return ok;
  }
 private:
  Item* items = nullptr;
  size_t cap = 0;
  volatile uint32_t next = 0;
  SemaphoreHandle_t mtx;
};
Ring ring;

// =====================================================================
//  진동 / LED
// =====================================================================
volatile uint8_t vibePattern = 0;   // 1=짧게1번 2=짧게2번 3=길게
uint32_t vibeStart = 0;

void vibe(uint8_t pattern) { vibePattern = pattern; vibeStart = millis(); }

void updateVibe() {
  if (PIN_VIBE < 0) return;
  bool on = false;
  uint32_t t = millis() - vibeStart;
  if (vibePattern == 1) on = t < 150;
  else if (vibePattern == 2) on = t < 120 || (t > 250 && t < 370);
  else if (vibePattern == 3) on = t < 700;
  if (vibePattern && t > 800) vibePattern = 0;
  digitalWrite(PIN_VIBE, on ? HIGH : LOW);
}

void led(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t last = 0xFFFFFFFF;
  uint32_t v = ((uint32_t)r << 16) | (g << 8) | b;
  if (v == last) return;
  last = v;
  rgbLedWrite(PIN_LED, (uint16_t)r * LED_BRIGHT / 255, (uint16_t)g * LED_BRIGHT / 255, (uint16_t)b * LED_BRIGHT / 255);
}

void updateLed() {
  uint32_t t = millis();
  RecState r = getRec();
  if (!camOk) {                                        // 카메라 고장: 빨강/흰색 교대
    (t / 300) % 2 ? led(255, 0, 0) : led(255, 255, 255);
  } else if (r.active) {
    if (liveUp) led(255, 0, 0);                        // 서버 녹화 중: 빨강 켜짐
    else (t / 400) % 2 ? led(255, 110, 0) : led(0, 0, 0);   // 와이파이 끊김, SD 보호저장: 주황 깜빡임
  } else if (usbPower) {
    if (batteryV >= 4.15f) led(0, 0, 255);             // 충전 완료: 파랑 켜짐
    else {                                             // 충전 중: 파랑 숨쉬기
      float k = (sinf(t / 600.0f) + 1) / 2;
      led(0, 0, (uint8_t)(40 + 215 * k));
    }
  } else if (batteryPct >= 0 && batteryPct <= LOW_BATTERY) {
    (t / 150) % 2 ? led(255, 0, 0) : led(0, 0, 0);    // 배터리 부족: 빨강 빠른 깜빡임
  } else if (pendingCount > 0 && wifiUp) {
    (t / 500) % 2 ? led(0, 200, 200) : led(0, 0, 0);  // 미전송 영상 보내는 중: 청록 깜빡임
  } else {
    (t % 3000) < 300 ? led(0, 255, 0) : led(0, 0, 0);  // 자체녹화 중: 초록 천천히 깜빡임
  }
}

// =====================================================================
//  배터리 / USB
// =====================================================================
int voltToPercent(float v) {
  static const float tbl[][2] = {{4.20f, 100}, {4.10f, 90}, {4.00f, 80}, {3.90f, 65}, {3.80f, 50},
                                 {3.70f, 30}, {3.60f, 15}, {3.50f, 7}, {3.30f, 0}};
  if (v >= tbl[0][0]) return 100;
  for (int i = 1; i < 9; i++)
    if (v >= tbl[i][0])
      return (int)(tbl[i][1] + (v - tbl[i][0]) / (tbl[i - 1][0] - tbl[i][0]) * (tbl[i - 1][1] - tbl[i][1]));
  return 0;
}

void readPower() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  float v = analogReadMilliVolts(PIN_BAT_ADC) * BAT_DIVIDER / 1000.0f;
  usbPower = digitalRead(PIN_USB_SENSE) == HIGH;
  if (v < 2.5f) { batteryPct = -1; batteryV = 0; return; }   // 분압 회로 미연결
  batteryV = batteryV == 0 ? v : batteryV * 0.8f + v * 0.2f;
  batteryPct = voltToPercent(batteryV);
}

// =====================================================================
//  버튼
// =====================================================================
void makeSession(char* out, size_t n) {
  uint16_t rnd = (uint16_t)esp_random();
  if (timeSynced) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(out, n, "%Y%m%d-%H%M%S", &tmv);
    snprintf(out + strlen(out), n - strlen(out), "-%04x", rnd);
  } else {
    snprintf(out, n, "boot%lu-%04x", (unsigned long)millis(), rnd);
  }
}

void startRecord() {
  int64_t nowUs = esp_timer_get_time();
  uint32_t startSeq = ring.seqAtTime(nowUs - (int64_t)PRE_SEC * 1000000LL);
  char sid[33];
  makeSession(sid, sizeof(sid));
  portENTER_CRITICAL(&stMux);
  rec.active = true;
  strncpy(rec.session, sid, sizeof(rec.session));
  rec.startSeq = startSeq;
  rec.stopSeq = 0;
  rec.startEpochMs = toEpochMs(nowUs);
  portEXIT_CRITICAL(&stMux);
  evStart = true;
  vibe(1);
  Serial.printf(">>> 녹화 시작 %s (사전녹화 seq %u부터)\n", sid, startSeq);
}

void stopRecord() {
  uint32_t stopSeq = ring.nextSeq();
  portENTER_CRITICAL(&stMux);
  rec.active = false;
  rec.stopSeq = stopSeq > rec.startSeq ? stopSeq : rec.startSeq + 1;
  portEXIT_CRITICAL(&stMux);
  evStop = true;
  vibe(2);
  Serial.println(">>> 녹화 종료");
}

void checkButton() {
  static bool stable = HIGH, lastRead = HIGH;
  static uint32_t changedAt = 0, lastToggle = 0;
  bool v = digitalRead(PIN_BUTTON);
  if (v != lastRead) { lastRead = v; changedAt = millis(); }
  if (millis() - changedAt > 40 && v != stable) {
    stable = v;
    if (stable == LOW && millis() - lastToggle > 1500) {   // 1.5초 안의 연타는 무시
      lastToggle = millis();
      if (!camOk) { vibe(3); return; }
      getRec().active ? stopRecord() : startRecord();
    }
  }
}

// =====================================================================
//  카메라 / 마이크 작업
// =====================================================================
bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = CAM_FRAME_SIZE;
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("카메라 초기화 실패 0x%x (PSRAM 설정과 카메라 연결을 확인하세요)\n", err);
    return false;
  }
  return true;
}

void camTask(void*) {
  for (;;) {
    uint32_t t0 = millis();
    int fps = lagging ? FPS / 2 : FPS;       // 전송이 밀리면 잠시 프레임을 줄임
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      ring.push(K_JPEG, esp_timer_get_time(), fb->buf, fb->len);
      esp_camera_fb_return(fb);
      frameCount++;
    }
    int wait = 1000 / fps - (int)(millis() - t0);
    vTaskDelay(pdMS_TO_TICKS(wait > 1 ? wait : 1));
  }
}

I2SClass i2s;

void audioTask(void*) {
  uint8_t* buf = (uint8_t*)malloc(AUDIO_CHUNK);
  for (;;) {
    size_t got = i2s.readBytes((char*)buf, AUDIO_CHUNK);
    if (got == 0) { vTaskDelay(10); continue; }
    int16_t* s = (int16_t*)buf;
    for (size_t i = 0; i < got / 2; i++) {
      int32_t v = (int32_t)s[i] * AUDIO_GAIN;
      s[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    int64_t ts = esp_timer_get_time() - (int64_t)got * 1000000 / (AUDIO_RATE * 2);
    ring.push(K_PCM, ts, buf, got);
  }
}

// =====================================================================
//  SD카드 (블랙박스 + 이벤트 보호 저장)
//  파일 형식: [1바이트 종류][8바이트 epoch ms][4바이트 길이][데이터] 반복
//    /loop/000123.bin   평상시 1분 단위, 오래된 것부터 삭제
//    /event/<sid>.bin   녹화 버튼 구간 (서버 전송 완료 시 삭제)
//    /event/<sid>.open  녹화 중 표시,  /event/<sid>.sent  서버가 받은 마지막 시각
// =====================================================================
bool sdLock(uint32_t ms = 2000) { return xSemaphoreTake(sdMutex, pdMS_TO_TICKS(ms)) == pdTRUE; }
void sdUnlock() { xSemaphoreGive(sdMutex); }

String evPath(const char* sid, const char* ext) { return String("/event/") + sid + ext; }

bool writeRecord(File& f, uint8_t kind, int64_t epochMs, const uint8_t* data, uint32_t len) {
  uint8_t h[13];
  h[0] = kind;
  memcpy(h + 1, &epochMs, 8);
  memcpy(h + 9, &len, 4);
  return f.write(h, 13) == 13 && f.write(data, len) == len;
}

void writeText(const String& path, const String& text) {
  File f = SD.open(path, FILE_WRITE);
  if (f) { f.print(text); f.close(); }
}

String readText(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

uint32_t lastLoopIndex() {
  uint32_t maxIdx = 0;
  File dir = SD.open("/loop");
  if (!dir) return 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    uint32_t idx = strtoul(f.name(), nullptr, 10);
    if (idx > maxIdx) maxIdx = idx;
    f.close();
  }
  dir.close();
  return maxIdx;
}

void countPending() {
  int n = 0;
  File dir = SD.open("/event");
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    f.close();
    if (name.endsWith(".bin") && !SD.exists("/event/" + name.substring(0, name.length() - 4) + ".open")) n++;
  }
  dir.close();
  pendingCount = n;
}

void sdTask(void*) {
  uint8_t* buf = (uint8_t*)ps_malloc(BUF_CAP);
  uint32_t loopCursor = ring.nextSeq(), evCursor = 0;
  uint32_t loopIdx = 0;
  int64_t segStartUs = 0;
  File loopFile, evFile;
  char evSid[33] = "";
  uint32_t evStop = 0, lastFlush = 0, lastCount = 0;
  const uint32_t keepSegs = LOOP_KEEP_MIN * 60 / LOOP_SEG_SEC;

  if (sdLock()) {
    loopIdx = lastLoopIndex();
    countPending();
    sdUnlock();
  }

  for (;;) {
    if (!sdOk) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    bool worked = false;
    RecState r = getRec();

    // 새 녹화가 시작되면 이벤트 파일을 열고, 사전녹화 지점부터 기록
    if (r.session[0] && strcmp(r.session, evSid) != 0 && (r.active || r.stopSeq)) {
      if (sdLock()) {
        if (evFile) evFile.close();
        strncpy(evSid, r.session, sizeof(evSid));
        writeText(evPath(evSid, ".open"), "1");
        evFile = SD.open(evPath(evSid, ".bin"), FILE_APPEND);
        sdUnlock();
      }
      evCursor = max(r.startSeq, ring.oldest());
      evStop = 0;
    }
    if (evFile && strcmp(r.session, evSid) == 0 && r.stopSeq) evStop = r.stopSeq;

    // 이벤트 파일 기록
    if (evFile) {
      if (evCursor < ring.oldest()) evCursor = ring.oldest();
      if (evStop && evCursor >= evStop) {
        if (sdLock()) {                                // 녹화 구간 끝: 파일 닫기
          evFile.close();
          SD.remove(evPath(evSid, ".open"));
          if (SD.exists(evPath(evSid, ".ok"))) {       // 이미 서버 전송 완료
            SD.remove(evPath(evSid, ".bin"));
            SD.remove(evPath(evSid, ".sent"));
            SD.remove(evPath(evSid, ".ok"));
          }
          countPending();
          sdUnlock();
        }
        loopCursor = max(loopCursor, evStop);
      } else if (evCursor < ring.nextSeq()) {
        uint8_t kind; int64_t ts; uint32_t len;
        if (ring.copy(evCursor, kind, ts, buf, BUF_CAP, len) && sdLock()) {
          writeRecord(evFile, kind, toEpochMs(ts), buf, len);
          sdUnlock();
        }
        evCursor++;
        worked = true;
      }
    }

    // 평상시 블랙박스 기록 (녹화 구간은 이벤트 파일에 있으므로 건너뜀)
    bool inEvent = evFile && (evStop == 0 || loopCursor < evStop);
    if (inEvent) {
      uint32_t lim = evStop ? min(evStop, ring.nextSeq()) : ring.nextSeq();
      loopCursor = max(loopCursor, lim);
    } else {
      if (loopCursor < ring.oldest()) loopCursor = ring.oldest();
      if (loopCursor < ring.nextSeq()) {
        uint8_t kind; int64_t ts; uint32_t len;
        if (ring.copy(loopCursor, kind, ts, buf, BUF_CAP, len) && sdLock()) {
          if (!loopFile || ts - segStartUs > (int64_t)LOOP_SEG_SEC * 1000000LL) {
            if (loopFile) loopFile.close();
            loopIdx++;
            char p[24];
            snprintf(p, sizeof(p), "/loop/%06lu.bin", (unsigned long)loopIdx);
            loopFile = SD.open(p, FILE_WRITE);
            segStartUs = ts;
            if (loopIdx > keepSegs) {                  // 덮어쓰기: 가장 오래된 파일 삭제
              snprintf(p, sizeof(p), "/loop/%06lu.bin", (unsigned long)(loopIdx - keepSegs));
              SD.remove(p);
            }
          }
          if (loopFile) writeRecord(loopFile, kind, toEpochMs(ts), buf, len);
          sdUnlock();
        }
        loopCursor++;
        worked = true;
      }
    }

    if (millis() - lastFlush > 2000 && sdLock()) {     // 전원이 갑자기 꺼져도 최대 2초만 손실
      if (loopFile) loopFile.flush();
      if (evFile) evFile.flush();
      sdUnlock();
      lastFlush = millis();
    }
    if (millis() - lastCount > 10000 && sdLock()) {
      countPending();
      sdUnlock();
      lastCount = millis();
    }
    if (!worked) vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool initSD() {
  const uint32_t speeds[] = {10000000, 4000000};     // 10MHz 먼저, 안 되면 4MHz
  bool sdBegun = false;
  for (int attempt = 0; attempt < 4 && !sdBegun; attempt++) {
    uint32_t hz = speeds[attempt % 2];
    SPI.begin(7, 8, 9, SD_CS_PIN);
    sdBegun = SD.begin(SD_CS_PIN, SPI, hz);
    if (sdBegun) { Serial.printf("SD카드 연결 (%u MHz, %d번째 시도)\n", (unsigned)(hz / 1000000), attempt + 1); }
    else { SD.end(); SPI.end(); delay(300); }
  }
  if (!sdBegun) {
    Serial.println("SD카드 없음 또는 인식 실패 (카드를 다시 끼우고 USB를 뽑았다 꽂아 보세요. FAT32 확인). 서버 업로드만 동작합니다");
    return false;
  }
  if (!SD.exists("/loop")) SD.mkdir("/loop");
  if (!SD.exists("/event")) SD.mkdir("/event");
  // 전원이 꺼지기 전 녹화 중이던 파일은 닫힌 것으로 처리
  File dir = SD.open("/event");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    f.close();
    if (name.endsWith(".open")) SD.remove("/event/" + name);
  }
  dir.close();
  Serial.printf("SD카드 %.1fGB 중 %.1fGB 사용\n", SD.totalBytes() / 1e9, SD.usedBytes() / 1e9);
  return true;
}

// =====================================================================
//  네트워크 (와이파이, 시간, MQTT, 업로드)
// =====================================================================
WiFiMulti wifiMulti;
WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);

struct Upload {
  bool on = false;
  bool fromFile = false;
  char sid[33] = "";
  uint32_t cursor = 0, stopSeq = 0;
  int64_t lastSentMs = 0;
  bool endSent = false;
  uint32_t endSentAt = 0, downSince = 0, lastSave = 0, gaps = 0, sentItems = 0, startedAt = 0;
  File f;
};
Upload up;
const char* abortWhy = "";            // 업로드를 포기한 이유 (시리얼에 출력)
WebSocketsClient* ws = nullptr;
volatile bool wsConnected = false, wsAck = false;
uint8_t* sendBuf = nullptr;

void wsEvent(WStype_t type, uint8_t* payload, size_t len) {
  if (type == WStype_CONNECTED) { wsConnected = true; Serial.printf("[ws] 연결됨 (%lu초)\n", millis() / 1000); }
  else if (type == WStype_DISCONNECTED) { wsConnected = false; Serial.printf("[ws] 끊김 (%lu초)\n", millis() / 1000); }
  else if (type == WStype_ERROR) { Serial.println("[ws] 오류"); }
  else if (type == WStype_TEXT) {
    Serial.print("[ws] 수신: ");
    Serial.write(payload, len > 80 ? 80 : len);
    Serial.println();
    if (len && strstr((const char*)payload, "\"ok\"")) wsAck = true;
  }
}

void wsOpen(const char* sid) {
  if (ws) { ws->disconnect(); delete ws; }
  ws = new WebSocketsClient();
  String path = "/ws/upload/" + camId + "?session=" + sid;
  ws->begin(SERVER_HOST, SERVER_PORT, path);
  ws->onEvent(wsEvent);
  ws->setReconnectInterval(1500);
  ws->enableHeartbeat(5000, 3000, 2);
  wsConnected = wsAck = false;
}

void wsClose() {
  if (ws) { ws->disconnect(); delete ws; ws = nullptr; }
  wsConnected = false;
  liveUp = false;
}

bool sendItem(uint8_t kind, int64_t epochMs, uint32_t len) {
  sendBuf[0] = kind;
  memcpy(sendBuf + 1, &epochMs, 8);
  bool ok = ws->sendBIN(sendBuf, 9 + len);
  if (ok) up.sentItems++;
  return ok;
}

void saveSent(const char* sid, int64_t ms) {
  if (!sdOk || !sdLock(500)) return;
  writeText(evPath(sid, ".sent"), String((long long)ms));
  sdUnlock();
}

// 업로드가 끝난 녹화는 SD에서 지운다 (아직 기록 중이면 .ok 표시만)
void markUploaded(const char* sid) {
  if (!sdOk || !sdLock()) return;
  if (SD.exists(evPath(sid, ".open"))) {
    writeText(evPath(sid, ".ok"), "1");
  } else {
    SD.remove(evPath(sid, ".bin"));
    SD.remove(evPath(sid, ".sent"));
  }
  countPending();
  sdUnlock();
}

void finishUpload(bool success) {
  if (success) {
    Serial.printf("업로드 완료 %s%s (힙 %u)\n", up.sid, up.gaps ? " (일부 프레임 누락)" : "", ESP.getFreeHeap());
    markUploaded(up.sid);
  } else {
    Serial.printf("업로드 중단 %s (%s, 이유: %s, 보낸 항목 %u개, %lu초 만에, 신호 %d dBm, 힙 %u), SD에 보관 후 나중에 다시 보냅니다\n",
                  up.sid, up.fromFile ? "SD재전송" : "실시간", abortWhy, up.sentItems,
                  (millis() - up.startedAt) / 1000, WiFi.RSSI(), ESP.getFreeHeap());
    saveSent(up.sid, up.lastSentMs);
  }
  if (up.f) { if (sdLock()) { up.f.close(); sdUnlock(); } }
  wsClose();
  up.on = false;
  lagging = false;
}

void startLive(const RecState& r) {
  up = Upload();
  up.on = true;
  strncpy(up.sid, r.session, sizeof(up.sid));
  up.cursor = max(r.startSeq, ring.oldest());
  up.stopSeq = r.stopSeq;
  up.startedAt = millis();
  wsOpen(up.sid);
  Serial.printf("서버 업로드 시작 %s\n", up.sid);
}

// SD에 남은 녹화를 찾아 전송 시작
void startSync() {
  if (!sdOk || !sdLock()) return;
  String found = "";
  File dir = SD.open("/event");
  for (File f = dir.openNextFile(); f && found == ""; f = dir.openNextFile()) {
    String name = f.name();
    f.close();
    if (!name.endsWith(".bin")) continue;
    String sid = name.substring(0, name.length() - 4);
    if (!SD.exists(evPath(sid.c_str(), ".open"))) found = sid;
  }
  dir.close();
  if (found != "") {
    up = Upload();
    up.on = true;
    up.fromFile = true;
    strncpy(up.sid, found.c_str(), sizeof(up.sid));
    String sent = readText(evPath(up.sid, ".sent"));
    up.lastSentMs = sent.length() ? atoll(sent.c_str()) : 0;
    up.f = SD.open(evPath(up.sid, ".bin"), FILE_READ);
    up.startedAt = millis();
  }
  sdUnlock();
  if (up.on) {
    if (!up.f) { up.on = false; return; }
    wsOpen(up.sid);
    Serial.printf("미전송 녹화 전송 시작 %s\n", up.sid);
  }
}

void pumpUpload() {
  if (!up.on) return;
  ws->loop();

  // 끝 신호에 대한 확인 응답을 받았으면, 서버가 곧바로 연결을 닫았더라도 성공이다.
  // 여기서 바로 정리해야 웹소켓 라이브러리가 자동으로 다시 접속하지 않는다 (서버에 유령 녹화가 생기는 원인)
  if (up.endSent && wsAck) { finishUpload(true); return; }

  if (!wsConnected) {
    liveUp = false;
    if (!up.downSince) up.downSince = millis();
    // 라이브 업로드가 6초 이상 끊기면 포기하고 SD 보관분을 나중에 보냄
    if (millis() - up.downSince > (up.fromFile ? 15000 : 6000)) { abortWhy = "서버와 연결 안 됨"; finishUpload(false); }
    return;
  }
  up.downSince = 0;
  liveUp = !up.fromFile;

  if (up.endSent) {
    if (wsAck) finishUpload(true);
    else if (millis() - up.endSentAt > 10000) { abortWhy = "다 보냈는데 서버 응답 없음"; finishUpload(false); }
    return;
  }

  uint32_t t0 = millis();
  while (millis() - t0 < 40) {                   // 한 번에 최대 40ms 동안 전송
    if (!up.fromFile) {
      RecState r = getRec();
      if (strcmp(r.session, up.sid) == 0 && r.stopSeq) up.stopSeq = r.stopSeq;
      if (up.cursor < ring.oldest()) { up.gaps += ring.oldest() - up.cursor; up.cursor = ring.oldest(); }
      if (up.stopSeq && up.cursor >= up.stopSeq) break;
      if (up.cursor >= ring.nextSeq()) break;
      uint8_t kind; int64_t ts; uint32_t len;
      if (ring.copy(up.cursor, kind, ts, sendBuf + 9, BUF_CAP, len)) {
        int64_t ms = toEpochMs(ts);
        if (!sendItem(kind, ms, len)) break;
        up.lastSentMs = ms;
      }
      up.cursor++;
      lagging = ring.nextSeq() - up.cursor > (uint32_t)(FPS * 2 * 3);   // 3초 이상 밀림
    } else {
      if (!sdLock(200)) break;
      uint8_t h[13];
      bool eof = up.f.read(h, 13) != 13;
      uint32_t len = 0;
      int64_t ms = 0;
      if (!eof) {
        memcpy(&ms, h + 1, 8);
        memcpy(&len, h + 9, 4);
        eof = len > BUF_CAP || up.f.read(sendBuf + 9, len) != (int)len;
      }
      sdUnlock();
      if (eof) { up.stopSeq = 1; break; }
      if (ms <= up.lastSentMs) continue;          // 서버가 이미 받은 부분은 건너뜀
      if (!sendItem(h[0], ms, len)) break;
      up.lastSentMs = ms;
    }
  }

  bool done = up.fromFile ? up.stopSeq == 1 : (up.stopSeq && up.cursor >= up.stopSeq);
  if (done) {
    ws->sendTXT("{\"cmd\":\"end\"}");
    up.endSent = true;
    up.endSentAt = millis();
  } else if (millis() - up.lastSave > 3000 && !up.fromFile) {
    saveSent(up.sid, up.lastSentMs);
    up.lastSave = millis();
  }
}

void publishStatus() {
  RecState r = getRec();
  const char* mode = r.active ? (liveUp ? "recording" : "offline_saving") : usbPower ? "charging" : "buffering";
  char bat[8];
  if (batteryPct < 0) strcpy(bat, "null"); else snprintf(bat, sizeof(bat), "%d", batteryPct);
  char msg[256];
  snprintf(msg, sizeof(msg),
           "{\"battery\":%s,\"charging\":%s,\"rssi\":%d,\"mode\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\","
           "\"pending_upload\":%s,\"sd\":%s,\"volt\":%.2f}",
           bat, usbPower ? "true" : "false", WiFi.RSSI(), mode, FW_VERSION,
           WiFi.localIP().toString().c_str(), (pendingCount > 0 && !r.active) ? "true" : "false",
           sdOk ? "true" : "false", batteryV);
  mqtt.publish(("bodycam/" + camId + "/status").c_str(), msg);
}

void publishEvent(const char* type, const RecState& r) {
  char msg[128];
  snprintf(msg, sizeof(msg), "{\"type\":\"%s\",\"session\":\"%s\",\"ts\":%lld}", type, r.session,
           (long long)(strcmp(type, "rec_start") == 0 ? r.startEpochMs : toEpochMs(esp_timer_get_time())));
  mqtt.publish(("bodycam/" + camId + "/event").c_str(), msg);
}

void syncTime() {
  if (time(nullptr) < 1700000000) return;       // 아직 시간 못 받음
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  int64_t nowMs = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  epochOffsetMs = nowMs - esp_timer_get_time() / 1000;
  if (!timeSynced) Serial.println("시간 동기화 완료");
  timeSynced = true;
}

void netTask(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                         // 전송 지연을 줄이기 위해 절전 끔
  wifiMulti.addAP(WIFI_SSID, WIFI_PASS);
  configTime(9 * 3600, 0, NTP_SERVER);
  mqtt.setServer(SERVER_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  sendBuf = (uint8_t*)ps_malloc(BUF_CAP + 9);

  uint32_t lastWifiTry = 0, lastMqttTry = 0, lastStatus = 0, lastTime = 0, lastSyncTry = 0;
  for (;;) {
    // 와이파이
    if (WiFi.status() != WL_CONNECTED) {
      wifiUp = mqttUp = false;
      if (millis() - lastWifiTry > 3000) {
        lastWifiTry = millis();
        wifiMulti.run(2000);
        if (WiFi.status() == WL_CONNECTED) Serial.println("와이파이 연결 " + WiFi.localIP().toString());
      }
    } else {
      wifiUp = true;
    }

    if (wifiUp && (lastTime == 0 || millis() - lastTime > (timeSynced ? 60000u : 2000u))) {
      syncTime();
      lastTime = millis();
    }

    // MQTT
    if (wifiUp && !mqtt.connected() && millis() - lastMqttTry > 3000) {
      lastMqttTry = millis();
      String will = "bodycam/" + camId + "/status";
      if (mqtt.connect(camId.c_str(), will.c_str(), 1, false, "{\"online\":false}")) {
        Serial.println("MQTT 연결됨");
        lastStatus = 0;
      }
    }
    mqttUp = mqtt.connected();
    if (mqttUp) {
      mqtt.loop();
      RecState r = getRec();
      if (evStart) { publishEvent("rec_start", r); evStart = false; }
      if (evStop && !evStart) { publishEvent("rec_stop", r); evStop = false; }
      if (millis() - lastStatus > 5000) { publishStatus(); lastStatus = millis(); }
    }

    // 업로드
    RecState r = getRec();
    // 사전녹화 시작점이 아직 링버퍼에 있을 때만 라이브 전송 (없으면 녹화 후 SD에서 전송)
    if (!up.on && wifiUp && r.active && strcmp(r.session, up.sid) != 0 && r.startSeq >= ring.oldest()) startLive(r);
    if (!up.on && wifiUp && !r.active && pendingCount > 0 && millis() - lastSyncTry > 10000) {
      lastSyncTry = millis();
      startSync();
    }
    // 녹화가 새로 시작되면 진행 중인 과거분 전송은 잠시 멈추고 라이브 우선
    if (up.on && up.fromFile && r.active) { abortWhy = "새 녹화 시작"; finishUpload(false); }
    pumpUpload();

    vTaskDelay(pdMS_TO_TICKS(up.on ? 1 : 10));
  }
}

// =====================================================================
//  시리얼 명령: id cam03 / info
// =====================================================================
void serialCommands() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("id ")) {
    String id = line.substring(3);
    id.trim();
    id.toLowerCase();
    prefs.putString("id", id);
    Serial.println("바디캠 번호를 " + id + "(으)로 저장했습니다. 재시작합니다");
    delay(300);
    ESP.restart();
  } else if (line == "rec") {                   // 버튼 없이 녹화 시험 (버튼을 누른 것과 같음)
    if (!camOk) { Serial.println("카메라가 없어 녹화할 수 없습니다"); return; }
    getRec().active ? stopRecord() : startRecord();
  } else if (line == "info") {
    RecState r = getRec();
    Serial.printf("id=%s fw=%s cam=%d mic=%d sd=%d wifi=%d mqtt=%d time=%d\n", camId.c_str(), FW_VERSION, camOk,
                  micOk, sdOk, wifiUp, mqttUp, timeSynced);
    Serial.printf("frames=%u ring=%u..%u rec=%d live=%d pending=%d battery=%d%% (%.2fV) usb=%d psram free=%u rssi=%d dBm heap=%u min=%u\n",
                  frameCount, ring.oldest(), ring.nextSeq(), r.active, liveUp, pendingCount, batteryPct, batteryV,
                  usbPower, ESP.getFreePsram(), WiFi.RSSI(), ESP.getFreeHeap(), ESP.getMinFreeHeap());
  } else if (line.length()) {
    Serial.println("명령: id cam03 (번호 저장) / info (상태 보기) / rec (녹화 시작·종료)");
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  esp_log_level_set("cam_hal", ESP_LOG_NONE);   // "FB-OVF" 경고 숨김 (촬영에는 영향 없음)
  {
    esp_reset_reason_t rr = esp_reset_reason();   // 왜 재시작됐는지 알려줌
    const char* why = rr == ESP_RST_POWERON ? "전원을 켬" : rr == ESP_RST_SW ? "소프트웨어 재시작(ESP.restart)" :
                      rr == ESP_RST_PANIC ? "프로그램 오류(패닉)" : rr == ESP_RST_INT_WDT ? "인터럽트 워치독" :
                      rr == ESP_RST_TASK_WDT ? "작업 워치독(작업이 멈춤)" : rr == ESP_RST_WDT ? "워치독" :
                      rr == ESP_RST_BROWNOUT ? "전압 부족" : rr == ESP_RST_EXT ? "리셋 버튼" : "기타";
    Serial.printf("\n[재시작 이유] %s (코드 %d)\n", why, (int)rr);
  }
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_USB_SENSE, INPUT_PULLDOWN);
  if (PIN_VIBE >= 0) { pinMode(PIN_VIBE, OUTPUT); digitalWrite(PIN_VIBE, LOW); }
  analogReadResolution(12);
  led(255, 255, 255);                           // 부팅 중: 흰색

  prefs.begin("bodycam", false);
  camId = prefs.getString("id", DEFAULT_CAM_ID);
  Serial.printf("\n바디캠 %s 펌웨어 %s\n", camId.c_str(), FW_VERSION);

  if (!psramFound()) Serial.println("PSRAM 없음! 보드 설정에서 OPI PSRAM을 켜세요");
  sdMutex = xSemaphoreCreateMutex();
  size_t ringItems = RING_SEC * (FPS + 10) + 20;
  if (!ring.begin(ringItems)) Serial.println("링버퍼 메모리 할당 실패");

  camOk = initCamera();
  i2s.setPinsPdmRx(MIC_CLK_PIN, MIC_DATA_PIN);
  micOk = i2s.begin(I2S_MODE_PDM_RX, AUDIO_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  if (!micOk) Serial.println("마이크 초기화 실패 - 영상만 녹화합니다");
  sdOk = initSD();

  if (camOk) xTaskCreatePinnedToCore(camTask, "cam", 4096, nullptr, 5, nullptr, 1);
  if (micOk) xTaskCreatePinnedToCore(audioTask, "mic", 4096, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(sdTask, "sd", 6144, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 4, nullptr, 0);
  vibe(1);
}

void loop() {
  checkButton();
  readPower();
  updateLed();
  updateVibe();
  serialCommands();
  delay(10);
}
