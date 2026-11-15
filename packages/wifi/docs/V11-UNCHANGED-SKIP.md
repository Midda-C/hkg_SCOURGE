# V11 — unchanged-package skip for fast iteration

Status: **offline-tested; on-car validation pending.**

Reinstalling the whole combo for a one-app change meant re-copying Tweaks'
665 files and restarting every daemon and the UI, then rebooting. V11 makes
the master skip any stage whose packaged bytes and installed state are
genuinely unchanged.

## Rule (MODE=all only)

A stage is skipped only when **all** hold:

1. the stage digest (md5 over its `manifest.md5` + `stage.sh`) equals the
   digest recorded after that stage's last successful install;
2. every curated destination in the stage's `skip.map` still md5-matches
   the packaged payload file, required paths exist (`!path`), and
   conditional destinations (`?path`, used for the standby-clock trees)
   still exist where the installer would write them;
3. every unit in `skip.services` is active;
4. the shared home-screen QML (Advanced/Wide/AllMenus/Setup/custom list/
   Q_ThemeImage) is byte-identical to the last full-stack run's recorded
   output.

The ledger lives in `/rw_data/combo/installed/`; a firmware update or a
successful restore invalidates it, and a corrupted/missing installed file or
a stopped service forces that stage (and any QML-dependent stage after it)
to reinstall. An explicit `MODE=<package>` always reinstalls — the skip only
applies to the wrapper's default `MODE=all`.

## What the maps cover

- Battery: daemon, unit, BatteryPage/BatterySwitch, icons (BatteryTile is
  excluded because the WiFi stage installs the last copy).
- WiFi: daemon, dropbear, unit, WiFi page/tiles, Hello/Battery tile
  refreshes, icons. `mdnsd.rooted` is excluded because the disarmed
  experiment would not install it.
- Tweaks: daemons, wrapper, unit, Tweaks/Reboot pages and components,
  icons, standby-clock files, tzdata version/zones markers (the 598
  zoneinfo files are installed atomically and validated by the version).
- Wifi1b: daemon, patcher, dropbear, unit, page/components, icons.

Builds gate every map against the staged manifests and packaged units
(`common/check_skip_maps.py`), so a typo cannot silently weaken the check.
The combo test covers: full install, all-skip re-run, single corrupted
file, changed payload (QML layers after it rebuild), stopped service,
restore invalidation, and the existing failure/validation paths.

## Limits

The skip decision is byte-based only; it deliberately ignores runtime
settings. First install after V11 still installs everything (empty ledger),
and any bundle where a package's payload or installer changed rebuilds the
affected QML layers. Whether the car accepts a skipped stage (e.g. services
remain healthy across reboots) is car-pending like the rest of the stack.
