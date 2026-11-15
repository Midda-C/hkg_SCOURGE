# Preserved WIP baseline: known issues

The baseline checkpoint preserves prior work. It is **not a deployable or
car-validated wireless CarPlay implementation**. Do not publish it as finished.

Independent review: DeepSeek 4.1 Flash-Max. Initial review failed for a
credential fragment in a negative release-test assertion and MD5-only OEM
binary validation in migration code. The fragment was removed; migration now
uses the SHA256 firmware pins, with fixture-only test overrides. An old personal
SSID comment was replaced by synthetic network names. These hygiene changes
do not rotate any owner credentials; rotate previously exposed credentials
outside the source tree if still in use.

## Remaining pre-existing WIP (must gate release, not merely document it)

- Wrapper/master first-action cleanup includes broad temporary-file deletion
  and removed-WiFi runtime deletion before complete bundle verification.
  Preserve required first-action space reclamation, but narrow deletion and
  fail closed on unknown/unreadable app-presence evidence. Do not destroy owner
  credentials or restore data when an archive is corrupt.
- Wifi1b restore does not fully restore a pre-existing service/enablement from
  its transaction ledger.
- Legacy migration has owner-symlink preservation, account-file I/O checks,
  archive readback and no-ledger service-removal issues. Migration is excluded
  from the requested standalone V20 path; do not enable it as a workaround.
- Battery's skip map omits its tile on an assumption that WiFi installs the
  last copy, which is not true for standalone/no-WiFi builds.
- Direct stock-USB control changes can call AP teardown outside the shared
  radio-ownership contract.
- Built-in DHCP server writes leases without reloading them, and does not
  correctly refresh all lease lifetimes. Packet option lengths, renewal,
  rebooted clients and INFORM responses need stronger coverage.
- Pure-query startup invokes temporary-file cleanup before identifying query
  mode; queries should not mutate runtime state.
- Payload timestamp/PID stamps defeat reproducibility. The vendor loader's
  content-digest skip mechanism needs a deterministic replacement.
- Side-by-side tests contain a broad host process-kill pattern; replace with
  fixture-owned PID/process-group cleanup.
- Wifi1b and migration READMEs/generated instructions still describe older
  WiFi-first layouts and need accurate standalone/validation/account details.
- Latest DHCP client retry patch still omits client ID in REQUEST, retains
  unsafe global rp_filter mutation, and can report address-application success
  despite command failure. These remain unvalidated, not established fixes.

## Mandatory scope preservation

Keep GB, owner settings/keys, network priority, stock USB, modem routing,
Battery/Tweaks/Reboot and the shared micom helper. No vehicle deployment is
authorized. Existing releases remain unchanged while this branch is WIP.
