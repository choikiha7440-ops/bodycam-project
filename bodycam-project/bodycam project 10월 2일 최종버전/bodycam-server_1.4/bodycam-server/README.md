# 바디캠 서버 (1단계)

라떼판다에서 돌아가는 바디캠 수신 서버와 대시보드, 그리고 보드 없이 테스트할 수 있는 가짜 바디캠 시뮬레이터입니다.

## 설치
```
cd ~/bodycam-server
./install.sh
```
설치가 끝나면 한 번 로그아웃했다가 다시 접속하세요 (하드웨어 인코더 권한 적용).

## 대시보드 주소
- 집이나 학원 네트워크의 PC: http://lattepanda.local
- BODYCAM 와이파이에 연결한 폰: http://10.42.0.1

## 시뮬레이터로 테스트
```
./venv/bin/python simulator/sim.py --cams 3             # 자동으로 가끔 버튼을 누름
./venv/bin/python simulator/sim.py --cams 3 --manual    # 1, 2, 3 입력 후 엔터로 버튼 누르기
```

## 자주 쓰는 명령
```
sudo systemctl status bodycam        # 서버 상태
journalctl -u bodycam -f             # 서버 로그 보기
sudo systemctl restart bodycam       # 서버 재시작
```

## 폴더
- recordings/<cam>/<날짜>/ : 녹화본(mp4), 썸네일(jpg), 정보(json)
- raw/ : 변환 전 임시 파일. 업로드가 끊기면 60초 기다렸다가 받은 분량만큼 저장
- data/cams.json : 바디캠 이름(착용자)

## 바디캠(ESP32)이 따라야 할 규칙
- MQTT bodycam/<cam>/status (5초마다)
  {"battery":87,"charging":false,"rssi":-55,"mode":"buffering","fw":"0.1"}
- MQTT bodycam/<cam>/event
  {"type":"rec_start","session":"...","ts":1790000000000} / {"type":"rec_stop",...}
- WebSocket ws://10.42.0.1/ws/upload/<cam>?session=<id>
  - 바이너리: [1바이트 종류][8바이트 ms 타임스탬프][데이터], 종류 1 = JPEG, 2 = PCM 16kHz 16bit mono
  - 끝낼 때 텍스트 {"cmd":"end"}
