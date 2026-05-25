#!/bin/bash
# 在 ECS 上执行：补全家长端文件 + 更新 Nginx（解决 /parent 404）
# 本机先 scp 整个 soti-standalone-server 目录，或至少下列文件到 /opt/soti/
set -euo pipefail

OPT_DIR=/opt/soti
NGINX_SITE=/etc/nginx/sites-available/soti-upload
NGINX_ENABLED=/etc/nginx/sites-enabled/soti-upload
REPO_SNIPPET=/tmp/soti-ecs/nginx-soti-upload-site.conf

echo "==> 检查 /opt/soti 家长端文件"
for f in parent_store.py parent_routes.py soti_upload_server.py; do
  if [[ ! -f "$OPT_DIR/$f" ]]; then
    echo "缺少 $OPT_DIR/$f" >&2
    echo "本机执行: scp tools/soti-standalone-server/$f root@<EIP>:/opt/soti/" >&2
    exit 1
  fi
done
if [[ ! -f "$OPT_DIR/static/parent/index.html" ]]; then
  echo "缺少 $OPT_DIR/static/parent/ — 本机执行:" >&2
  echo "  scp -r tools/soti-standalone-server/static/parent root@<EIP>:/opt/soti/static/" >&2
  exit 1
fi

python3 -m py_compile "$OPT_DIR"/parent_store.py "$OPT_DIR"/parent_routes.py "$OPT_DIR"/soti_upload_server.py

echo "==> 重启 soti-upload"
systemctl restart soti-upload
sleep 1

echo "==> 本机 Python 测家长页"
curl -fsS -o /dev/null -w "GET /parent/ => %{http_code}\n" http://127.0.0.1:3000/parent/
curl -fsS -o /dev/null -w "GET /parent/styles.css => %{http_code}\n" http://127.0.0.1:3000/parent/styles.css

if [[ -f "$REPO_SNIPPET" ]]; then
  echo "==> 安装 Nginx 站点（含 location /parent）"
  cp "$REPO_SNIPPET" "$NGINX_SITE"
  ln -sf "$NGINX_SITE" "$NGINX_ENABLED"
  # 去掉 default 站点抢 80 端口（可选）
  rm -f /etc/nginx/sites-enabled/default 2>/dev/null || true
  nginx -t
  systemctl reload nginx
elif grep -q 'location /parent' "$NGINX_SITE" 2>/dev/null; then
  echo "==> Nginx 已有 location /parent，仅 reload"
  nginx -t && systemctl reload nginx
else
  echo "WARN: 未找到 $REPO_SNIPPET，请手动在 Nginx 的 server 里加入:" >&2
  cat >&2 <<'EOF'
    location /parent {
        proxy_pass http://127.0.0.1:3000;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
EOF
  exit 3
fi

EIP=$(curl -s --max-time 2 http://100.100.100.200/latest/meta-data/eipv4 2>/dev/null || true)
echo "==> 经 Nginx 测（本机）"
curl -fsS -o /dev/null -w "GET /parent/ via nginx => %{http_code}\n" -H "Host: 127.0.0.1" http://127.0.0.1/parent/ || true

echo ""
echo "完成。浏览器打开: http://${EIP:-你的公网IP}/parent/"
echo "若仍 404: 确认 server_name 含公网 IP，且访问 URL 为 /parent/ 不是根路径 /"
