#!/usr/bin/env bash
# Локальный УЦ (Root CA, уровень 1) + серверный сертификат второго уровня (leaf).
#
# Схема доверия из двух уровней:
#   [1] Root CA  (самоподписанный, CA:TRUE, срок >= 5 лет по умолчанию 10 лет)
#         \__ подписывает
#   [2] Server certificate (CA:FALSE, EKU=serverAuth, срок 5 лет) — именно он
#       обслуживает HTTPS-листенер прокси. Самоподписанным НЕ является: он
#       выпущен Root CA, поэтому в браузере «зелёный замок» при установленном
#       в доверие корневом сертификате.
#
# SAN содержат только DNS-имена (без IP): адрес выдаёт DHCP и он меняется, а TLS
# проверяется по имени из SAN, поэтому сертификат к IP не привязан.
#
# Итоговые файлы:
#   $CA_DIR/ca.key        приватный ключ Root CA (0600, раздать нельзя)
#   $CA_DIR/ca.pem        Root CA — установить в доверие на клиентах
#   $CA_DIR/ca.cer        Root CA в DER — тот же ca.pem для Windows-мастеров
#   $TLS_DIR/server.key   приватный ключ серверного сертификата (0600)
#   $TLS_DIR/server.pem   серверный сертификат (уровень 2)
#   $TLS_DIR/fullchain.pem серверный сертификат + Root CA — класть в tls.cert_file
#
# Повторный запуск: Root CA не перезаписывается (иначе сломается доверие на уже
# настроенных клиентах). Пересоздать УЦ — только с --force-ca.
#
# Использование:
#   scripts/gen_local_ca.sh [--force-ca]
# Переменные окружения: CA_DIR, TLS_DIR, CA_CN, CA_DAYS, LEAF_DAYS, SAN, KEY_BITS_CA,
# KEY_BITS_LEAF. SAN перечисляет имена через запятую, напр. "DNS:server.lan,DNS:server".
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CA_DIR="${CA_DIR:-$ROOT/state/pki}"
TLS_DIR="${TLS_DIR:-$ROOT/state/tls}"
CA_CN="${CA_CN:-CostRelay Local Root CA}"
CA_ORG="${CA_ORG:-Home LAN}"
CA_DAYS="${CA_DAYS:-3650}"     # корневой УЦ: 10 лет (не менее 5 по требованию)
LEAF_DAYS="${LEAF_DAYS:-1825}" # серверный сертификат: 5 лет
KEY_BITS_CA="${KEY_BITS_CA:-4096}"
KEY_BITS_LEAF="${KEY_BITS_LEAF:-2048}"

FORCE_CA=0
[ "${1:-}" = "--force-ca" ] && FORCE_CA=1

# --- SAN по умолчанию: hostname, hostname.<DHCP-домен>, hostname.local (mDNS), localhost.
# IP не добавляем — сертификат не должен зависеть от DHCP-адреса машины.
if [ -z "${SAN:-}" ]; then
    HOST="$(hostname 2>/dev/null || echo localhost)"
    DOMAIN="$(resolvectl status 2>/dev/null | awk '/DNS Domain/{print $NF; exit}' || true)"
    names=("$HOST")
    [ -n "$DOMAIN" ] && names+=("$HOST.$DOMAIN")
    names+=("$HOST.local" "localhost")
    seen=""; SAN=""
    for n in "${names[@]}"; do
        case ",$seen," in *",$n,"*) continue ;; esac
        seen="$seen,$n"; SAN="$SAN${SAN:+,}DNS:$n"
    done
    PRIMARY_CN="$HOST"
    [ -n "$DOMAIN" ] && PRIMARY_CN="$HOST.$DOMAIN"
else
    PRIMARY_CN="$(printf '%s' "$SAN" | sed -n 's/^DNS:\([^,]*\).*/\1/p')"
    [ -n "$PRIMARY_CN" ] || PRIMARY_CN="$(hostname 2>/dev/null || echo localhost)"
fi

mkdir -p "$CA_DIR" "$TLS_DIR"
chmod 700 "$CA_DIR"

CA_KEY="$CA_DIR/ca.key"
CA_CRT="$CA_DIR/ca.pem"
CA_CER="$CA_DIR/ca.cer"
SRV_KEY="$TLS_DIR/server.key"
SRV_CSR="$TLS_DIR/server.csr"
SRV_CRT="$TLS_DIR/server.pem"
FULLCHAIN="$TLS_DIR/fullchain.pem"

# --- [1] Root CA -------------------------------------------------------------
if [ -f "$CA_KEY" ] && [ -f "$CA_CRT" ] && [ "$FORCE_CA" -ne 1 ]; then
    echo "Root CA уже существует, пропускаю (пересоздать: $0 --force-ca): $CA_CRT"
else
    umask 077
    openssl req -x509 -newkey "rsa:$KEY_BITS_CA" -sha256 -days "$CA_DAYS" -nodes \
        -keyout "$CA_KEY" -out "$CA_CRT" \
        -subj "/O=$CA_ORG/CN=$CA_CN" \
        -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -addext "subjectKeyIdentifier=hash" >/dev/null 2>&1
    echo "Создан Root CA: $CA_CRT"
fi
chmod 600 "$CA_KEY"

# Root CA в DER — для «Установить сертификат» на Windows.
openssl x509 -in "$CA_CRT" -outform DER -out "$CA_CER" >/dev/null 2>&1

# --- [2] Серверный сертификат второго уровня --------------------------------
EXT="$(mktemp)"; CSR_CFG="$(mktemp)"
trap 'rm -f "$EXT" "$CSR_CFG"' EXIT
cat >"$EXT" <<EOF
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = $SAN
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
EOF

umask 077
openssl genpkey -algorithm RSA -pkeyopt "rsa_keygen_bits:$KEY_BITS_LEAF" -out "$SRV_KEY" >/dev/null 2>&1
openssl req -new -key "$SRV_KEY" -subj "/O=$CA_ORG/CN=$PRIMARY_CN" -out "$SRV_CSR" >/dev/null 2>&1
openssl x509 -req -in "$SRV_CSR" -CA "$CA_CRT" -CAkey "$CA_KEY" -CAcreateserial \
    -days "$LEAF_DAYS" -sha256 -extfile "$EXT" -out "$SRV_CRT" >/dev/null 2>&1
cat "$SRV_CRT" "$CA_CRT" >"$FULLCHAIN"
chmod 600 "$SRV_KEY"

# --- Проверка и сводка -------------------------------------------------------
echo "== Проверка цепочки: leaf против Root CA =="
openssl verify -CAfile "$CA_CRT" "$SRV_CRT"
openssl verify -CAfile "$CA_CRT" "$FULLCHAIN"

echo "== Root CA =="
openssl x509 -in "$CA_CRT" -noout -subject -issuer -dates
echo "== Серверный сертификат (уровень 2) =="
openssl x509 -in "$SRV_CRT" -noout -subject -issuer -dates -ext basicConstraints -ext keyUsage -ext extendedKeyUsage -ext subjectAltName

echo
echo "Готово."
echo "  tls.cert_file: $FULLCHAIN"
echo "  tls.key_file:  $SRV_KEY"
echo "  Доверять клиентам: $CA_CRT (или $CA_CER)"
echo "  SAN: $SAN"
