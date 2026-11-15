# V7 installer and Advanced-layout repair

Status: **vendor-VM-tested installer primitives; host Qt Quick-tested layout;
on-car validation pending.** No external review or commit was performed for
this release. The owner explicitly requested skipping external reviews.

## Evidence and correction

The November 12 V6 readback identifies the matching archive and three failed
stages. BusyBox awk aborts while the shared helper parses /proc/mounts; all
stages stop before QML replacement. The photographed overlap therefore does
not establish that V6's bundled placement code was running. Backups were made,
but the new QML was not installed. Historical restore logs and existing daemon
status are not evidence of a successful current install.

The same awk command aborts with rc=134 in the vendor-kernel VM. In that VM,
the shell replacement identifies the root, settles it writable, copies and
verifies bytes without replacing the destination inode, and restores an
initially read-only root. Configuration migration is also tested against the
vendor shell with literal backslashes, ampersands, replacement/append and a
missing terminal newline. No vendor executable is run on the host.

## Changes

- Safety-critical mount detection no longer uses awk; unreadable, missing,
  malformed, conflicting or duplicate matching entries fail closed.
- The combo verifies a manifest-covered preflight helper and reads mount
  state before any package stage; dependent stages stop after a failure.
- Tweaks configuration migration avoids the other installer awk -v call.
- Current Advanced tile layouts are normalized from canonical build inputs,
  including existing tile blocks/helpers. All tiles use a root-owned placement
  snapshot, an explicit rebuild revision, and a not-ready/undefined guard.
  Unknown model entries are not treated as free cells. Completely full grids
  hide unplaceable tiles.
- New Qt Quick tests exercise the generated placement bindings and actual
  tile MouseAreas for both column counts, startup, full-first-page spill,
  same-count rearrangement, unknown models and overflow. Theme/model/action
  services are fixtures: this does not prove vendor C++ model equivalence.
- The flat build gates the selected final layout through Qt Quick Test as well
  as static checks. Qt Quick Test is now required for building a flat bundle.
- Tweaks' missing generated base tree is reconstructed from canonical WiFi
  inputs/artifacts, never a divergent sibling app checkout. Unmarked existing
  base directories are not overwritten.
- The wrapper never replaces authoritative current-stage results with an old
  runtime result. It exports only the matching install/restore logs. Status
  snapshots are explicitly named runtime_snapshot_*. Private qml_readback_*
  folders retain known before/after UI files for each executed stage.
- The owner-approved concurrent Reboot change is included: recovery reboot
  has no drivingRestriction gate, displays a navigation/media/camera warning,
  and retains the firmware-update guard. No actual vehicle reboot was tested.

## Limits and next readback

Host Qt is 6.12; the vendor is Qt 5.4.1. An attempted vendor Qt Quick Test run
was blocked first by the headless OpenGL context, then by the lab compositor's
missing desktop-shell dependency. Those failures are retained as failures, not
reported as Qt5 passes. Do not equate the host scene with AppHomeScreen or RF
CarPlay validation. Production SMACK, vehicle rendering and reboot remain
car-pending. The mdnsd uid-retention experiment remains disarmed.

Review the current stage results first. Return combo.log, matching install
logs, qml_readback_* and runtime snapshots. A status JSON from an old daemon
must not override a failed installer result. Preserve backups; do not wipe home
layout databases, credentials or owner settings to repair tile placement.
