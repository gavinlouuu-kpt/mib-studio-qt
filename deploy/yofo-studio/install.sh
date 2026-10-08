#!/bin/sh
# Install the YOFO Studio package on the PZ7035 (run as root on the board, from the package dir).
#   ./install.sh [--restart]
# The RAM root is rebuilt on every boot: run this after each boot, with the producer
# (/usr/lib/genicam/libpz7035_gentl.cti) and /etc/yofo/expected-core.json already in place.
# No token is needed: the unit listens on 127.0.0.1:8427 only (SSH tunnel from the host).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
md5sum -c MD5SUMS >/dev/null || { echo "install: MD5SUMS check failed" >&2; exit 1; }
install -d /usr/share/yofo-studio /usr/libexec/yofo-studio /var/lib/yofo-studio
install -m 0755 yofo-studio-server /usr/bin/yofo-studio-server
rm -rf /usr/share/yofo-studio/dist
tar -C /usr/share/yofo-studio -xf dist.tar
install -m 0755 pl-ready.sh /usr/libexec/yofo-studio/pl-ready
install -m 0644 yofo-studio.service /etc/systemd/system/yofo-studio.service
systemctl daemon-reload
echo "installed $(head -n 1 BUILD_INFO)"
if [ "${1:-}" = "--restart" ]; then
    systemctl restart yofo-studio
    echo "yofo-studio restarted"
fi
