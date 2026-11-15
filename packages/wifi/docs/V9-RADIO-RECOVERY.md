# V9 radio recovery — fixing the stuck "Radio busy/reserved" reservation

Status: **offline-tested; on-car validation pending.** Reported on-car with V8:
Wifi1b showed "Radio busy/reserved" while the WiFi app was in Off mode.

## Diagnosis

The V8 contract released the radio only on *verified* owner-Off. The WiFi
app's verification demanded an AP-absent probe plus a ConnMan power state;
both are unprovable on an already-downed interface, so a genuine Off could
never be confirmed. The UI showed Off (the config was saved), the cleanup
message said "recovery will retry", but the watchdog only retried when it
observed leftover processes — with none observed, the reservation stuck
forever. Fail-closed did its job (never two owners), but with no healing
path the peer stayed blocked.

## Why not "just kill processes"

Killing the peer's daemon or supplicant from the blocked app orphans the
peer's ConnMan tether, firewall chains and AP state, and the peer's
watchdog (Restart=always, 3 s) immediately re-applies and fights back —
the exact radio war this contract exists to prevent. The fix keeps one
mutating owner at all times, but removes every path where a *stale*
reservation could persist.

## Changes (both radio daemons share `common/radio_owner.h`)

- **Off is now verifiable**: an interface that is absent or
  administratively down (IFF_UP clear in `/sys/class/net/wlan0/flags`),
  with no supplicant/SSH/DHCP alive, is off. No AP probe or ConnMan power
  state required. A real Off now releases the reservation.
- **Watchdog self-heal**: configured-Off plus a retained reservation is by
  definition stale; each daemon retries the verified release on its normal
  backoff even when nothing else is observed alive.
- **Stale-owner takeover**: the record now carries `comm:pid`. A contender
  may take the reservation when the recorded owner has no live process
  (crashed, stopped, uninstalled), or is our known peer, alive, reporting
  Off in its status file, while the interface is verifiably idle. A live
  active owner, an unknown third app, or a busy interface always blocks.
  Takeover happens under the kernel flock, so it cannot race a worker.
  Legacy V8 records (bare name, no pid) parse as a name with a dead owner.
- **Diagnostics**: both `status.json` files now include `radio_owner` (the
  record name), so a readback shows exactly who holds the radio.
- Wifi1b's legacy peer-status peek no longer second-guesses a successful
  ownership begin (a crashed peer's stale status must not block a
  legitimate takeover); the Wifi2 status check remains, as Wifi2 is not
  part of the contract.

## Operation

Turn the active app Off, wait a few seconds for the verified release, then
start the other. If the peer's daemon is gone, or it reports Off with an
idle interface, the other app takes the radio by itself — no restart, no
reboot. If a daemon crashes mid-apply, restart that daemon (or reboot) and
choose Off once; systemd's Restart=always usually does this within 3 s.
Never delete the record file while a daemon lives.

## Validation limits

Host tests cover real flock races, crash-retained records, dead-peer and
live-Off-peer takeover, busy-interface blocking, verified release, both
daemons' blocked/idle entry guards (no radio commands executed), plus the
full WiFi/Wifi1b/Tweaks/combo suites. The on-car release path, the exact
V8 wedge cause, vendor SMACK and RF behavior remain car-pending. Send a
readback (`*_result.txt`, both `status.json` with `radio_owner`, daemon
logs) after the first V9 run.
