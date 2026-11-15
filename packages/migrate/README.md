# migrate — remove WiFi, keep Battery/Tweaks and Wifi1b

One-way, hash-gated migration for a head unit that already has Battery,
Tweaks, WiFi and Wifi1b installed.

It:

1. verifies the current shared QML and the rebuilt Wifi1b target against the
   shipped accepted-QML ledger;
2. stops both WiFi apps and requires both radios to report `mode=off` and
   `wlan_on=false`;
3. reverts WiFi-created files through WiFi's own transaction ledger, so
   pre-existing owner files are restored and only WiFi-only files are removed;
4. restores stock `mdnsd` if the experimental root patch was armed, removes the
   WiFi `carplay_mdnsd` drop-in, and restores `supplicantmanager` to its
   pre-WiFi mask state (a pre-existing mask is preserved);
5. rebuilds the home screen from the canonical no-WiFi Battery/Tweaks base
   shipped in this payload plus the packaged Wifi1b patcher. WiFi's own restore
   point is not used for QML because it predates Tweaks in the V11 order;
6. archives `/rw_data/wifi` to the stick before deleting it;
7. rebases `/rw_data/wifi1b/backup/latest` onto the no-WiFi base so a later
   Wifi1b restore does not resurrect WiFi QML.

Wifi1b configuration, credentials, SSH keys and backups under
`/rw_data/wifi1b` are preserved.

Restore is intentionally a no-op. To put WiFi back, use the original
WiFi-bearing bundle and install WiFi before Wifi1b.

The archived `wifi-pre-migration-*.tar.gz` contains WiFi configuration and
credentials. Treat the stick as private.
