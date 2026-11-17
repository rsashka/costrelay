#!/usr/bin/env bash
# Общие шаги smoke-скриптов: фоновый запуск, ожидание готовности, уборка.
# Подключается через `. scripts/lib.sh`; ROOT — каталог репозитория.
set -euo pipefail

SMOKE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SMOKE_LOG_DIR="${SMOKE_LOG_DIR:-/tmp}"
SMOKE_WAIT_TRIES="${SMOKE_WAIT_TRIES:-80}"
SMOKE_PIDS=()
SMOKE_PATHS=()

# Возвращает 0 всегда: иначе статус ловушки EXIT подменит статус скрипта.
smoke_cleanup() {
    local pid path
    for pid in "${SMOKE_PIDS[@]:-}"; do
        if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || true; fi
    done
    for path in "${SMOKE_PATHS[@]:-}"; do
        if [ -n "$path" ]; then rm -rf "$path" || true; fi
    done
    return 0
}
trap smoke_cleanup EXIT

# smoke_start <log-имя> <команда...> — запускает команду в фоне с логом в /tmp.
smoke_start() {
    local log="$SMOKE_LOG_DIR/$1"
    shift
    "$@" >"$log" 2>&1 &
    SMOKE_PIDS+=("$!")
}

# smoke_wait_http <url> [флаги curl...] — ждёт ответа, иначе выходит с ошибкой.
smoke_wait_http() {
    local url="$1"
    shift
    local attempt
    for attempt in $(seq 1 "$SMOKE_WAIT_TRIES"); do
        if curl -sf -m 1 "$@" "$url" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    echo "error: $url не отвечает (попыток: $SMOKE_WAIT_TRIES)" >&2
    return 1
}

smoke_die() {
    echo "error: $*" >&2
    exit 1
}
