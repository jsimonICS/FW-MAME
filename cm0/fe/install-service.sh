#!/bin/sh
# install-service.sh [--uninstall] - install fwmame.service (not enabled: the
# FW2 display app starts it when you launch FW-MAME).
#   sudo sh ~/fwmame/src/fe/install-service.sh
#   sudo sh ~/fwmame/src/fe/install-service.sh --uninstall
set -e
cd "$(dirname "$0")"
[ "$(id -u)" -eq 0 ] || { echo "run as root" >&2; exit 1; }
if [ "$1" = --uninstall ]; then
  systemctl stop fwmame 2>/dev/null || true
  rm -f /etc/systemd/system/fwmame.service
  systemctl daemon-reload
  echo "fwmame.service removed"
  exit 0
fi
install -m 0644 fwmame.service /etc/systemd/system/fwmame.service
systemctl daemon-reload
echo "fwmame.service installed (started on demand by the FW2 display app)"
