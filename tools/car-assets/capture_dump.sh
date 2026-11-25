#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
#
# Read-only ground-truth dump to a mounted USB on the owner's parked, off-road
# car. It writes only under the supplied mountpoint. The dump is local/private:
# it can contain OEM binaries, logs, credentials and personal data. Never commit
# it. Use the asset extractor for a bounded UI/runtime subset.
set -u
export LC_ALL=C
umask 077

INCLUDE_SENSITIVE=${SCOURGE_CAPTURE_INCLUDE_SENSITIVE:-0}
INCLUDE_LOGS=${SCOURGE_CAPTURE_INCLUDE_LOGS:-0}
INCLUDE_MAPS=${SCOURGE_CAPTURE_INCLUDE_MAPS:-0}
INCLUDE_NAVI2=${SCOURGE_CAPTURE_INCLUDE_NAVI2:-0}

if [ "${1:-}" = "--check-policy" ]; then
    if [ "$INCLUDE_SENSITIVE" = "1" ]; then
        echo 'sensitive: enabled (personal/raw archives permitted)'
    else
        echo 'default-skip: voicememo Data'
        echo 'default-secret-exclude: etc/shadow etc/shadow- etc/gshadow etc/gshadow-'
    fi
    exit 0
fi

if [ "${SCOURGE_CAPTURE_CONFIRM:-}" != "1" ]; then
    echo 'refusing: set SCOURGE_CAPTURE_CONFIRM=1 only after explicit owner approval' >&2
    exit 2
fi

for cmd in realpath findmnt; do
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "refusing: required command missing: $cmd" >&2
        exit 2
    }
done
if [ "${SCOURGE_CAPTURE_ALLOW_NONREMOVABLE:-0}" != "1" ]; then
    command -v lsblk >/dev/null 2>&1 || {
        echo 'refusing: lsblk missing; install it or explicitly override' >&2
        exit 2
    }
fi

MNT=${1:-}
if [ -z "$MNT" ] || [ ! -d "$MNT" ]; then
    echo "usage: $0 <mounted-usb-directory>" >&2
    exit 2
fi
MNT=$(realpath -e "$MNT" 2>/dev/null) || {
    echo "refusing: cannot resolve $MNT" >&2
    exit 2
}
case "$MNT" in
    /media/?*|/run/media/?*) ;;
    *)
        echo "refusing: dump only to a removable-media mount under /media or /run/media (got $MNT)" >&2
        exit 2
        ;;
esac
if ! grep -q " $MNT " /proc/mounts; then
    echo "refusing: $MNT is not a mounted filesystem" >&2
    exit 2
fi
SRC=$(findmnt -n -o SOURCE -T "$MNT" 2>/dev/null | head -1) || SRC=
SRC=${SRC%%\[*}
case "$SRC" in
    /dev/*) ;;
    *)
        echo "refusing: cannot identify a block device for $MNT" >&2
        exit 2
        ;;
esac
if [ "${SCOURGE_CAPTURE_ALLOW_NONREMOVABLE:-0}" != "1" ]; then
    PK=$(lsblk -no PKNAME "$SRC" 2>/dev/null | head -1) || PK=
    if [ -n "$PK" ]; then BASE=/dev/$PK; else BASE=$SRC; fi
    RM=$(lsblk -no RM "$BASE" 2>/dev/null | head -1) || RM=
    if [ "$RM" != "1" ]; then
        echo "refusing: $BASE is not reported as removable (override only if you are certain)" >&2
        exit 2
    fi
fi
OPTS=$(findmnt -n -o OPTIONS -T "$MNT" 2>/dev/null | head -1) || OPTS=
case ",$OPTS," in
    *,ro,*) echo "refusing: $MNT is mounted read-only" >&2; exit 2 ;;
esac

AVAIL_KB=$(df -k "$MNT" | awk 'NR==2 {print $4}')
if [ "${AVAIL_KB:-0}" -lt 15000000 ]; then
    echo "refusing: need at least 15 GiB free on $MNT (have ${AVAIL_KB:-0} KiB)" >&2
    exit 2
fi

OUT="$MNT/car-dump-$(date -u '+%Y%m%dT%H%M%SZ')"
mkdir -p "$OUT"/{archives,partitions,runtime,meta,critical} || exit 1
LOG="$OUT/meta/capture.log"
exec > >(tee -a "$LOG") 2>&1

echo "=== car capture started $(date -u '+%Y-%m-%dT%H:%M:%SZ') ==="
echo "mountpoint: $MNT"
echo "output: $OUT"
df -h "$MNT"

run() {
    name=$1
    shift
    echo "--- $name ---"
    "$@"
    rc=$?
    echo "--- $name rc=$rc ---"
    return 0
}

# Root filesystem only. Mounted filesystems and maps are separate.
echo "=== rootfs archive ==="
EXCLUDES=(
    --exclude=./proc --exclude=./sys --exclude=./dev --exclude=./run
    --exclude=./tmp --exclude=./media --exclude=./mnt
    --exclude=./navi --exclude=./navi2 --exclude=./rw_data
    --exclude=./log_data --exclude=./update --exclude=./vr
    --exclude=./space --exclude=./Data --exclude=./voicememo
    --exclude=./lost+found
)
if [ "$INCLUDE_SENSITIVE" != "1" ]; then
    EXCLUDES+=(
        --exclude=./etc/shadow --exclude=./etc/shadow.Global
        --exclude=./etc/shadow- --exclude=./etc/gshadow
        --exclude=./etc/gshadow- --exclude=./etc/passwd.secure
        --exclude=./etc/ssh
        --exclude=./etc/machine-id --exclude=./etc/hostname
        --exclude='./app/share/AppUpgrade/*key*'
        --exclude=./app/share/dmengine/tree.xml
        --exclude=./app/share/dmengine/config.txt
    )
fi
tar -C / --one-file-system --ignore-failed-read "${EXCLUDES[@]}" \
    -cf "$OUT/archives/rootfs.tar" . || echo "rootfs rc=$?"

for pair in \
    "/update:update" \
    "/rw_data:rw_data" \
    "/vr:vr" \
    "/space:space" \
    "/Data:Data" \
    "/voicememo:voicememo"; do
    src=${pair%%:*}
    name=${pair##*:}
    if [ "$INCLUDE_SENSITIVE" != "1" ]; then
        case "$name" in
            voicememo|Data)
                echo "=== $name archive skipped (personal data; opt in with SCOURGE_CAPTURE_INCLUDE_SENSITIVE=1) ==="
                continue
                ;;
        esac
    fi
    echo "=== $name archive ==="
    EXCL=()
    if [ "$INCLUDE_SENSITIVE" != "1" ] && [ "$name" = "rw_data" ]; then
        EXCL=(--exclude=./home --exclude=./navi_data --exclude=./AppVH
              --exclude=./mof --exclude=./wifi --exclude=./initial_root.log
              --exclude=./stage3 --exclude=./combo --exclude=./rooted_daemons
              --exclude=./var/core)
    fi
    tar -C "$src" --ignore-failed-read ${EXCL[@]+"${EXCL[@]}"} \
        -cf "$OUT/archives/$name.tar" . 2>&1 || echo "$name rc=$?"
done

if [ "$INCLUDE_LOGS" = "1" ] && [ -d /log_data ]; then
    echo "=== log_data archive ==="
    tar -C /log_data --ignore-failed-read -cf "$OUT/archives/log_data.tar" . 2>&1 ||
        echo "log_data rc=$?"
fi

if [ "$INCLUDE_NAVI2" = "1" ] && [ -d /navi2 ]; then
    echo "=== navi2 non-map archive ==="
    tar -C /navi2 --ignore-failed-read \
        --exclude=./Data --exclude=./GlobalImage --exclude=./vr \
        -cf "$OUT/archives/navi2-nonmap.tar" . 2>&1 ||
        echo "navi2 rc=$?"
fi

if [ "$INCLUDE_MAPS" = "1" ] && [ -d /navi2 ]; then
    echo "=== navi2 map archive (large) ==="
    tar -C /navi2 --ignore-failed-read \
        -cf "$OUT/archives/navi2-maps.tar" ./Data ./GlobalImage 2>&1 ||
        echo "maps rc=$?"
fi

# Small critical subset for fast local analysis. This archive can contain OEM
# binaries, logs and key paths, so it is opt-in and still excludes secret files.
if [ "$INCLUDE_SENSITIVE" = "1" ]; then
    echo "=== critical archive (sensitive opt-in) ==="
    {
        for p in \
            lib/systemd/esinit \
            etc boot lib/modules/4.1.27-abl \
            usr/lib/weston usr/lib/dri usr/lib/drinano \
            usr/bin/wcap-decode usr/bin/weston usr/bin/weston-info \
            app/bin app/lib app/plugins app/share/AppUpgrade \
            opt/ias update log_data/AppUpgrade \
            rw_data/AppUpgrade.txt rw_data/RUN_STUFF_OK.txt \
            rw_data/rooted_daemons rw_data/stage3 rw_data/combo \
            rw_data/initial_root.log rw_data/second_stage_preflight.txt \
            rw_data/applauncher_sigstub rw_data/battery rw_data/tweaks \
            rw_data/wifi1b; do
            [ -e "/$p" ] && echo "$p"
        done
    } | tar -C / --ignore-failed-read \
        --exclude='*teruten5_pub.key' --exclude='*authorized_keys*' \
        --exclude='*wifi.txt*' --exclude='*etc/shadow*' \
        --exclude='*etc/gshadow*' \
        --exclude='*dmengine/tree.xml' --exclude='*dmengine/config.txt' \
        -cf "$OUT/critical/critical.tar" -T - ||
        echo "critical rc=$?"
else
    echo "=== critical archive skipped (set SCOURGE_CAPTURE_INCLUDE_SENSITIVE=1) ==="
fi

# Raw unmounted/read-only partitions. Maps (p14/p15) are intentionally omitted.
if [ "$INCLUDE_SENSITIVE" = "1" ]; then
    for spec in \
        "mmcblk0p1:p1-boot-a.img" \
        "mmcblk0p2:p2-boot-b.img" \
        "mmcblk0p3:p3-rootfs-b.img" \
        "mmcblk0p5:p5-rootfs-a.img" \
        "mmcblk0p6:p6.img" \
        "mmcblk0p7:p7-vr.img" \
        "mmcblk0p16:p16.img" \
        "mmcblk0boot0:boot0.img" \
        "mmcblk0boot1:boot1.img"; do
        dev=${spec%%:*}
        name=${spec##*:}
        mnt=$(findmnt -n -o TARGET -S "/dev/$dev" 2>/dev/null | head -1) || mnt=
        if [ -n "$mnt" ]; then
            mopts=$(findmnt -n -o OPTIONS -S "/dev/$dev" 2>/dev/null | head -1) || mopts=
            case ",$mopts," in
                *,ro,*) ;;
                *)
                    echo "skip raw /dev/$dev: mounted read-write at $mnt"
                    continue
                    ;;
            esac
        fi
        echo "=== dd /dev/$dev -> $name ==="
        dd if="/dev/$dev" of="$OUT/partitions/$name" bs=4M 2>&1 ||
            echo "dd $dev rc=$?"
    done
else
    echo "=== raw partition images skipped (set SCOURGE_CAPTURE_INCLUDE_SENSITIVE=1) ==="
fi

# Runtime evidence.
echo "=== runtime evidence ==="
R="$OUT/runtime"
run identity sh -c 'uname -a; cat /proc/version; cat /proc/cmdline; cat /etc/version; cat /etc/os-release' > "$R/identity.txt"
cat /proc/config.gz > "$R/kernel-config.gz" 2>/dev/null || true
run mounts sh -c 'cat /proc/mounts; df -hT; df -i' > "$R/mounts.txt"
run block sh -c 'lsblk -a 2>/dev/null; blkid 2>/dev/null; sfdisk -d /dev/mmcblk0 2>/dev/null; fdisk -l /dev/mmcblk0 2>/dev/null' > "$R/block.txt"
run proc sh -c 'cat /proc/partitions /proc/devices /proc/modules /proc/interrupts /proc/iomem /proc/ioports /proc/cpuinfo /proc/meminfo /proc/swaps' > "$R/proc.txt"
run pci sh -c 'lspci -nn 2>/dev/null; lspci -vv 2>/dev/null' > "$R/lspci.txt"
run usb sh -c 'lsusb -t 2>/dev/null; lsusb -v 2>/dev/null' > "$R/lsusb.txt"
run systemd sh -c 'systemctl --version; systemctl list-unit-files --no-pager; systemctl list-units --all --no-pager; systemctl list-dependencies --all --no-pager; systemd-analyze; systemd-analyze critical-chain --no-pager; systemd-analyze blame --no-pager' > "$R/systemd.txt"
run units sh -c 'for u in esinit.service local-fs.target sysinit.target basic.target multi-user.target ApplicationLauncher.service Dbuslaunch.service dbus.service dbus-com.lge.car.micom.service MediaBrowser.service connman.service credman.service CryptoService.service AudioManager.service wifi1b-daemon.service tweaks-daemon.service battery-daemon.service initial-root-failsafe.service usb-automount.service; do echo "===== $u ====="; systemctl cat "$u" 2>&1; systemctl show "$u" --no-pager 2>&1; done' > "$R/units.txt"
run processes sh -c 'ps -eo pid,ppid,user,stat,lstart,comm,args' > "$R/processes.txt"
run dbus sh -c 'busctl list --no-pager 2>/dev/null; busctl tree --no-pager 2>/dev/null' > "$R/dbus.txt"
run network sh -c 'ip -details -statistics link show 2>/dev/null; ip addr show 2>/dev/null; ip route show 2>/dev/null; ip neigh show 2>/dev/null; cat /proc/net/dev; cat /proc/net/can/stats; cat /proc/net/can/rcvlist_all' > "$R/network.txt"
run smack sh -c 'cat /proc/self/attr/current; for f in access access2 ambient cipso cipso2 direct doi load load-self load-self2 logging mapped netlabel onlycap ptrace revoke-subject syslog unconfined; do echo "===== $f ====="; cat "/sys/fs/smackfs/$f" 2>&1; done; cat /sys/fs/smackfs/load2 2>&1; find /etc/smack -type f -print -exec sed -n "1,200p" {} \;' > "$R/smack.txt"
run xattrs sh -c 'for d in /etc /app/bin /app/lib /app/share/AppUpgrade /usr/lib/weston /usr/lib/systemd /lib/modules/4.1.27-abl /opt /usr/bin /usr/sbin /bin /sbin; do echo "===== $d ====="; getfattr -R -d -m - --absolute-names "$d" 2>/dev/null; done' > "$R/xattrs.txt"
run tree sh -c 'for d in / /app /opt /usr /etc /boot /vr /space /Data /update /log_data /rw_data /media /voicememo /whatsnew /manual; do echo "===== $d ====="; find "$d" -xdev -print 2>/dev/null; done' > "$R/tree.txt"
run sizes sh -c 'du -x -d 2 -k / /log_data /update /rw_data /vr /space /Data /voicememo /media 2>/dev/null' > "$R/sizes.txt"
dmesg > "$R/dmesg.txt" 2>&1 || true
journalctl -b --no-pager -o short-monotonic > "$R/journal.txt" 2>&1 || true
ls -la /sys/fs/pstore > "$R/pstore.txt" 2>&1 || true
find /lib/firmware -maxdepth 3 -type f > "$R/firmware.txt" 2>&1 || true

# Metadata and integrity.
echo "=== metadata ==="
{
    date -u '+%Y-%m-%dT%H:%M:%SZ'
    uname -a
    cat /etc/version 2>/dev/null || true
    echo 'excluded by default: maps; log_data unless opted in; secret paths'
    echo 'default archives skip personal rw_data dirs, /voicememo and /Data'
    echo 'critical archive and raw partitions: opt-in sensitive only'
    echo 'raw partitions (when opted in): p1 p2 p3 p5 p6 p7 p16 boot0 boot1'
    echo 'sensitive: raw partitions and optional archives may contain private data; local-only'
    echo 'navi2 non-map archive only when opted in'
} > "$OUT/meta/SOURCE.txt"
# Metadata files must be final before hashing.
( cd "$OUT" && find . -type f ! -name 'SHA256SUMS*' ! -name capture.log \
    -exec stat -c '%s %n' {} \; ) > "$OUT/meta/sizes.txt" 2>/dev/null
( cd "$OUT" && ls -lR ) > "$OUT/meta/files.txt" 2>/dev/null
sync
( cd "$OUT" && find . -type f ! -name 'SHA256SUMS*' ! -name capture.log \
    -exec sha256sum {} \; ) > "$OUT/meta/SHA256SUMS" 2>/dev/null
sync
echo "=== car capture finished $(date -u '+%Y-%m-%dT%H:%M:%SZ') ==="
echo "result: $OUT"
