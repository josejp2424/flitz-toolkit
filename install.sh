#!/bin/sh
set -eu
cd "$(dirname "$0")"

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run this installer as root." >&2
    exit 1
fi

make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
make PREFIX=/usr/local install

command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true

printf '%s\n' "Flitz Toolkit 6.0 installed in /usr/local/flitz"
printf '%s\n' "ROX launcher preserved: /usr/local/flitz/flitz-extractor"
