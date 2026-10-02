# 바디캠 펌웨어 0.2.5 (XIAO ESP32S3 Sense)

## 준비
1. 아두이노 IDE 보드 매니저에서 **esp32 by Espressif Systems 3.x** 설치
2. 라이브러리 매니저에서 **PubSubClient** (Nick O'Leary), **WebSockets** (Markus Sattler) 설치
3. 보드 설정
   - 보드: XIAO_ESP32S3
   - PSRAM: OPI PSRAM
   - Partition Scheme: Default with spiffs (3MB APP/1.5MB SPIFFS) (기본값)
   - USB CDC On Boot: Enabled
4. microSD 카드를 FAT32로 포맷해서 보드에 꽂기 (32GB 이하)

## 배선
| 부품 | XIAO 핀 | 비고 |
|---|---|---|
| WS2812B VDD / GND / DIN | 3V3 / GND / D0 | |
| 택트 스위치 | D1 ↔ GND | 저항 필요 없음 |
| 배터리 전압 | BAT+ → 100kΩ → D2 → 100kΩ → GND | 분압 |
| USB 감지 | 5V → 100kΩ → D3 → 100kΩ → GND | 분압 |
| 진동모터 (선택) | D4 → 1kΩ → NPN 트랜지스터 베이스 | 모터는 3V3와 컬렉터 사이, 다이오드 병렬 |
| LiPo 배터리 | 보드 뒷면 BAT+ / BAT- 패드 | 극성 확인 |

## 기기마다 번호 정하기
같은 펌웨어를 올린 뒤 시리얼 모니터(115200, 줄바꿈 "New Line")에서
```
id cam03
```
입력하면 저장되고 재시작합니다. `info` 는 현재 상태를 보여주고, `rec` 은 버튼을 누른 것처럼 녹화를 시작하거나 종료합니다 (버튼 연결 전 시험용).

## LED
| 색 | 의미 |
|---|---|
| 흰색 | 부팅 중 |
| 초록 천천히 깜빡임 | 평상시 자체녹화 |
| 빨강 켜짐 | 서버로 녹화 중 |
| 주황 깜빡임 | 녹화 중인데 와이파이 끊김 (SD에 보호 저장) |
| 청록 깜빡임 | SD에 남은 녹화를 서버로 보내는 중 |
| 파랑 숨쉬기 / 켜짐 | 충전 중 / 충전 완료 |
| 빨강 빠른 깜빡임 | 배터리 부족 |
| 빨강·흰색 교대 | 카메라 고장 |

진동: 짧게 1번 = 녹화 시작, 짧게 2번 = 녹화 종료, 길게 = 오류

## SD카드 폴더
- `/loop/` 평상시 1분 단위 파일, 60분 분량을 넘으면 오래된 것부터 삭제
- `/event/` 녹화 버튼 구간. 서버로 전송이 끝나면 자동 삭제
- PC에서 보려면 서버의 `tools/sd2mp4.py`로 MP4 변환
