# Wifi1b

Wifi1b ("WiFi build B") is a standalone or side-by-side CarPlay AP/client app
for the Ioniq 5 Gen5W head unit. It replaces the ConnMan backend with the
field-proven public static recipe (static `wpa_supplicant` AP/client, own
DHCP server **and client**, key-only dropbear on 2224) and hardens the
installer to address the observed install failures.

This integrated version is **offline-tested, not car-validated**. It is work
in progress: do not present it as a verified fix and do not deploy it to a
vehicle without explicit owner authorization.

- Works standalone on the canonical Battery/Tweaks/Reboot base; no WiFi app
  reinstall and no migration stage
- Also works beside an existing WiFi app and does not modify WiFi's page,
  tile, service, config or status
- Separate paths: `/rw_data/wifi1b`
- Separate service: `wifi1b-daemon.service`
- Separate HTTP port: `8093`
- Separate SSH: `wifi1b@<ip> -p 2224` (UID 0, key-only, bind-all)
- Separate QML: `Wifi1bPage` / `Wifi1bTile` (additive patcher preserves WiFi,
  Battery, Tweaks and Reboot)
- Installer preflight stops only its own radio daemons; no manual WiFi Off
  step is needed
- Installer: no SMACK readback gate; verified inode-preserving I/O,
  non-fatal import, systemd preferred with single-instance fallback
- Daemon starts idle; importing credentials never activates anything
- Restore puts back the original shared QML and the original
  service file/enabled/active state from the install transaction

The `wifi1b` account is a deliberate product default with UID 0, not a hidden
credential. Owner keys already present in `/rw_data/wifi1b` are preserved
unless a replacement `authorized_keys` is supplied. Treat the stick and its
backups as private.

This is the canonical integration, not the legacy Wifi1b combo wrapper.
See [V8 integration](../wifi/docs/V8-WIFI1B.md) for validation and limitations.

Build and verify:

```sh
make test                 # offline tests; set SCOURGE_DROPBEAR first
sh ../build_flat.sh       # standalone Battery/Tweaks/Reboot/Wifi1b bundle
```

The default `build_flat.sh` bundle is standalone: it does **not** reinstall
WiFi and has **no** migration stage. Existing WiFi-bearing release bundles are
preserved separately.

Artifacts:

- `out/wifi1b_usb.zip`
- `out/usb/` (standalone payload; requires the current canonical
  Battery/Tweaks or Battery/Tweaks/WiFi shared QML)
- Repository `out/appcombo-flat/` (standalone three-file bundle; preferred)
