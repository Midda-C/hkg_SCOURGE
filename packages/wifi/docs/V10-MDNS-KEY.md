# V10 — mDNS firewall hook and on-screen AP key

Status: **offline-tested; on-car validation pending.**

## Why mDNS was stopped (from the V9 on-car readback)

The V9 Wifi1b CarPlay AP came up for the first time on-car (2.4 GHz after the
5 GHz fallback), with the CarPlay IE and iAP2 service active — but the WiFi
1b page showed "mDNS Stopped, Ready: no" and the iPhone never offered
wireless CarPlay. The readback showed `carplay_mdnsd.service` in a
systemd hold-off restart loop and the car's iptables dump showed the reason:
Wifi1b's `WIFI1B_APP_CP` chain was hooked only into `INPUT -i wlan0`. The
vendor `HARDEN_LOUT` chain drops outbound multicast unless a rule accepts it
first, and the stock rules only allow it on `ncm0` (USB CarPlay). mdnsd's
multicast send on wlan0 was therefore rejected (EPERM), it exited, and
systemd restarted it forever — so the `_carplay._tcp` advertisement never
reached the phone. The WiFi app already hooked its chain into
`OUTPUT -o wlan0` (its comment documents the same vendor behavior); Wifi1b
never did. This is the same firewall-rejection mechanism seen in the
vendor-kernel VM, not the disarmed uid-retention experiment.

## Changes

- Wifi1b `fw_chain_ensure()`/`fw_chain_close()` now install and remove the
  `OUTPUT -o wlan0` hook (iptables and ip6tables) in addition to INPUT, with
  the same CarPlay/mDNS/AirPlay port set as the WiFi app. The installer's
  restore path removes the OUTPUT jumps as well. The daemon test asserts
  both hooks and the restore cleanup.
- Both app pages now show the CarPlay AP **key on the landing Status tab**
  (the "Access point" card), not only inside the CarPlay AP tab. The owner
  needs it to join the AP from a laptop and reach SSH; the iPhone joins via
  the Bluetooth/iAP2 handshake and does not need it typed.

## Limits

The firewall change is verified offline (source/test gates, Qt page tests)
and the packaged bundle is verified byte-for-byte. Whether mdnsd stabilizes
and the iPhone receives the pairing prompt is car-only and remains pending.
If mDNS still loops after V10, capture
`journalctl -u carplay_mdnsd -n 50 --no-pager` while the AP is up; the next
step then depends on the mdnsd exit reason rather than the firewall.
