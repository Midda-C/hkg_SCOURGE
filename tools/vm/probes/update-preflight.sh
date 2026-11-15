#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Read-only update-media preflight in the vendor rootfs. Validates the real
# package map, manifest and payload header without launching AppUpgrade,
# decrypting the package or staging an update.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

fail() {
    echo "HARNESS_FAIL $1"
    exit 1
}

modprobe ehci-pci 2>/dev/null || true
modprobe usb-storage 2>/dev/null || true
modprobe sd_mod 2>/dev/null || true
sleep 2
mkdir -p /media/usb
MEDIA=
for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o ro "$dev" /media/usb 2>/dev/null || continue
    if [ -f /media/usb/IONIQ5_EU.ver ] \
        && [ -f /media/usb/MANIFEST.sha256 ] \
        && [ -f /media/usb/.lge.upgrade.xml ]; then
        MEDIA=/media/usb
        break
    fi
    umount /media/usb 2>/dev/null || true
done
[ -n "$MEDIA" ] || { echo 'HARNESS_BLOCKED update-preflight stick missing'; exit 77; }
trap 'cd /; umount "$MEDIA" 2>/dev/null || true' EXIT

cd "$MEDIA" || fail 'preflight-cd'
[ -s IONIQ5_EU.ver ] || fail 'preflight-ver-empty'
[ -s MANIFEST.sha256 ] || fail 'preflight-manifest-empty'
[ -s .lge.upgrade.xml ] || fail 'preflight-package-empty'
# The package blob is encrypted/binary, not plain XML.
FIRST=$(od -An -tx1 -N1 .lge.upgrade.xml | tr -d ' \n')
[ "$FIRST" != "3c" ] || fail 'preflight-package-plaintext'
grep -q "IONIQ5_EU.ver" MANIFEST.sha256 || fail 'preflight-manifest-ver'
echo 'HARNESS_CHECK preflight-media PASS'

tr -d '\r' < IONIQ5_EU.ver > /tmp/preflight.ver || fail 'preflight-ver-copy'
HEADER=$(head -n 1 /tmp/preflight.ver)
case "$HEADER" in
    +\|*) ;;
    *) fail 'preflight-ver-header' ;;
esac
[ "$(printf '%s' "$HEADER" | awk -F'|' '{print NF}')" = 7 ] || fail 'preflight-ver-header-fields'
PRODUCT=$(printf '%s' "$HEADER" | awk -F'|' '{print $5}')
[ -n "$PRODUCT" ] || fail 'preflight-ver-product'
if ! tail -n +2 /tmp/preflight.ver | awk -F'|' 'NF != 6 {exit 1}'; then
    fail 'preflight-ver-entry-fields'
fi

tail -n +2 /tmp/preflight.ver > /tmp/preflight.entries
ENTRIES=0
PRESENT=0
while IFS='|' read -r path filename ident check size flag; do
    [ -n "$path" ] || fail 'preflight-ver-empty-path'
    case "$path" in
        *..*|/*) fail "preflight-ver-unsafe-path $path" ;;
        "$PRODUCT"|"$PRODUCT"\\*) ;;
        *) fail "preflight-ver-product-mismatch $path" ;;
    esac
    case "$filename" in
        ''|.|..|*/*|*\\*) fail "preflight-ver-unsafe-name $filename" ;;
    esac
    case "$size" in
        ''|*[!0-9]*) fail "preflight-ver-size $filename" ;;
    esac
    ENTRIES=$((ENTRIES + 1))
    rel=${path#"$PRODUCT"}
    rel=${rel#\\}
    rel=${rel//\\//}
    if [ -n "$rel" ]; then
        full="$rel/$filename"
    else
        full="$filename"
    fi
    if [ -f "$full" ]; then
        actual=$(wc -c < "$full" | tr -d ' ')
        [ "$actual" = "$size" ] \
            || fail "preflight-ver-size-mismatch $full declared=$size actual=$actual"
        PRESENT=$((PRESENT + 1))
    fi
done < /tmp/preflight.entries
[ "$ENTRIES" -gt 0 ] || fail 'preflight-ver-no-entries'
[ "$PRESENT" -gt 0 ] || fail 'preflight-ver-no-present-files'
echo 'HARNESS_CHECK preflight-ver PASS'

tr -d '\r' < MANIFEST.sha256 > /tmp/preflight.manifest || fail 'preflight-manifest-copy'
if ! awk 'NF != 2 || length($1) != 64 || $1 !~ /^[0-9a-f]+$/ {exit 1}' \
    /tmp/preflight.manifest; then
    fail 'preflight-manifest-format'
fi
if ! /bin/busybox sha256sum -c /tmp/preflight.manifest >/tmp/preflight-hashes.log 2>&1; then
    tail -20 /tmp/preflight-hashes.log
    fail 'preflight-manifest-hash'
fi
echo 'HARNESS_CHECK preflight-manifest PASS'

PAYLOAD=HU/images/mango-rootfs.tar.gz
[ -s "$PAYLOAD" ] || fail 'preflight-payload-missing'
# The real package payload is encrypted/opaque despite the .tar.gz name.
HEAD=$(od -An -tx1 -N2 "$PAYLOAD" | tr -d ' \n')
[ "$HEAD" != "1f8b" ] || fail 'preflight-payload-plain-gzip'
USTAR=$(od -An -tx1 -j 257 -N 5 "$PAYLOAD" | tr -d ' \n')
[ "$USTAR" != "7573746172" ] || fail 'preflight-payload-plain-tar'
echo 'HARNESS_CHECK preflight-payload PASS'

# Nothing may have launched the updater or staged an update.
if ps -ef | grep -E 'AppUpgrade|dmengine' | grep -v grep >/tmp/preflight-processes.log 2>&1; then
    cat /tmp/preflight-processes.log
    fail 'preflight-updater-running'
fi
[ ! -e /tmp/AppUpgrade ] || fail 'preflight-updater-staged'
echo 'HARNESS_CHECK preflight-no-exec PASS'
