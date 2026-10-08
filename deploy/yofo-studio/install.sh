#!/bin/sh
# Install the YOFO Studio package on the PZ7035 (run as root on the board, from the package dir).
#   ./install.sh [--restart]
# The RAM root is rebuilt on every boot: run this after each boot, with the producer
# (/usr/lib/genicam/libpz7035_gentl.cti) and /etc/yofo/expected-core.json already in place.
# No token is needed: the unit listens on 127.0.0.1:8427 only (SSH tunnel from the host).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
# MD5SUMS lists the whole bundle; the host/ part (PL image, firmware, Linux) stays on the PC, so only
# the board side is checked here.
grep -v '  host/' MD5SUMS | md5sum -c >/dev/null || { echo "install: MD5SUMS check failed" >&2; exit 1; }
install -d /usr/share/yofo-studio /usr/libexec/yofo-studio /var/lib/yofo-studio
install -m 0755 yofo-studio-server /usr/bin/yofo-studio-server
rm -rf /usr/share/yofo-studio/dist
tar -C /usr/share/yofo-studio -xf dist.tar
install -m 0755 pl-ready.sh /usr/libexec/yofo-studio/pl-ready
install -m 0644 yofo-studio.service /etc/systemd/system/yofo-studio.service
# A bundle carries its producer and the identity of its PL: install them so the pair is matched.
if [ -f producer/libpz7035_gentl.cti ]; then
    install -d -m 755 /usr/lib/genicam
    install -m 0644 producer/libpz7035_gentl.cti /usr/lib/genicam/libpz7035_gentl.cti
fi
if [ -f core.json ]; then
    install -D -m 0644 core.json /etc/yofo/expected-core.json
fi
systemctl daemon-reload
echo "installed $(head -n 1 BUILD_INFO)"
if [ "${1:-}" = "--restart" ]; then
    systemctl restart yofo-studio
    echo "yofo-studio restarted"
fi
