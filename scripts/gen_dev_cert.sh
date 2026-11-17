#!/usr/bin/env bash
# Generates a local self-signed certificate for the panel/agent HTTPS listener.
# SANs cover loopback, the machine hostname and its primary LAN IPv4 address,
# so agents on the local network can verify the certificate by name or IP.
# Usage: scripts/gen_dev_cert.sh [output-dir]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/state/dev-tls}"
mkdir -p "$OUT"
CERT="$OUT/cert.pem"
KEY="$OUT/key.pem"

SAN="DNS:localhost,IP:127.0.0.1"
HOST="$(hostname 2>/dev/null || true)"
if [ -n "$HOST" ]; then
    SAN="$SAN,DNS:$HOST"
fi
LAN_IP="$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)"
if [ -n "$LAN_IP" ]; then
    SAN="$SAN,IP:$LAN_IP"
fi

openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
    -keyout "$KEY" -out "$CERT" \
    -subj "/CN=${HOST:-localhost}" \
    -addext "subjectAltName=$SAN" >/dev/null 2>&1
chmod 600 "$KEY"
echo "cert: $CERT"
echo "key:  $KEY"
echo "SAN:  $SAN"
