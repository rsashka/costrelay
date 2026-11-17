#!/usr/bin/env bash
# Дымовая проверка TLS: панель по HTTPS, отказ HTTP-панели, чат агента, метрики.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$SMOKE_ROOT"

TLS_DIR="$SMOKE_ROOT/state/dev-tls"
"$SMOKE_ROOT/scripts/gen_dev_cert.sh" "$TLS_DIR" >/dev/null
CFG="$(mktemp)"
COOKIES="$(mktemp)"
SMOKE_PATHS+=("$CFG" "$COOKIES")

python3 - "$SMOKE_ROOT/config/costrelay.dev.yaml" "$CFG" "$TLS_DIR/cert.pem" "$TLS_DIR/key.pem" \
    "$SMOKE_ROOT/build/costrelayd" <<'PY'
import re, subprocess, sys
src, dst, cert, key, binary = sys.argv[1:]
text = open(src, encoding="utf-8").read()
# Smoke использует предсказуемый dev-пароль, не локальный secret-файл.
admin_hash = subprocess.check_output([binary, "--hash-password"], input=b"admin\n").decode().strip()
text = re.sub(r'password_hash: "[^"]*"', f'password_hash: "{admin_hash}"', text, count=1)
text = text.replace(
    '  listen: "127.0.0.1:18080"\n',
    f'  listen: "127.0.0.1:18080"\n'
    f'  tls:\n'
    f'    listen: "0.0.0.0:18443"\n'
    f'    cert_file: "{cert}"\n'
    f'    key_file: "{key}"\n',
    1,
)
open(dst, "w", encoding="utf-8").write(text)
PY

smoke_start proxy-https-mock.log python3 scripts/mock_routerai.py
sleep 1
smoke_start proxy-https-app.log ./build/costrelayd --config "$CFG"
smoke_wait_http https://127.0.0.1:18443/healthz -k

echo "== HTTP panel is denied"
code=$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18080/panel/login)
[ "$code" = "403" ] || smoke_die "expected 403, got $code"
echo "403"

echo "== HTTPS panel login"
curl -skf -c "$COOKIES" -d 'username=admin&password=admin' \
  https://127.0.0.1:18443/panel/login -o /dev/null
curl -skf -b "$COOKIES" https://127.0.0.1:18443/panel/api/overview | head -c 200
echo

echo "== HTTPS chat"
curl -skf -H 'Authorization: Bearer dev-token' -H 'Content-Type: application/json' \
  -d '{"model":"default","messages":[{"role":"user","content":"hi"}],"max_tokens":10}' \
  https://127.0.0.1:18443/v1/chat/completions
echo

echo "== metrics"
curl -skf https://127.0.0.1:18443/metrics | grep -E '^costrelay_(requests_total|stats_db_available|up)' || true
