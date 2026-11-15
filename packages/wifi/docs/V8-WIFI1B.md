# V8 combined Wifi1b bundle

Status: **offline-tested; on-car validation pending.** No external review,
commit or deployment. V7 and legacy Wifi1b releases are preserved.

The flat bundle now installs Battery → WiFi → Tweaks/Reboot → Wifi1b.
Restore is the reverse, and stops on the first failure. Owner credentials
are forwarded privately to both WiFi apps; Wifi1b import stays non-activating.
Wifi1b retains primary and secondary client profiles; the static supplicant
receives descending priorities (primary first, then backup networks).

## Conforming changes

- Wifi1b sources and first-party UI/build inputs live in packages/wifi1b;
  no OEM base trees or executables were imported into tracked source.
- The additive patcher counts Reboot: Battery/WiFi/Tweaks/Reboot/Wifi1b use
  ordinals 0–4, root-owned reactive bindings and null/undefined guards. It
  preserves the existing allocator instead of replacing V7's snapshot.
- Wide home appends after Reboot and fits all nine shortcuts in its strip;
  the All Menus allocator also counts Reboot. No layout database is reset.
- The build runs the packaged patcher against packaged final Tweaks QML,
  requires five unique tiles, checks idempotency, and executes real Qt Quick
  placement/click tests at both widths. Vendor model/theme services are mocks.
- Wifi1b accepts only manifest-covered current canonical QML hashes. It
  patches private copies then uses V7's verified inode-preserving I/O. Root
  state restoration, shell mount parsing, optional icons, rollback, current
  stage results and QML snapshots follow the combined installer conventions.
- Both radio daemons use the same kernel flock plus boot-local owner record.
  Workers hold the lock through mutations; the record reserves wlan0 between
  workers and survives process crashes. Opposite activation/scan/SSH requests
  fail closed. Idle/off/shutdown does not clean up another app's radio.
  Successful owner-Off releases; failed cleanup keeps the reservation.
- Watchdog eviction is serialized through the guarded worker. Instance locks
  exclude duplicate manual/systemd daemons; systemd remains preferred.
- An active legacy Wifi1b blocks a combined install before package stages.
  Turn it Off first. Restore WiFi refuses while Wifi1b remains installed.

## Operation and validation limits

Turn the active app Off, wait for its cleanup, then start the other. If both
have pending activation requests, the first eligible requester can acquire
the freed radio; cancel unwanted requests with Off before switching. Never
delete the live owner file to force a switch. After a daemon crash, restart
that owning app and request Off to clean up; a reboot clears boot-local
reservations. Do not run legacy radio daemons or Wifi2 beside these versions.

Host tests cover real concurrent locks, retained crash reservations, both
daemon entry guards, DHCP server/client fixtures, credentials/persistence,
overlay input, additive installation/restoration and combo dispatch. V7's
shell I/O primitives were previously vendor-kernel-VM tested. The newly
integrated RF paths, vehicle SMACK, vendor Qt 5.4 rendering/model behavior,
wireless CarPlay/iPhone pairing and recovery reboot remain car-pending.
The mdnsd uid-retention experiment remains disarmed. Battery controls and
owner-approved Reboot behavior are unchanged from V7.
