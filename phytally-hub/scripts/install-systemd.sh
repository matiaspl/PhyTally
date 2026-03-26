#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "run as root" >&2
  exit 1
fi

install -d /opt/phytally /etc/phytally
install -m 0755 ./phytally-hub-linux-arm64 /usr/local/bin/phytally-hub
install -m 0644 ./phytally-hub.example.json /etc/phytally/phytally-hub.json
install -m 0644 ./deploy/systemd/phytally-hub.service /etc/systemd/system/phytally-hub.service
systemctl daemon-reload
systemctl enable --now phytally-hub.service
