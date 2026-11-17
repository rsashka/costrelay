#!/usr/bin/env bash
# Устанавливает корневой сертификат локального УЦ в системное хранилище доверия
# Linux-клиента. Автоматически выбирает механизм дистрибутива:
#   ca-certificates (Debian/Ubuntu) -> update-ca-certificates
#   ca-trust (RHEL/Fedora/Arch/…)   -> update-ca-trust
#   иначе p11-kit                    -> trust anchor --store
# Запуск на КЛИЕНТЕ: sudo ./install-client-ca.sh [путь/к/ca.pem]
# По умолчанию берёт ca.pem рядом со скриптом. Корень публичный, приватный ключ не нужен.
set -euo pipefail

CA_SRC="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ca.pem}"
[ -f "$CA_SRC" ] || { echo "не найден сертификат: $CA_SRC" >&2; exit 1; }
# Проверить, что это именно сертификат, а не ключ.
grep -q "BEGIN CERTIFICATE" "$CA_SRC" || { echo "$CA_SRC не похож на PEM-сертификат" >&2; exit 1; }

if command -v update-ca-certificates >/dev/null 2>&1; then
    DST="/usr/local/share/ca-certificates/costrelay-root-ca.crt"
    cp "$CA_SRC" "$DST"
    update-ca-certificates
    echo "Установлено в ca-certificates: $DST"
elif command -v update-ca-trust >/dev/null 2>&1; then
    DST="/etc/pki/ca-trust/source/anchors/costrelay-root-ca.pem"
    [ -d /etc/pki/ca-trust/source/anchors ] || DST="/etc/ca-certificates/trust-source/anchors/costrelay-root-ca.pem"
    cp "$CA_SRC" "$DST"
    update-ca-trust extract 2>/dev/null || update-ca-trust
    echo "Установлено в ca-trust: $DST"
elif command -v trust >/dev/null 2>&1; then
    trust anchor --store "$CA_SRC"
    echo "Установлено через trust anchor"
else
    echo "Не найден ни update-ca-certificates, ни update-ca-trust, ни trust." >&2
    echo "Добавьте $CA_SRC в хранилище доверия вручную (см. README.md, раздел «TLS и доверие локальному УЦ»)." >&2
    exit 1
fi

echo "Проверка: curl https://server.lan:8444/healthz должен вернуть 200 без -k"
