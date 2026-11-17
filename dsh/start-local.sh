#!/bin/sh
# Local DSH for costrelayd. Listens on loopback only; dsh/local-no-auth.mjs
# removes DSH's launch token, so the proxy panel is the only authentication.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

HOST="${DSH_HOST:-127.0.0.1}"
PORT="${DSH_PORT:-3080}"
PROFILE="${DSH_PROFILE:-web}"

# DSH requires an absolute path in the overlay, so the patch is rendered per
# checkout instead of being committed with one machine's path.
PATCH="$ROOT/state/dsh-local-service.patch.yml"
mkdir -p "$ROOT/state"
cat > "$PATCH" <<EOF
# Local-only DSH overlay for dsh-web.service. DSH binds 127.0.0.1:3080.
# The proxy panel authenticates remote users; no DSH token is needed locally.
- insert:
    - id: local-no-auth
      name: "file:$ROOT/dsh/local-no-auth.mjs"

# Do not print a launch-token URL to the journal and never open a browser.
- id: web-runtime
  config:
    openBrowser: false
    printUrl: false
    surfaceContext: true
EOF

case "$HOST" in
    127.0.0.1|::1) ;;
    *)
        echo "error: DSH_HOST must be a loopback IP literal (127.0.0.1 or ::1)" >&2
        exit 2
        ;;
esac

DSH_BIN="${DSH_BIN:-}"
if [ -z "$DSH_BIN" ]; then
    for cand in "$HOME"/.npm/_npx/*/node_modules/@deepseek-ai/dsh/lib/bin.js; do
        [ -f "$cand" ] && DSH_BIN="$cand" && break
    done
fi
if [ -z "$DSH_BIN" ] || [ ! -f "$DSH_BIN" ]; then
    echo "error: DSH not found; install it with: npx @deepseek-ai/dsh --version" >&2
    exit 1
fi

# If a manual DSH already owns the port, wait for it instead of failing in a
# systemd restart loop. At boot the port is free, so DSH starts immediately.
if command -v ss >/dev/null 2>&1 && ss -ltn 2>/dev/null | grep -q ":$PORT "; then
    echo "waiting for $HOST:$PORT to be released by the running DSH" >&2
    while ss -ltn 2>/dev/null | grep -q ":$PORT "; do
        sleep 5
    done
fi

exec node "$DSH_BIN" --profile "$PROFILE" \
    --patch "$PATCH" \
    --no-open --host "$HOST" --port "$PORT"
