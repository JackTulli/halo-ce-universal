#!/bin/sh
# Installs (or updates) the master server and its status page on a Linux
# host with systemd and Apache (Ubuntu/Debian: the www-data user):
#   port/master/deploy.sh root@172.104.30.93 [ssh options...]
# Copies halo_master.py to /opt/halo-master and index.html to
# /var/www/html/halo (where the server writes status.json), installs the
# service, allows UDP 27920 in ufw when ufw is active, and (re)starts it.
set -e
target="$1"
[ -n "$target" ] || { echo "usage: deploy.sh user@host [ssh options]" >&2; exit 2; }
shift
here="$(cd "$(dirname "$0")" && pwd)"
ssh "$@" "$target" "mkdir -p /opt/halo-master /var/www/html/halo"
scp "$@" "$here/halo_master.py" "$target:/opt/halo-master/halo_master.py"
scp "$@" "$here/index.html" "$target:/var/www/html/halo/index.html"
scp "$@" "$here/halo-master.service" "$target:/etc/systemd/system/halo-master.service"
ssh "$@" "$target" "chown -R www-data:www-data /var/www/html/halo && chmod 755 /var/www/html/halo; \
if command -v ufw >/dev/null 2>&1 && ufw status | grep -q '^Status: active'; then ufw allow 27920/udp; fi; \
systemctl daemon-reload && systemctl enable halo-master >/dev/null 2>&1; systemctl restart halo-master; \
sleep 3; systemctl --no-pager --lines=5 status halo-master; echo; ls -l /var/www/html/halo; \
curl -s http://127.0.0.1/halo/status.json; echo"
