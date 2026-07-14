#!/bin/sh
set -eu

cd "$(dirname "$0")"

PACKAGE="flitz-toolkit"
VERSION="6.0-2"
ARCH="$(dpkg --print-architecture 2>/dev/null || uname -m)"
case "$ARCH" in
    x86_64) ARCH=amd64 ;;
    aarch64) ARCH=arm64 ;;
    i?86) ARCH=i386 ;;
esac

printf '%s\n' '=============================================='
printf '%s\n' ' Flitz C - compilation and DEB packaging'
printf '%s\n' '=============================================='

make clean
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"

ROOT="package-root"
rm -rf "$ROOT"
make DESTDIR="$PWD/$ROOT" PREFIX=/usr/local install

mkdir -p "$ROOT/DEBIAN"
INSTALLED_SIZE="$(du -sk "$ROOT" | awk '{print $1}')"
cat > "$ROOT/DEBIAN/control" <<CONTROL
Package: $PACKAGE
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Installed-Size: $INSTALLED_SIZE
Maintainer: josejp2424 <puppylinuxjosejp2424@gmail.com>
Depends: libgtk-3-0, tar
Recommends: zip, unzip, p7zip-full, squashfs-tools, zstd, binutils, cpio
Suggests: unrar, unar, cabextract, rpm2cpio, dmg2img, innoextract, msitools
Homepage: https://sourceforge.net/projects/essora/
Description: Native GTK3 archive compressor and extractor
 Flitz compresses folders as tar.gz, tar.xz, ZIP, 7z and SquashFS.
 It extracts common archives and special package/image formats including
 PET, DEB, RPM, AppImage, SFS, ISO, DMG, MSI and CAB.
 This C version does not require Python, Tkinter, Pillow or tkinterdnd2.
CONTROL

cat > "$ROOT/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
exit 0
POSTINST
chmod 0755 "$ROOT/DEBIAN/postinst"

cat > "$ROOT/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
exit 0
POSTRM
chmod 0755 "$ROOT/DEBIAN/postrm"

if command -v fakeroot >/dev/null 2>&1; then
    fakeroot dpkg-deb --build "$ROOT" "${PACKAGE}_${VERSION}_${ARCH}.deb"
else
    dpkg-deb --build "$ROOT" "${PACKAGE}_${VERSION}_${ARCH}.deb"
fi

printf '\nCreated: %s\n' "$PWD/${PACKAGE}_${VERSION}_${ARCH}.deb"
printf '%s\n' 'ROX compatibility preserved:'
printf '%s\n' '  /usr/local/flitz/flitz-extractor -> /usr/local/flitz/flitz-extractor.desktop'
