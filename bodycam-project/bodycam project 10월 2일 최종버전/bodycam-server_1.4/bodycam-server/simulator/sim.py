"""가짜 바디캠 시뮬레이터

실제 ESP32와 똑같은 방식으로 서버에 상태와 영상을 보낸다.
- 평소: 최근 10초 분량을 메모리 링버퍼에 계속 덮어쓴다 (자체녹화)
- 버튼: 링버퍼 10초 + 실시간 영상을 서버로 업로드
- 5초마다 MQTT로 배터리, 신호, 상태를 보고

사용 예
    python simulator/sim.py --cams 3              # 3대, 자동으로 가끔 버튼을 누름
    python simulator/sim.py --cams 3 --manual     # 터미널에 1,2,3 입력으로 직접 버튼 누르기
"""

import argparse
import array
import asyncio
import collections
import colorsys
import io
import json
import math
import random
import struct
import sys
import time
from datetime import datetime

import paho.mqtt.client as mqtt
import websockets
from PIL import Image, ImageDraw, ImageFont

HEADER = struct.Struct("<BQ")
FRAME_JPEG, FRAME_PCM = 1, 2
AUDIO_RATE = 16000


def load_font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


class FakeCam:
    def __init__(self, idx, args):
        self.idx, self.args = idx, args
        self.cid = f"cam{idx:02d}"
        self.w, self.h = args.width, args.height
        self.ring = collections.deque(maxlen=args.fps * args.pre)   # (프레임, 음성) 묶음
        self.recording = False
        self.ws = None
        self.rec_until = 0.0
        self.sid = None
        self.battery = random.uniform(55, 100)
        self.charging = idx == args.charging
        self.phase = 0
        self.tone = 330 + idx * 55
        r, g, b = colorsys.hsv_to_rgb((idx * 0.13) % 1, 0.45, 0.55)
        self.bg = (int(r * 255), int(g * 255), int(b * 255))
        self.font_big = load_font(max(20, self.h // 7))
        self.font = load_font(max(12, self.h // 20))

        self.mqtt = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sim-{self.cid}")
        self.mqtt.will_set(f"bodycam/{self.cid}/status", json.dumps({"online": False}), qos=1)
        self.mqtt.connect_async(args.server, args.mqtt_port, keepalive=15)
        self.mqtt.loop_start()

    # ------------------------------------------------------------ 가짜 데이터
    def frame(self, ts):
        img = Image.new("RGB", (self.w, self.h), self.bg)
        d = ImageDraw.Draw(img)
        t = ts / 1000
        x = int((math.sin(t * 1.3 + self.idx) * 0.4 + 0.5) * self.w)
        d.ellipse([x - 30, self.h // 2 - 30, x + 30, self.h // 2 + 30], fill=(240, 240, 240))
        d.text((16, 12), self.cid.upper(), font=self.font_big, fill=(255, 255, 255))
        stamp = datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S.") + f"{int(ts % 1000):03d}"
        d.text((16, self.h - 20 - getattr(self.font, "size", 14)), stamp, font=self.font, fill=(255, 255, 255))
        if self.recording:
            d.ellipse([self.w - 40, 16, self.w - 16, 40], fill=(220, 30, 50))
        buf = io.BytesIO()
        img.save(buf, "JPEG", quality=self.args.quality)
        return buf.getvalue()

    def audio(self):
        n = AUDIO_RATE // self.args.fps
        samples = array.array("h", (
            int(2500 * math.sin(2 * math.pi * self.tone * (self.phase + i) / AUDIO_RATE)) for i in range(n)))
        self.phase += n
        if sys.byteorder != "little":
            samples.byteswap()
        return samples.tobytes()

    # ------------------------------------------------------------ MQTT
    def status(self):
        mode = "recording" if self.recording else "charging" if self.charging else "buffering"
        self.mqtt.publish(f"bodycam/{self.cid}/status", json.dumps({
            "battery": round(self.battery, 1), "charging": self.charging,
            "rssi": random.randint(-72, -45), "mode": mode, "fw": "sim-0.1", "ip": f"10.42.0.{100 + self.idx}",
        }), qos=0)

    def event(self, etype):
        self.mqtt.publish(f"bodycam/{self.cid}/event", json.dumps({
            "type": etype, "session": self.sid, "ts": int(time.time() * 1000)}), qos=1)

    # ------------------------------------------------------------ 녹화 버튼
    async def press(self):
        if self.recording:
            await self.stop()
        else:
            await self.start()

    async def start(self, length=None):
        self.sid = datetime.now().strftime("%Y%m%d-%H%M%S") + f"-{random.randint(0, 0xffff):04x}"
        url = f"ws://{self.args.server}:{self.args.http_port}/ws/upload/{self.cid}?session={self.sid}"
        self.event("rec_start")
        try:
            self.ws = await websockets.connect(url, max_size=None, ping_interval=10)
        except Exception as e:
            print(f"[{self.cid}] 서버 연결 실패: {e}")
            self.ws = None
            return
        self.recording = True
        self.rec_until = time.time() + (length or random.uniform(self.args.min_len, self.args.max_len))
        pre = list(self.ring)
        self.ring.clear()
        for jpg_msg, pcm_msg in pre:        # 버튼 누르기 전 10초 분량
            await self.ws.send(jpg_msg)
            await self.ws.send(pcm_msg)
        print(f"[{self.cid}] 녹화 시작 (사전녹화 {len(pre)}장 전송)")
        self.status()

    async def stop(self):
        if not self.ws:
            return
        try:
            await self.ws.send(json.dumps({"cmd": "end"}))
            ack = await asyncio.wait_for(self.ws.recv(), 5)
            print(f"[{self.cid}] 녹화 종료 {ack}")
        except Exception as e:
            print(f"[{self.cid}] 종료 중 오류: {e}")
        finally:
            await self.ws.close()
            self.ws = None
            self.recording = False
            self.event("rec_stop")
            self.status()

    # ------------------------------------------------------------ 메인 루프
    async def run(self):
        period = 1 / self.args.fps
        next_status = 0.0
        next_press = time.time() + random.uniform(self.args.min_gap, self.args.max_gap)
        while True:
            t0 = time.time()
            ts = int(t0 * 1000)
            jpg = HEADER.pack(FRAME_JPEG, ts) + await asyncio.to_thread(self.frame, ts)
            pcm = HEADER.pack(FRAME_PCM, ts) + self.audio()

            if self.recording and self.ws:
                try:
                    await self.ws.send(jpg)
                    await self.ws.send(pcm)
                except Exception as e:
                    print(f"[{self.cid}] 전송 끊김: {e}")
                    self.ws = None
                    self.recording = False
            else:
                self.ring.append((jpg, pcm))

            if t0 >= next_status:
                self.battery = min(100, self.battery + 0.05) if self.charging else max(3, self.battery - 0.02)
                self.status()
                next_status = t0 + 5

            if not self.args.manual:
                if not self.recording and t0 >= next_press:
                    await self.start()
                    next_press = t0 + random.uniform(self.args.min_gap, self.args.max_gap)
                elif self.recording and t0 >= self.rec_until:
                    await self.stop()
            await asyncio.sleep(max(0, period - (time.time() - t0)))


async def keyboard(cams):
    print("번호를 입력하고 엔터를 누르면 그 바디캠의 버튼을 누릅니다 (예: 1). q는 종료")
    while True:
        line = (await asyncio.to_thread(sys.stdin.readline)).strip()
        if line.lower() == "q":
            raise SystemExit
        if line.isdigit() and 1 <= int(line) <= len(cams):
            await cams[int(line) - 1].press()


async def main():
    p = argparse.ArgumentParser(description="가짜 바디캠 시뮬레이터")
    p.add_argument("--cams", type=int, default=3, help="바디캠 수")
    p.add_argument("--server", default="127.0.0.1", help="서버 주소")
    p.add_argument("--http-port", type=int, default=80, help="웹 포트 (nginx 80, 직접 접속 8000)")
    p.add_argument("--mqtt-port", type=int, default=1883)
    p.add_argument("--fps", type=int, default=10)
    p.add_argument("--pre", type=int, default=10, help="사전녹화 초")
    p.add_argument("--width", type=int, default=640)
    p.add_argument("--height", type=int, default=480)
    p.add_argument("--quality", type=int, default=70)
    p.add_argument("--manual", action="store_true", help="자동 버튼 누르기를 끄고 키보드로 조작")
    p.add_argument("--min-gap", type=float, default=30, help="자동 버튼 최소 간격(초)")
    p.add_argument("--max-gap", type=float, default=90)
    p.add_argument("--min-len", type=float, default=15, help="녹화 길이 최소(초)")
    p.add_argument("--max-len", type=float, default=40)
    p.add_argument("--charging", type=int, default=0, help="충전 중으로 보일 바디캠 번호 (0이면 없음)")
    args = p.parse_args()

    cams = [FakeCam(i, args) for i in range(1, args.cams + 1)]
    print(f"{args.cams}대 시작. 서버 {args.server} (웹 {args.http_port}, MQTT {args.mqtt_port})")
    tasks = [asyncio.create_task(c.run()) for c in cams]
    if args.manual:
        tasks.append(asyncio.create_task(keyboard(cams)))
    try:
        await asyncio.gather(*tasks)
    finally:
        for c in cams:
            c.mqtt.publish(f"bodycam/{c.cid}/status", json.dumps({"online": False}))
            c.mqtt.loop_stop()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except (KeyboardInterrupt, SystemExit):
        print("종료")
