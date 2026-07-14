#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run this uninstaller as root." >&2
    exit 1
fi

rm -f /usr/local/bin/flitz-extractor /usr/local/bin/flitz-compressor
rm -f /usr/share/applications/flitz-extractor.desktop
rm -f /usr/share/applications/flitz-compressor.desktop
rm -rf /usr/local/flitz
rm -rf /usr/share/doc/flitz-toolkit
for lang in ar ca de es fr hu it ja pt ru zh; do
    rm -f "/usr/share/locale/$lang/LC_MESSAGES/flitz-toolkit.mo"
done

command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true

printf '%s\n' "Flitz removed."
