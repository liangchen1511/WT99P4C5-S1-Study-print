#!/bin/bash
# 在 ECS 上 root 执行（路径可按习惯改）。用法：
#   scp tools/soti-standalone-server/soti_upload_server.py root@8.154.20.8:/opt/soti/
#   scp -r tools/soti-standalone-server/ecs/* root@8.154.20.8:/tmp/soti-ecs/
#   ssh root@8.154.20.8 'bash /tmp/soti-ecs/deploy-on-ecs.sh'
set -euo pipefail

OPT_DIR=/opt/soti
ENV_FILE=/etc/soti-upload.env

mkdir -p "$OPT_DIR"
if [[ ! -f "$OPT_DIR/soti_upload_server.py" ]]; then
  echo "缺少 $OPT_DIR/soti_upload_server.py，请先从本机 scp 上传。" >&2
  exit 1
fi

if [[ ! -f "$OPT_DIR/soti_modes.py" ]]; then
  echo "缺少 $OPT_DIR/soti_modes.py，请 scp tools/soti-standalone-server/soti_modes.py" >&2
  exit 1
fi
for f in parent_store.py parent_routes.py album_preprocess.py; do
  if [[ ! -f "$OPT_DIR/$f" ]]; then
    echo "缺少 $OPT_DIR/$f（家长控制台），请 scp tools/soti-standalone-server/$f" >&2
    exit 1
  fi
done
if [[ ! -f "$OPT_DIR/static/parent/index.html" ]]; then
  echo "缺少 $OPT_DIR/static/parent/，请 scp -r tools/soti-standalone-server/static/parent $OPT_DIR/static/" >&2
  exit 1
fi
python3 -m py_compile "$OPT_DIR/soti_upload_server.py" "$OPT_DIR/soti_modes.py" "$OPT_DIR/soti_answer_sanitize.py" \
  "$OPT_DIR/parent_store.py" "$OPT_DIR/parent_routes.py" "$OPT_DIR/album_preprocess.py"
pip3 install -q Pillow 2>/dev/null || echo "WARN: pip install Pillow 失败，将跳过二值化"

if [[ ! -f "$ENV_FILE" ]]; then
  cp /tmp/soti-ecs/soti-upload.env.example "$ENV_FILE" 2>/dev/null || true
  chmod 600 "$ENV_FILE"
  echo "已创建 $ENV_FILE — 请编辑填入 DOUBAO_API_KEY 与 DOUBAO_MODEL=ep-xxx 后重新运行本脚本或 systemctl restart"
  exit 2
fi

grep -q '^DOUBAO_API_KEY=.\+' "$ENV_FILE" || { echo "请设置 DOUBAO_API_KEY" >&2; exit 2; }
grep -q '^DOUBAO_MODEL=ep-' "$ENV_FILE" || { echo "请设置 DOUBAO_MODEL=ep-xxxx（方舟推理接入点 ID）" >&2; exit 2; }

cp /tmp/soti-ecs/soti-upload.service /etc/systemd/system/soti-upload.service
systemctl daemon-reload
systemctl enable soti-upload
systemctl restart soti-upload

sleep 1
curl -fsS "http://127.0.0.1:3000/upload/health" | head -c 200
echo ""
curl -fsS -o /dev/null -w "parent/ HTTP %{http_code}\n" "http://127.0.0.1:3000/parent/" || echo "WARN: /parent/ 非 200，检查 static/parent"
systemctl --no-pager status soti-upload || true
echo "OK: 本机 health 通过。"
echo "家长页: 须 Nginx 配置 location /parent — 运行: bash /tmp/soti-ecs/deploy-parent-to-ecs.sh"
echo "公网再测: curl http://8.154.20.8/upload/health && curl -I http://8.154.20.8/parent/"
