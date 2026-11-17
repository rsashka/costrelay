#!/bin/sh
# Install dsh-web.service as a system service. The service starts at boot.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEMPLATE="$ROOT/dsh/dsh-web.service.in"
UNIT=/etc/systemd/system/dsh-web.service
RUN_USER="${SUDO_USER:-$(stat -c '%U' "$ROOT")}"
RUN_GROUP="$(stat -c '%G' "$ROOT")"
RUN_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
DSH_HOME_DIR="${DSH_HOME:-$RUN_HOME/.dsh}"

if [ "$(id -u)" -ne 0 ]; then
    echo "error: run with sudo $0" >&2
    exit 1
fi
if [ -z "$RUN_HOME" ]; then
    echo "error: no home directory for $RUN_USER" >&2
    exit 1
fi

# A system service and the old per-user unit must not compete for port 3080.
USER_WANTS="$RUN_HOME/.config/systemd/user/default.target.wants/dsh-web.service"
if [ -L "$USER_WANTS" ]; then
    rm -f "$USER_WANTS"
    echo "disabled the per-user dsh-web.service"
fi

systemctl stop dsh-web.service 2>/dev/null || true
sed -e "s|@ROOT@|$ROOT|g" \
    -e "s|@USER@|$RUN_USER|g" \
    -e "s|@GROUP@|$RUN_GROUP|g" \
    -e "s|@HOME@|$RUN_HOME|g" \
    -e "s|@DSH_HOME@|$DSH_HOME_DIR|g" \
    "$TEMPLATE" > "$UNIT"
chmod 644 "$UNIT"
systemctl daemon-reload
systemctl enable dsh-web.service
echo "installed and enabled: $UNIT"
echo "start now with: sudo systemctl start dsh-web.service"
