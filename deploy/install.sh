#!/bin/sh
# Installs a Release build and the systemd units on a Debian/Ubuntu host.
# Run from the repository root as root after `cmake --build build-release`.
set -eu
BUILD=${1:-build-release}
install -d /etc/tradebot /var/lib/tradebot/data /var/lib/tradebot/runs
id -u tradebot >/dev/null 2>&1 || useradd --system --home /var/lib/tradebot --shell /usr/sbin/nologin tradebot
install -m 0755 "$BUILD"/tools/tradebot-* /usr/local/bin/
install -m 0644 deploy/systemd/*.service deploy/systemd/*.timer /etc/systemd/system/
[ -f /etc/tradebot/collect.conf ] || install -m 0644 configs/collect.example.conf /etc/tradebot/collect.conf
[ -f /etc/tradebot/secrets.env ] || { install -m 0600 deploy/env.example /etc/tradebot/secrets.env; chown tradebot:tradebot /etc/tradebot/secrets.env; }
chown -R tradebot:tradebot /var/lib/tradebot
systemctl daemon-reload
echo "installed. next: edit /etc/tradebot/*.conf and secrets.env, then"
echo "  systemctl enable --now tradebot-collect"
echo "  systemctl enable --now tradebot-live@<instance> tradebot-watchdog@<instance>.timer"
