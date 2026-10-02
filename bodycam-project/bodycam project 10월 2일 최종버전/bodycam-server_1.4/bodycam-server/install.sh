#!/usr/bin/env bash
# 바디캠 서버 설치 스크립트 (라떼판다에서 일반 사용자로 실행: ./install.sh)
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
ME="$(whoami)"
cd "$DIR"

echo "[1/5] 파이썬 가상환경과 패키지 설치"
python3 -m venv venv
./venv/bin/pip install --upgrade pip -q
./venv/bin/pip install -r requirements.txt -q

echo "[2/5] 하드웨어 인코더 사용 권한 (render, video 그룹)"
sudo usermod -aG render,video "$ME"

echo "[3/5] 서비스 등록 (부팅 시 자동 실행)"
sed -e "s|__USER__|$ME|g" -e "s|__DIR__|$DIR|g" deploy/bodycam.service | sudo tee /etc/systemd/system/bodycam.service >/dev/null
sudo systemctl daemon-reload
sudo systemctl enable --now bodycam
sudo systemctl restart bodycam

echo "[4/5] nginx 설정 (기존 default는 백업 후 비활성화)"
sudo cp deploy/nginx-bodycam.conf /etc/nginx/sites-available/bodycam
if [ -L /etc/nginx/sites-enabled/default ]; then
    sudo cp /etc/nginx/sites-available/default "$HOME/nginx-default.backup" 2>/dev/null || true
    sudo rm /etc/nginx/sites-enabled/default
fi
sudo ln -sf /etc/nginx/sites-available/bodycam /etc/nginx/sites-enabled/bodycam
sudo nginx -t
sudo systemctl reload nginx

echo "[5/5] 상태 확인"
sleep 2
systemctl is-active bodycam && curl -s http://127.0.0.1/api/health && echo
echo
echo "설치 완료. 대시보드: http://$(hostname).local  또는  http://10.42.0.1"
