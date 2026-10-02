"""바디캠 SD카드 파일(.bin)을 MP4로 변환

SD카드의 /loop/000123.bin (평상시 블랙박스) 이나 /event/<세션>.bin (녹화 버튼 구간)을
PC나 라떼판다에 복사한 뒤 실행하세요.

    python3 tools/sd2mp4.py 000123.bin              # 000123.mp4 생성
    python3 tools/sd2mp4.py /media/sd/loop/*.bin     # 여러 개 한 번에
"""
import struct
import subprocess
import sys
import tempfile
from datetime import datetime
from pathlib import Path

HDR = struct.Struct("<BqI")   # 종류, epoch ms, 길이


def convert(src: Path):
    frames = 0
    first = last = None
    with tempfile.TemporaryDirectory() as tmp:
        v, a = Path(tmp, "v.mjpeg"), Path(tmp, "a.pcm")
        with open(src, "rb") as f, open(v, "wb") as vf, open(a, "wb") as af:
            while True:
                h = f.read(HDR.size)
                if len(h) < HDR.size:
                    break
                kind, ts, n = HDR.unpack(h)
                data = f.read(n)
                if len(data) < n:
                    break
                if kind == 1:
                    vf.write(data)
                    frames += 1
                    first = ts if first is None else first
                    last = ts
                elif kind == 2:
                    af.write(data)
        if frames < 2:
            print(f"{src.name}: 프레임이 부족해 건너뜁니다")
            return
        fps = max(1.0, min(30.0, (frames - 1) / max((last - first) / 1000, 0.1)))
        out = src.with_suffix(".mp4")
        cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-f", "mjpeg", "-framerate", f"{fps:.3f}",
               "-i", str(v)]
        if a.stat().st_size > 16000:
            cmd += ["-f", "s16le", "-ar", "16000", "-ac", "1", "-i", str(a), "-c:a", "aac", "-shortest"]
        cmd += ["-c:v", "libx264", "-preset", "veryfast", "-crf", "24", "-pix_fmt", "yuv420p",
                "-movflags", "+faststart", str(out)]
        subprocess.run(cmd, check=True)
        start = datetime.fromtimestamp(first / 1000) if first > 1e12 else None
        print(f"{src.name} -> {out.name} ({frames}장, {fps:.1f}fps"
              f"{', 시작 ' + start.strftime('%Y-%m-%d %H:%M:%S') if start else ''})")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    for arg in sys.argv[1:]:
        convert(Path(arg))
