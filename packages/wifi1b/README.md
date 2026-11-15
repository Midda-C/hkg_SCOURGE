# Wifi1b

Wifi1b ("WiFi build B") is a side-by-side companion to the existing WiFi app
for the Ioniq 5 Gen5W head unit. It replaces the ConnMan backend with the
field-proven public static recipe (static `wpa_supplicant` AP/client, own
DHCP server **and client**, key-only dropbear on 2224) and hardens the
installer to address the observed install failures. This integrated version
is offline-tested, not car-validated.

- Separate paths: `/rw_data/wifi1b`
- Separate service: `wifi1b-daemon.service`
- Separate HTTP port: `8093`
- Separate SSH: `wifi1b@<ip> -p 2224` (key-only, bind-all)
- Separate QML: `Wifi1bPage` / `Wifi1bTile` (additive patcher preserves WiFi,
  Battery, Tweaks and Reboot)
- Does not modify WiFi's page, tile, service, config or status
- Installer: no SMACK readback gate; verified inode-preserving I/O,
  non-fatal import, systemd preferred with single-instance fallback
- Daemon starts idle; importing credentials never activates anything

This is the canonical integration, not the legacy Wifi1b combo wrapper.
See [V8 integration](../wifi/docs/V8-WIFI1B.md) for validation and limitations.

Build and verify:

```sh
make test                 # offline tests; set SCOURGE_DROPBEAR first
sh ../build_flat.sh       # full canonical Battery/WiFi/Tweaks/Wifi1b bundle
```

Artifacts:

- `out/wifi1b_usb.zip`
- `out/usb/` (standalone; requires current canonical WiFi QML)
- Repository `out/appcombo-flat/` (full two-file bundle; preferred)
