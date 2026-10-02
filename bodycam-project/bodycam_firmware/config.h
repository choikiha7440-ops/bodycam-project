#pragma once
// =====================================================================
//  바디캠 펌웨어 설정 (필요한 값만 바꾸세요)
// =====================================================================

#define FW_VERSION      "0.2.5"

// 바디캠 번호. 같은 펌웨어를 10대에 올린 뒤, 시리얼 모니터에서
// "id cam03" 처럼 입력하면 기기마다 번호가 저장됩니다.
#define DEFAULT_CAM_ID  "cam01"

// ---- 네트워크 (라떼판다 BODYCAM 핫스팟) ----
#define WIFI_SSID       "BODYCAM"
#define WIFI_PASS       "bodycam1234"
#define SERVER_HOST     "10.42.0.1"
#define SERVER_PORT     80          // nginx
#define MQTT_PORT       1883
#define NTP_SERVER      "10.42.0.1"

// ---- 영상 / 음성 ----
#define FPS             10          // 초당 프레임
#define PRE_SEC         10          // 버튼 누르기 전 몇 초부터 보낼지
#define RING_SEC        16          // 메모리 링버퍼 길이 (PRE_SEC보다 커야 함)
#define CAM_FRAME_SIZE  FRAMESIZE_VGA   // 640x480
#define JPEG_QUALITY    12          // 낮을수록 고화질, 용량 증가 (10~20 권장)
#define AUDIO_RATE      16000
#define AUDIO_GAIN      4           // 내장 마이크 소리가 작아서 디지털 증폭

// ---- SD카드 블랙박스 (평상시 자체녹화, 덮어쓰기) ----
#define LOOP_SEG_SEC    60          // 파일 하나의 길이 (초)
#define LOOP_KEEP_MIN   60          // 보관할 분량 (분). 넘으면 오래된 것부터 삭제

// ---- 핀 배치 (XIAO ESP32S3 Sense) ----
#define PIN_LED         1           // D0  WS2812B 데이터
#define PIN_BUTTON      2           // D1  녹화 스위치 (다른 쪽 다리는 GND)
#define PIN_BAT_ADC     3           // D2  배터리 전압 (100k/100k 분압)
#define PIN_USB_SENSE   4           // D3  USB 5V 감지 (100k/100k 분압)
#define PIN_VIBE        5           // D4  진동모터 (트랜지스터 경유). 안 쓰면 -1
#define SD_CS_PIN       21          // Sense 보드 내장 SD 슬롯

#define LED_BRIGHT      24          // 0~255. 배터리 절약을 위해 낮게
#define BAT_DIVIDER     2.0f        // 분압비 (100k/100k = 2)
#define LOW_BATTERY     15          // % 이하이면 배터리 부족 표시

// ---- 녹화 상태 (여러 작업이 함께 쓰는 구조체) ----
// 아두이노는 컴파일 전에 함수 선언을 파일 맨 위에 자동으로 만들기 때문에,
// 구조체는 .ino 가 아니라 여기(헤더)에 정의해야 합니다.
#include <stdint.h>
struct RecState {
  bool active = false;
  char session[33] = "";
  uint32_t startSeq = 0;
  uint32_t stopSeq = 0;      // 0이면 녹화 진행 중
  int64_t startEpochMs = 0;
};
