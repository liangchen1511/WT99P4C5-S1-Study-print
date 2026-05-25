#!/bin/bash
set -euo pipefail
TOKEN=$(curl -fsS -m 5 -X POST http://127.0.0.1:3000/parent/api/login \
  -H 'Content-Type: application/json' \
  -d '{"device_code":"WT99-DEMO-01","password":"parent2025"}' \
  | python3 -c 'import sys,json; print(json.load(sys.stdin)["token"])')
echo "token ok"
curl -fsS -m 5 -X PUT http://127.0.0.1:3000/parent/api/policy \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"policy":{"version":1,"timezone":"Asia/Shanghai","rules":[{"name":"学习时段","days":[1,2,3,4,5],"start":"19:00","end":"21:00","allow_apps":["soti"],"deny_apps":["game_2048"]}],"app_toggles":{"game_2048":false,"video":true,"music":true}}}'
echo ""
curl -fsS -m 5 -H "Authorization: Bearer $TOKEN" http://127.0.0.1:3000/parent/api/stats
echo ""
