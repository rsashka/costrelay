#!/usr/bin/env bash
# Real RouterAI smoke:
#   * public /models + per-model /endpoints price API;
#   * starts the proxy with auto-refresh and validates /readyz + /v1/models.
# With ROUTERAI_API_KEY and ROUTERAI_SMOKE_CHAT=1 it also performs one real
# (paid) chat completion. Without the key only the public price API is checked.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
cd "$SMOKE_ROOT"

BASE="${ROUTERAI_BASE_URL:-https://routerai.ru/api/v1}"
KEY="${ROUTERAI_API_KEY:-}"
CHAT="${ROUTERAI_SMOKE_CHAT:-0}"
PORT="${ROUTERAI_SMOKE_PORT:-18991}"
TMP="$(mktemp -d)"
SMOKE_PATHS+=("$TMP")
SMOKE_WAIT_TRIES=120

MODEL="$(python3 - "$BASE" "$TMP/config.yaml" "$TMP/state" "$KEY" "$PORT" <<'PY'
import hashlib, json, os, sys, urllib.parse, urllib.request

base, cfg_path, state, key, port = sys.argv[1:]
base = base.rstrip("/")
headers = {"Accept": "application/json", "User-Agent": "costrelay-smoke/0.1"}
if key:
    headers["Authorization"] = "Bearer " + key

def get(url):
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)

models = [m["id"] for m in get(base + "/models").get("data", []) if "/" in m.get("id", "")]
preferred = ["deepseek/deepseek-v4-pro", "openai/gpt-4o-mini",
             "google/gemini-2.0-flash-001", "anthropic/claude-sonnet-4.5"]
ordered = [m for m in preferred if m in models] + [m for m in models if m not in preferred]
chosen = []
for model in ordered[:20]:
    author, name = model.split("/", 1)
    url = f"{base}/models/{urllib.parse.quote(author)}/{urllib.parse.quote(name)}/endpoints"
    try:
        data = get(url)
        eps = (data.get("data") or {}).get("endpoints") or []
        if eps:
            chosen.append(model)
            if len(chosen) >= 2:
                break
    except Exception:
        continue
if not chosen:
    raise SystemExit("no RouterAI model with a public endpoints price list")

os.makedirs(os.path.dirname(cfg_path), exist_ok=True)
os.makedirs(state, exist_ok=True)
q = lambda s: json.dumps(s)
api_key_line = f'      api_key: {q(key)}\n' if key else ''
models_yaml = "".join(f"  - {q(m)}\n" for m in chosen)
smoke_hash = hashlib.sha256(b"routerai-smoke-token").hexdigest()
cfg = f"""proxy:
  name: "routerai-smoke"
  data_dir: {q(state)}
  listen: "127.0.0.1:{int(port)}"
portal: {{ enabled: false }}
models:
{models_yaml}default_model: {q(chosen[0])}
providers:
  external:
    - id: "routerai"
      type: "routerai"
      url: {q(base)}
{api_key_line}consumers:
  - id: "routerai-smoke"
    token_hash: "sha256:{smoke_hash}"
    models: ["*"]
"""
with open(cfg_path, "w", encoding="utf-8") as f:
    f.write(cfg)
os.chmod(cfg_path, 0o600)
print(chosen[0])
print("catalog models: " + ", ".join(chosen), file=sys.stderr)
PY
)"
echo "== RouterAI price API"
echo "model: $MODEL"

smoke_start proxy-routerai.log ./build/costrelayd --config "$TMP/config.yaml"
if ! smoke_wait_http "http://127.0.0.1:$PORT/readyz" -o "$TMP/ready.json"; then
  cat "$TMP/proxy-routerai.log" >&2
  exit 1
fi
python3 - "$TMP/ready.json" <<'PY'
import json, sys
v = json.load(open(sys.argv[1]))
assert v.get("ready") is True, v
assert int(v.get("catalog_models", 0)) > 0, v
print("readyz:", v)
PY

echo "== /v1/models through the proxy"
curl -sf -H 'Authorization: Bearer routerai-smoke-token' \
  "http://127.0.0.1:$PORT/v1/models" | head -c 240
echo

if [[ -z "$KEY" ]]; then
  echo "== chat skipped: set ROUTERAI_API_KEY to enable the paid smoke"
  exit 0
fi
if [[ "$CHAT" != "1" ]]; then
  echo "== chat skipped: set ROUTERAI_SMOKE_CHAT=1 to allow one paid request"
  exit 0
fi

echo "== real chat completion (paid)"
curl -sS -m 120 -w '\nHTTP %{http_code}\n' \
  -H 'Authorization: Bearer routerai-smoke-token' -H 'Content-Type: application/json' \
  -d "{\"model\":\"$MODEL\",\"messages\":[{\"role\":\"user\",\"content\":\"Reply with OK only.\"}],\"max_tokens\":8}" \
  "http://127.0.0.1:$PORT/v1/chat/completions" | tee "$TMP/chat.txt"
python3 - "$TMP/chat.txt" <<'PY'
import json, sys
raw = open(sys.argv[1], encoding="utf-8").read()
body = raw.rsplit("\nHTTP ", 1)[0]
v = json.loads(body)
assert v.get("choices"), v
print("chat choices:", len(v["choices"]))
PY
