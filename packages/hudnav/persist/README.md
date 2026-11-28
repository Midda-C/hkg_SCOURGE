# Persistent Android Auto -> HUD (installed on the car 2026-11-26 11:55 UTC)

Status: car-validated for one full reboot (boot guard "boot confirmed", HUD arrow + distance working) and
one earlier session-mode drive. Roundabouts / slight / sharp / ramps not yet confirmed.

## Pieces
| Unit (`/etc/systemd/system`) | Script | Role |
|---|---|---|
| `hudnav-storeguard.service` | `/rw_data/hudnav-storeguard/hudnav-storeguard.sh check` | before pas-daemon: restore the shared settings index from the saved good copy if missing/damaged (the 2026-11-26 incident) |
| `hudnav-bootguard.service` | `/rw_data/hudnav-bootguard/hudnav-bootguard.sh early` | count unconfirmed UI boots; two in a row -> rollback (kit renamed, preload/launcher/carplay reverted) |
| `hudnav-bootok.service` | same script, `ok` | confirm a UI boot healthy (pids stable, no crashes, index ok); background wakes not counted |
| `hudnav-aa.service` | `/rw_data/hudnav/bin/hudnav-aa-boot.sh start` | start hudnavd (tx), then the session overlay (bind-mounted DT_NEEDED AppProjection copy) |

Kit: `/rw_data/hudnav/bin` (SHA256SUMS covers AppProjection.aa, aa_shim.so, hudnavd, hudnav-run.sh,
micom-send, micom-send-tp, CANService, aa-session.sh, hudnav-aa-boot.sh). Switches: `/rw_data/hudnav/enabled`
(master), `/rw_data/hudnav/aa-nav-enable` (shim nav feature).

## Install / undo
- Install: `sh install-persist.sh` (report) then `--yes`, from the kit folder. Tests: `tests/test_persist.sh`.
- Disable without removing: `rm /rw_data/hudnav/enabled` (next boot is stock).
- Remove: `sh /rw_data/hudnav/bin/uninstall-persist.sh --yes`.
- No SSH: the v4 USB recovery stick renames the kit (feature off next boot); the boot guard does the same
  by itself after two failed UI boots.

## Known issue (fix pending)
The overlay restarts AppProjection ~10 s into boot, while the user-agreement screen is up: the unit shows
"not available on user agreement screen" and the owner has to press the AA button. Fix: do the bind mount
before ApplicationLauncher starts (no restart), daemon stage after the session bus.
