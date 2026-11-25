#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
#
# Read-only runtime inventory for the owner's parked, off-road head unit.
# Run over SSH with: ssh <car-host> 'SCOURGE_CAR_CONFIRM=1 bash -s' < this.sh
# It writes nothing on the car and prints metadata only. Output may contain
# device identifiers, filenames and private log details; review before sharing.
set -u
export LC_ALL=C

if [ "${SCOURGE_CAR_CONFIRM:-}" != "1" ]; then
    echo 'refusing: set SCOURGE_CAR_CONFIRM=1 only after explicit owner approval' >&2
    exit 2
fi

status=0
section() { echo; echo "===== $* ====="; }

section identity
uname -a 2>&1 || status=1
cat /proc/version 2>&1 || status=1
cat /proc/cmdline 2>&1 || status=1
cat /etc/version 2>/dev/null || true
cat /etc/os-release 2>/dev/null || true
cat /etc/hostname 2>/dev/null || true
cat /etc/machine-id 2>/dev/null || true

section mounts
cat /proc/mounts 2>&1 || status=1
df -hT 2>&1 || status=1
df -i 2>&1 || status=1

section block-devices
if command -v lsblk >/dev/null 2>&1; then
    lsblk -a -o NAME,MAJ:MIN,RM,SIZE,RO,TYPE,FSTYPE,MOUNTPOINT,LABEL,UUID 2>&1 || status=1
fi
if command -v blkid >/dev/null 2>&1; then
    blkid 2>&1 || status=1
fi
if command -v sfdisk >/dev/null 2>&1; then
    sfdisk -d /dev/mmcblk0 2>&1 || status=1
fi
if command -v fdisk >/dev/null 2>&1; then
    fdisk -l /dev/mmcblk0 2>&1 | head -80 || status=1
fi

section filesystems
cat /proc/filesystems 2>&1 || status=1

section partitions
cat /proc/partitions 2>&1 || status=1
for p in /dev/mmcblk0p3 /dev/mmcblk0p5 /dev/mmcblk0p6 /dev/mmcblk0p7 \
         /dev/mmcblk0p8 /dev/mmcblk0p9 /dev/mmcblk0p10 /dev/mmcblk0p11 \
         /dev/mmcblk0p12 /dev/mmcblk0p13 /dev/mmcblk0p17; do
    section "tune2fs $p"
    tune2fs -l "$p" 2>&1 | grep -E 'Filesystem volume name|Filesystem UUID|Filesystem features|Block size|Inode size|Journal|Filesystem created|Last mount time|Last write time|Lifetime writes|Mount count|Maximum mount count|Reserved block count|Free blocks|Free inodes' || status=1
done

section systemd
systemctl --version 2>&1 || status=1
systemctl list-unit-files --no-pager 2>&1 || status=1
systemctl list-units --all --no-pager 2>&1 || status=1
systemctl list-dependencies --all --no-pager 2>&1 || status=1
systemd-analyze 2>&1 || status=1
systemd-analyze critical-chain --no-pager 2>&1 || status=1
systemd-analyze blame --no-pager 2>&1 || status=1

section key-units
for u in esinit.service local-fs.target sysinit.target basic.target multi-user.target \
         ApplicationLauncher.service Dbuslaunch.service dbus.service \
         dbus-com.lge.car.micom.service MediaBrowser.service connman.service \
         credman.service CryptoService.service AudioManager.service \
         usb-automount.service; do
    section "systemctl cat $u"
    systemctl cat "$u" 2>&1 || status=1
    section "systemctl show $u"
    systemctl show "$u" --no-pager 2>&1 || status=1
done

section processes
ps -eo pid,ppid,user,stat,lstart,comm,args 2>&1 || status=1

section dbus
if command -v busctl >/dev/null 2>&1; then
    busctl list --no-pager 2>&1 || status=1
    busctl tree --no-pager 2>&1 | head -400 || status=1
fi

section network
if command -v ip >/dev/null 2>&1; then
    ip -details -statistics link show 2>&1 || status=1
    ip addr show 2>&1 || status=1
    ip route show 2>&1 || status=1
    ip neigh show 2>&1 || status=1
fi
cat /proc/net/dev 2>&1 || status=1
cat /proc/net/can/stats 2>&1 || status=1
cat /proc/net/can/rcvlist_all 2>&1 || status=1

section smack
cat /proc/self/attr/current 2>&1 || status=1
for f in access access2 ambient cipso cipso2 direct doi load load-self load-self2 \
         logging mapped netlabel onlycap ptrace revoke-subject syslog unconfined; do
    section "smackfs/$f"
    cat "/sys/fs/smackfs/$f" 2>&1 | head -500 || status=1
done
cat /sys/fs/smackfs/load2 2>&1 || status=1
find /etc/smack -maxdepth 3 -type f -print -exec sed -n '1,200p' {} \; 2>&1 || status=1

section kernel-security
cat /proc/sys/kernel/kptr_restrict 2>&1 || status=1
cat /proc/sys/kernel/dmesg_restrict 2>&1 || status=1
cat /proc/sys/kernel/modules_disabled 2>&1 || status=1
cat /proc/sys/kernel/panic_on_oops 2>&1 || status=1
cat /proc/sys/kernel/randomize_va_space 2>&1 || status=1
cat /sys/kernel/security/lsm 2>&1 || status=1
cat /proc/modules 2>&1 || status=1

section cpu-memory
cat /proc/cpuinfo 2>&1 || status=1
cat /proc/meminfo 2>&1 || status=1
cat /proc/swaps 2>&1 || status=1

section pci-usb
if command -v lspci >/dev/null 2>&1; then
    lspci -nn 2>&1 || status=1
fi
if command -v lsusb >/dev/null 2>&1; then
    lsusb -t 2>&1 || status=1
fi

section drm-audio
ls -la /sys/class/drm 2>&1 || status=1
cat /sys/class/drm/version 2>&1 || true
cat /proc/asound/cards 2>&1 || status=1
cat /proc/asound/devices 2>&1 || status=1

section firmware
find /lib/firmware -maxdepth 3 -type f -printf '%p %s\n' 2>&1 | head -500 || status=1

section rootfs-top
ls -la / 2>&1 || status=1
for d in /app /opt /usr /etc /boot /vr /space /Data /update /log_data \
         /rw_data /media /voicememo /whatsnew /manual; do
    section "top $d"
    ls -la "$d" 2>&1 || status=1
done

section package-db
for f in /var/lib/opkg/status /var/lib/dpkg/status /usr/lib/opkg/status; do
    section "$f"
    [ -f "$f" ] && head -80 "$f" 2>&1 || true
done

section hashes
sha256sum /lib/systemd/esinit /boot/bzImage-4.1.27-abl /proc/config.gz \
    /app/bin/AppUpgrade /app/bin/dmengine /app/bin/install.sh \
    /usr/bin/wcap-decode 2>&1 || status=1

exit "$status"
