#!/usr/bin/env bash
# Дымовая проверка HTTP API и панели на мок-провайдере.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$SMOKE_ROOT"

smoke_start proxy-mock.log python3 scripts/mock_routerai.py
sleep 1
smoke_start proxy-app.log ./build/costrelayd --config config/costrelay.dev.yaml
smoke_wait_http http://127.0.0.1:18080/healthz

echo "== models"
curl -sf -H 'Authorization: Bearer dev-token' http://127.0.0.1:18080/v1/models
echo
echo "== non-stream"
curl -sf -H 'Authorization: Bearer dev-token' -H 'Content-Type: application/json' \
  -d '{"model":"default","messages":[{"role":"user","content":"hi"}],"max_tokens":10}' \
  http://127.0.0.1:18080/v1/chat/completions
echo
echo "== stream"
curl -sS -N -m 10 -H 'Authorization: Bearer dev-token' -H 'Content-Type: application/json' \
  -d '{"model":"default","messages":[{"role":"user","content":"hi"}],"stream":true}' \
  http://127.0.0.1:18080/v1/chat/completions | head -20
