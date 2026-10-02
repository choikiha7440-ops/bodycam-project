"""바디캠 서버

- MQTT(bodycam/<cam>/status, bodycam/<cam>/event)로 바디캠 상태와 버튼 이벤트를 받는다.
- WebSocket(/ws/upload/<cam>?session=<id>)으로 JPEG 프레임과 PCM 음성을 받는다.
- 업로드가 끝나면 MP4(H.264 + AAC)로 변환해 recordings/<cam>/<날짜>/ 에 저장한다.
- 대시보드(/)에 상태, 라이브 화면, 녹화 목록을 실시간으로 보여준다.

업로드 바이너리 메시지 형식 (리틀 엔디언)
    [1바이트 종류][8바이트 타임스탬프(ms, epoch)][데이터]
    종류 1 = JPEG 프레임, 종류 2 = PCM 16kHz 16bit mono
업로드 종료: 텍스트 메시지 {"cmd": "end"}
"""

import asyncio
import json
import logging
import os
import re
import shutil
import struct
import subprocess
import time
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

# ---------------------------------------------------------------- 설정
BASE_DIR = Path(__file__).resolve().parent.parent
REC_DIR = Path(os.environ.get("BODYCAM_REC_DIR", BASE_DIR / "recordings"))
RAW_DIR = BASE_DIR / "raw"          # 변환 전 임시 파일 (웹에 공개되지 않음)
DATA_DIR = BASE_DIR / "data"
STATIC_DIR = Path(__file__).resolve().parent / "static"

MQTT_HOST = os.environ.get("BODYCAM_MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("BODYCAM_MQTT_PORT", "1883"))
CAM_COUNT = int(os.environ.get("BODYCAM_CAM_COUNT", "10"))

OFFLINE_AFTER = 15      # 초. 이 시간 동안 소식이 없으면 연결 끊김으로 표시
RESUME_WAIT = 300       # 초. 업로드가 끊긴 뒤 재접속(또는 SD 재전송)을 기다리는 시간
DISK_LIMIT = 0.85       # 디스크 사용률이 이 값을 넘으면 오래된 날짜 폴더부터 삭제
VAAPI_DEVICE = "/dev/dri/renderD128"
AUDIO_RATE = 16000

FRAME_JPEG = 1
FRAME_PCM = 2
HEADER = struct.Struct("<BQ")

# 이 값(2020-01-01)보다 작은 시각은 바디캠이 아직 시간을 못 받아서 보낸 "부팅 후 경과 ms"다.
# 그런 프레임은 서버 시계로 날짜를 다시 매긴다 (1970-01-01 폴더에 저장되던 문제)
TIME_VALID_MS = 1_577_836_800_000

ID_RE = re.compile(r"^[a-z0-9_-]{1,32}$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
FILE_RE = re.compile(r"^[a-z0-9_-]{1,64}$")

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("bodycam")

for d in (REC_DIR, RAW_DIR, DATA_DIR):
    d.mkdir(parents=True, exist_ok=True)


def valid_id(value: str) -> bool:
    return bool(ID_RE.match(value or ""))


# ---------------------------------------------------------------- 바디캠 목록
class CamRegistry:
    def __init__(self):
        self.path = DATA_DIR / "cams.json"
        self.cams: dict[str, dict] = {}
        names = {}
        if self.path.exists():
            try:
                names = json.loads(self.path.read_text("utf-8"))
            except Exception:
                log.warning("cams.json을 읽지 못해 기본 이름을 사용합니다")
        for i in range(1, CAM_COUNT + 1):
            names.setdefault(f"cam{i:02d}", f"바디캠 {i}")
        for cid, name in names.items():
            self.cams[cid] = self._blank(cid, name)
        self.save()

    @staticmethod
    def _blank(cid, name):
        return {
            "id": cid, "name": name, "online": False, "mode": "standby",
            "battery": None, "charging": False, "rssi": None, "fw": None, "ip": None,
            "last_seen": None, "recording": False, "rec_started": None,
            "uploading": False, "pending_upload": False,
        }

    def save(self):
        data = {cid: c["name"] for cid, c in sorted(self.cams.items())}
        self.path.write_text(json.dumps(data, ensure_ascii=False, indent=2), "utf-8")

    def get(self, cid):
        if cid not in self.cams:
            self.cams[cid] = self._blank(cid, cid)
            self.save()
        return self.cams[cid]

    @staticmethod
    def public(c):
        if c["recording"] or c["uploading"]:
            state = "recording"
        elif not c["online"]:
            state = "offline"
        elif c["pending_upload"]:
            state = "sync"
        elif c["charging"]:
            state = "charging"
        else:
            state = "standby"
        return {**c, "state": state}

    def all_public(self):
        return [self.public(c) for _, c in sorted(self.cams.items())]


reg = CamRegistry()


# ---------------------------------------------------------------- 대시보드 브로드캐스트
class Hub:
    def __init__(self):
        self.clients: set[WebSocket] = set()

    async def send(self, msg: dict):
        data = json.dumps(msg, ensure_ascii=False)
        for ws in list(self.clients):
            try:
                await ws.send_text(data)
            except Exception:
                self.clients.discard(ws)

    async def cam(self, cid):
        await self.send({"type": "cam", "cam": reg.public(reg.get(cid))})


hub = Hub()


# ---------------------------------------------------------------- 라이브 화면
class Live:
    def __init__(self):
        self.frames: dict[str, tuple[float, bytes]] = {}

    def put(self, cid, jpg):
        self.frames[cid] = (time.time(), jpg)

    def clear(self, cid):
        self.frames.pop(cid, None)


live = Live()


# ---------------------------------------------------------------- 녹화 세션
class Session:
    """업로드 한 건. 연결이 끊겨도 같은 session으로 다시 붙으면 이어서 기록한다."""

    def __init__(self, cid, sid):
        self.cid, self.sid = cid, sid
        self.dir = RAW_DIR / f"{cid}_{sid}"
        self.dir.mkdir(parents=True, exist_ok=True)
        self.meta_path = self.dir / "meta.json"
        meta = {}
        if self.meta_path.exists():
            try:
                meta = json.loads(self.meta_path.read_text())
            except Exception:
                pass
        self.frames = meta.get("frames", 0)
        self.first_ts = meta.get("first_ts")
        self.last_ts = meta.get("last_ts")
        self.audio_bytes = meta.get("audio_bytes", 0)
        self.last_audio_ts = meta.get("last_audio_ts")
        self.boot_first = meta.get("boot_first")    # 시간 동기화 전 프레임의 시각 (부팅 후 ms)
        self.boot_last = meta.get("boot_last")
        self.clock_fix = meta.get("clock_fix")      # 부팅 후 ms -> 실제 시각으로 바꿀 때 더할 값
        self.vf = open(self.dir / "video.mjpeg", "ab")
        self.af = open(self.dir / "audio.pcm", "ab")
        self.finalize_task: asyncio.Task | None = None
        self.conns = 0              # 이 녹화에 지금 붙어 있는 연결 수
        self.closed = False         # 저장이 시작되어 더 이상 쓰면 안 되는 상태

    def add(self, kind, ts, payload):
        if self.closed:
            return False
        if kind == FRAME_JPEG:
            if payload[:2] != b"\xff\xd8":
                return False
            if ts < TIME_VALID_MS:
                if self.boot_last is not None and ts <= self.boot_last:
                    return False      # 재전송으로 이미 받은 프레임
                # 도착 시각과의 차이 중 가장 작은 값 = 실시간으로 막 찍혀 온 프레임 기준의 보정값
                lag = int(time.time() * 1000) - ts
                self.clock_fix = lag if self.clock_fix is None else min(self.clock_fix, lag)
                self.boot_first = ts if self.boot_first is None else min(self.boot_first, ts)
                self.boot_last = ts if self.boot_last is None else max(self.boot_last, ts)
            else:
                if self.last_ts is not None and ts <= self.last_ts:
                    return False      # 재전송으로 이미 받은 프레임
                self.first_ts = ts if self.first_ts is None else min(self.first_ts, ts)
                self.last_ts = ts if self.last_ts is None else max(self.last_ts, ts)

            self.vf.write(payload)
            self.frames += 1
            return True
        if kind == FRAME_PCM:
            if self.last_audio_ts is not None and ts <= self.last_audio_ts:
                return False
            self.last_audio_ts = ts
            self.af.write(payload)
            self.audio_bytes += len(payload)
        return False

    def time_range(self):
        """실제 시각 기준 (처음, 끝) ms. 시간 동기화 전 프레임은 서버 시계로 보정한 값"""
        firsts, lasts = [], []
        if self.first_ts is not None:
            firsts.append(self.first_ts)
            lasts.append(self.last_ts)
        if self.boot_first is not None and self.clock_fix is not None:
            firsts.append(self.boot_first + self.clock_fix)
            lasts.append(self.boot_last + self.clock_fix)
        if not firsts:
            return None, None
        return min(firsts), max(lasts)

    def flush(self):
        if self.closed:
            return
        try:
            self.vf.flush()
            self.af.flush()
            self.meta_path.write_text(json.dumps({
                "cam": self.cid, "session": self.sid, "frames": self.frames,
                "first_ts": self.first_ts, "last_ts": self.last_ts,
                "audio_bytes": self.audio_bytes, "last_audio_ts": self.last_audio_ts,
                "boot_first": self.boot_first, "boot_last": self.boot_last, "clock_fix": self.clock_fix,
            }))
        except (OSError, ValueError) as e:       # 폴더가 지워졌거나 파일이 닫힌 경우
            log.warning("%s 임시 정보 저장 실패(무시): %s", self.dir.name, e)

    def close(self):
        if self.closed:
            return
        self.flush()
        self.closed = True
        for f in (self.vf, self.af):
            try:
                f.close()
            except OSError:
                pass


sessions: dict[tuple[str, str], Session] = {}
transcode_lock = asyncio.Lock()
vaapi_ok: bool | None = None


def run_ffmpeg(s: Session, fps: float, out: Path) -> str | None:
    """MJPEG + PCM을 MP4로 변환. 인텔 하드웨어 인코더를 먼저 쓰고, 안 되면 CPU로 변환."""
    global vaapi_ok
    video = s.dir / "video.mjpeg"
    audio = s.dir / "audio.pcm"
    has_audio = s.audio_bytes > AUDIO_RATE * 2 * 0.5  # 0.5초 이상일 때만

    inputs = ["-f", "mjpeg", "-framerate", f"{fps:.3f}", "-i", str(video)]
    if has_audio:
        inputs += ["-f", "s16le", "-ar", str(AUDIO_RATE), "-ac", "1", "-i", str(audio)]
    audio_args = ["-c:a", "aac", "-b:a", "64k", "-shortest"] if has_audio else ["-an"]
    tail = ["-movflags", "+faststart", str(out)]

    attempts = []
    if vaapi_ok is not False and Path(VAAPI_DEVICE).exists():
        attempts.append(("vaapi", ["-vaapi_device", VAAPI_DEVICE] + inputs + [
            "-vf", "format=nv12,hwupload", "-c:v", "h264_vaapi", "-low_power", "1", "-qp", "26",
        ]))
    attempts.append(("cpu", inputs + [
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "26", "-pix_fmt", "yuv420p",
    ]))

    for name, args in attempts:
        cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error"] + args + audio_args + tail
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0 and out.exists() and out.stat().st_size > 0:
            if name == "vaapi":
                vaapi_ok = True
            return name
        log.warning("%s 변환 실패: %s", name, result.stderr.strip()[-400:])
        if name == "vaapi":
            vaapi_ok = False
    return None


def build_clip(s: Session) -> dict | None:
    first_ts, last_ts = s.time_range()
    if s.frames < 2 or first_ts is None:
        log.info("%s 프레임이 부족해 버립니다 (%d장)", s.dir.name, s.frames)
        shutil.rmtree(s.dir, ignore_errors=True)
        return None

    if s.boot_first is not None:
        log.warning("%s 바디캠이 시간을 받기 전에 녹화함. 서버 시계로 날짜를 매깁니다", s.dir.name)
    span = max((last_ts - first_ts) / 1000, 0.1)
    fps = max(1.0, min(30.0, (s.frames - 1) / span))
    start = datetime.fromtimestamp(first_ts / 1000)
    day = start.strftime("%Y-%m-%d")
    out_dir = REC_DIR / s.cid / day
    out_dir.mkdir(parents=True, exist_ok=True)

    base = f"{s.cid}_{start:%H%M%S}"
    n = 1
    while (out_dir / f"{base}.mp4").exists():
        n += 1
        base = f"{s.cid}_{start:%H%M%S}_{n}"
    mp4 = out_dir / f"{base}.mp4"
    thumb = out_dir / f"{base}.jpg"

    encoder = run_ffmpeg(s, fps, mp4)
    if not encoder:
        log.error("%s 변환 실패. 원본은 %s 에 남겨둡니다", base, s.dir)
        return None

    subprocess.run(
        ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-ss", str(min(1.0, s.frames / fps / 2)),
         "-i", str(mp4), "-frames:v", "1", "-vf", "scale=320:-2", str(thumb)],
        capture_output=True,
    )

    meta = {
        "cam": s.cid, "name": reg.get(s.cid)["name"], "date": day, "file": base,
        "video": f"/media/{s.cid}/{day}/{base}.mp4",
        "thumb": f"/media/{s.cid}/{day}/{base}.jpg" if thumb.exists() else None,
        "start": start.isoformat(timespec="seconds"),
        "duration": round(s.frames / fps, 1), "frames": s.frames, "fps": round(fps, 1),
        "size": mp4.stat().st_size, "audio": s.audio_bytes > 0,
        "session": s.sid, "encoder": encoder,
    }
    (out_dir / f"{base}.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), "utf-8")
    shutil.rmtree(s.dir, ignore_errors=True)
    log.info("저장 완료 %s (%.1f초, %s)", mp4, meta["duration"], encoder)
    return meta


async def finalize(s: Session):
    async with transcode_lock:
        meta = await asyncio.to_thread(build_clip, s)
    if meta:
        await hub.send({"type": "recording", "item": meta})


async def finalize_later(key):
    try:
        await asyncio.sleep(RESUME_WAIT)
    except asyncio.CancelledError:
        return
    s = sessions.get(key)
    if s and s.conns == 0 and not s.closed:
        sessions.pop(key, None)
        log.info("%s 재접속이 없어 지금까지 받은 분량으로 저장합니다", s.dir.name)
        s.close()
        await finalize(s)


def recover_raw():
    """서버가 재시작되기 전에 남아 있던 임시 녹화를 저장한다."""
    for d in RAW_DIR.iterdir():
        if d.is_dir() and (d / "meta.json").exists():
            try:
                meta = json.loads((d / "meta.json").read_text())
                s = Session(meta["cam"], meta["session"])
                s.close()
                asyncio.create_task(finalize(s))
            except Exception as e:
                log.warning("임시 녹화 복구 실패 %s: %s", d, e)


# ---------------------------------------------------------------- MQTT
LOOP: asyncio.AbstractEventLoop | None = None
mqtt_state = {"connected": False}


async def handle_mqtt(topic: str, payload: str):
    parts = topic.split("/")
    if len(parts) != 3 or not valid_id(parts[1]):
        return
    _, cid, kind = parts
    cam = reg.get(cid)
    try:
        data = json.loads(payload) if payload.strip().startswith("{") else {}
    except json.JSONDecodeError:
        data = {}

    if kind == "status":
        if data.get("online") is False or payload.strip() == "offline":
            cam["online"] = False
        else:
            cam["online"] = True
            cam["last_seen"] = time.time()
            for k in ("battery", "charging", "rssi", "fw", "ip", "mode", "pending_upload"):
                if k in data:
                    cam[k] = data[k]
            if data.get("mode") == "recording" and not cam["recording"]:
                cam["recording"] = True
                cam["rec_started"] = cam["rec_started"] or time.time()
            elif data.get("mode") in ("buffering", "charging") and cam["recording"] and not cam["uploading"]:
                cam["recording"] = False          # 바디캠이 대기 중이라고 보고하면 녹화 표시 해제
                cam["rec_started"] = None
    elif kind == "event":
        cam["online"] = True
        cam["last_seen"] = time.time()
        etype = data.get("type")
        if etype == "rec_start":
            cam["recording"] = True
            ts = data.get("ts") or 0
            if ts < TIME_VALID_MS:            # 바디캠이 아직 시간을 못 받음: 서버 시계 사용
                ts = time.time() * 1000
            cam["rec_started"] = ts / 1000
            await hub.send({"type": "alert", "cam": cid, "name": cam["name"], "at": time.time()})
        elif etype == "rec_stop":
            cam["recording"] = False
            cam["rec_started"] = None
    else:
        return
    await hub.cam(cid)


def mqtt_on_connect(client, userdata, flags, reason_code, properties=None):
    mqtt_state["connected"] = not reason_code.is_failure
    if mqtt_state["connected"]:
        client.subscribe([("bodycam/+/status", 1), ("bodycam/+/event", 1)])
        log.info("MQTT 연결됨 (%s:%d)", MQTT_HOST, MQTT_PORT)


def mqtt_on_disconnect(client, userdata, flags, reason_code, properties=None):
    mqtt_state["connected"] = False
    log.warning("MQTT 연결 끊김, 다시 시도합니다")


def mqtt_on_message(client, userdata, msg):
    try:
        payload = msg.payload.decode("utf-8", "replace")
    except Exception:
        return
    if LOOP:
        asyncio.run_coroutine_threadsafe(handle_mqtt(msg.topic, payload), LOOP)


mqttc = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="bodycam-server")
mqttc.on_connect = mqtt_on_connect
mqttc.on_disconnect = mqtt_on_disconnect
mqttc.on_message = mqtt_on_message
mqttc.reconnect_delay_set(1, 10)


# ---------------------------------------------------------------- 백그라운드 작업
def disk_info():
    u = shutil.disk_usage(REC_DIR)
    return {"total": u.total, "used": u.used, "ratio": round(u.used / u.total, 3)}


def health():
    return {"mqtt": mqtt_state["connected"], "disk": disk_info(),
            "encoder": {True: "vaapi", False: "cpu", None: "auto"}[vaapi_ok]}


async def watchdog():
    tick = 0
    while True:
        await asyncio.sleep(2)
        now = time.time()
        for cid, cam in reg.cams.items():
            if cam["uploading"] and not refresh_uploading(cid):     # 연결이 없는데 업로드 중으로 남은 경우 바로잡기
                cam["uploading"] = False
                live.clear(cid)
                await hub.cam(cid)
            if cam["online"] and not cam["uploading"] and cam["last_seen"] and now - cam["last_seen"] > OFFLINE_AFTER:
                cam["online"] = False
                await hub.cam(cid)
        tick += 1
        if tick % 15 == 0:
            await hub.send({"type": "health", "health": health()})
        if tick % 300 == 0:
            await asyncio.to_thread(cleanup_disk)


def cleanup_disk():
    """디스크가 차면 가장 오래된 날짜 폴더부터 지운다."""
    while disk_info()["ratio"] > DISK_LIMIT:
        days = sorted(
            (d for cam in REC_DIR.iterdir() if cam.is_dir() for d in cam.iterdir()
             if d.is_dir() and DATE_RE.match(d.name)),
            key=lambda d: d.name,
        )
        if not days:
            break
        log.warning("저장공간 확보를 위해 삭제: %s", days[0])
        shutil.rmtree(days[0], ignore_errors=True)


@asynccontextmanager
async def lifespan(app: FastAPI):
    global LOOP
    LOOP = asyncio.get_running_loop()
    mqttc.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    mqttc.loop_start()
    recover_raw()
    task = asyncio.create_task(watchdog())
    yield
    task.cancel()
    mqttc.loop_stop()
    mqttc.disconnect()
    for s in sessions.values():
        s.close()


app = FastAPI(title="Bodycam Server", lifespan=lifespan)
app.mount("/media", StaticFiles(directory=REC_DIR), name="media")
app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


# ---------------------------------------------------------------- HTTP API
@app.get("/")
async def index():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/api/health")
async def api_health():
    return health()


@app.get("/api/cams")
async def api_cams():
    return reg.all_public()


class Rename(BaseModel):
    name: str


@app.put("/api/cams/{cid}")
async def api_rename(cid: str, body: Rename):
    if not valid_id(cid) or cid not in reg.cams:
        raise HTTPException(404, "등록되지 않은 바디캠입니다")
    name = body.name.strip()[:30]
    if not name:
        raise HTTPException(400, "이름을 입력하세요")
    reg.cams[cid]["name"] = name
    reg.save()
    await hub.cam(cid)
    return reg.public(reg.cams[cid])


@app.get("/api/recordings")
async def api_recordings(cam: str = "", limit: int = 300):
    if cam and not valid_id(cam):
        raise HTTPException(400, "잘못된 바디캠 이름입니다")
    cams = [REC_DIR / cam] if cam else [d for d in REC_DIR.iterdir() if d.is_dir()]
    items = []
    for cdir in cams:
        if not cdir.is_dir():
            continue
        for meta in cdir.glob("*/*.json"):
            try:
                item = json.loads(meta.read_text("utf-8"))
                item["name"] = reg.get(item["cam"])["name"]
                items.append(item)
            except Exception:
                continue
    items.sort(key=lambda x: x.get("start", ""), reverse=True)
    return items[: max(1, min(limit, 2000))]


@app.delete("/api/recordings/{cid}/{day}/{name}")
async def api_delete(cid: str, day: str, name: str):
    if not (valid_id(cid) and DATE_RE.match(day) and FILE_RE.match(name)):
        raise HTTPException(400, "잘못된 경로입니다")
    folder = REC_DIR / cid / day
    removed = 0
    for ext in (".mp4", ".jpg", ".json"):
        f = folder / f"{name}{ext}"
        if f.exists():
            f.unlink()
            removed += 1
    if not removed:
        raise HTTPException(404, "녹화본을 찾을 수 없습니다")
    return {"ok": True}


@app.get("/api/live/{cid}.jpg")
async def api_live_jpg(cid: str):
    item = live.frames.get(cid)
    if not item:
        raise HTTPException(404, "라이브 화면이 없습니다")
    return Response(item[1], media_type="image/jpeg", headers={"Cache-Control": "no-store"})


@app.get("/api/live/{cid}.mjpg")
async def api_live_mjpg(cid: str):
    async def stream():
        last = 0.0
        idle = time.time()
        while time.time() - idle < 20:
            item = live.frames.get(cid)
            if item and item[0] != last:
                last, idle = item[0], time.time()
                jpg = item[1]
                yield (b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                       + str(len(jpg)).encode() + b"\r\n\r\n" + jpg + b"\r\n")
            await asyncio.sleep(0.05)

    return StreamingResponse(stream(), media_type="multipart/x-mixed-replace; boundary=frame",
                             headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"})


# ---------------------------------------------------------------- WebSocket
@app.websocket("/ws/dashboard")
async def ws_dashboard(ws: WebSocket):
    await ws.accept()
    hub.clients.add(ws)
    await ws.send_text(json.dumps({"type": "snapshot", "cams": reg.all_public(), "health": health(),
                                   "server_time": time.time()}, ensure_ascii=False))
    try:
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        hub.clients.discard(ws)


def refresh_uploading(cid):
    """이 바디캠에 붙어 있는 연결이 하나라도 있으면 업로드 중"""
    return any(v.conns > 0 and not v.closed and v.frames > 0 for (c, _), v in sessions.items() if c == cid)


@app.websocket("/ws/upload/{cid}")
async def ws_upload(ws: WebSocket, cid: str, session: str = ""):
    if not valid_id(cid) or not valid_id(session):
        await ws.close(code=1008)
        return
    await ws.accept()

    key = (cid, session)
    s = sessions.get(key)
    if s is None or s.closed:                 # 이미 저장된 녹화에 다시 붙으면 새 조각으로 시작
        s = sessions[key] = Session(cid, session)
        log.info("업로드 시작 %s / %s", cid, session)
    else:
        log.info("업로드 재개 %s / %s", cid, session)
    if s.finalize_task:
        s.finalize_task.cancel()
        s.finalize_task = None
    s.conns += 1

    cam = reg.get(cid)
    cam.update(online=True, last_seen=time.time())   # 접속만으로는 녹화 중으로 표시하지 않음 (유령 녹화 방지)
    announced = False

    ended = False
    count = 0
    try:
        while True:
            msg = await ws.receive()
            if msg["type"] == "websocket.disconnect":
                break
            data = msg.get("bytes")
            if data is not None:
                if len(data) <= HEADER.size:
                    continue
                kind, ts = HEADER.unpack_from(data)
                payload = data[HEADER.size:]
                if s.add(kind, ts, payload):
                    if not announced:               # 첫 프레임이 들어온 순간부터 녹화 중으로 표시
                        announced = True
                        cam["uploading"] = True
                        if not cam["recording"]:
                            cam["recording"] = True
                            cam["rec_started"] = time.time()
                        await hub.cam(cid)
                    live.put(cid, payload)
                    first_ts, _ = s.time_range()
                    if cam["rec_started"] and first_ts and first_ts / 1000 < cam["rec_started"]:
                        cam["rec_started"] = first_ts / 1000     # 사전녹화 10초 포함
                count += 1
                if count % 50 == 0:
                    s.flush()
                    cam["last_seen"] = time.time()
            elif msg.get("text"):
                try:
                    cmd = json.loads(msg["text"]).get("cmd")
                except (json.JSONDecodeError, AttributeError):
                    cmd = None
                if cmd == "end":
                    ended = True
                    await ws.send_text(json.dumps({"ok": True, "frames": s.frames}))
                    # 응답을 보낸 직후에 연결을 닫으면 바디캠의 웹소켓 라이브러리가 응답을 읽기 전에
                    # 연결 종료를 먼저 알아채서 응답을 버린다. 바디캠이 먼저 닫을 때까지 잠깐 기다린다.
                    try:
                        await asyncio.wait_for(ws.receive(), timeout=3)
                    except Exception:
                        pass
                    break
    except WebSocketDisconnect:
        pass
    except Exception as e:                    # 어떤 에러가 나도 아래 상태 정리는 반드시 실행
        log.warning("업로드 처리 중 오류 %s / %s: %s", cid, session, e)
    finally:
        try:
            s.conns -= 1
            if ended and not s.closed:
                s.flush()
                sessions.pop(key, None)
                s.close()
                asyncio.create_task(finalize(s))
                log.info("업로드 완료 %s / %s (%d장)", cid, session, s.frames)
            elif not ended and not s.closed:
                s.flush()
                if s.conns == 0:              # 다른 연결이 이어받지 않았을 때만 재접속 대기
                    log.warning("업로드 끊김 %s / %s, %d초 동안 재접속을 기다립니다", cid, session, RESUME_WAIT)
                    s.finalize_task = asyncio.create_task(finalize_later(key))
        except Exception as e:
            log.warning("업로드 마무리 중 오류 %s / %s: %s", cid, session, e)
        finally:
            cam["uploading"] = refresh_uploading(cid)
            if not cam["uploading"]:
                live.clear(cid)
            if ended and not cam["uploading"]:
                cam["recording"] = False
                cam["rec_started"] = None
            await hub.cam(cid)
