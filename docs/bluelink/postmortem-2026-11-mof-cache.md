# Postmortem: BlueLink unavailable because MOF could not read its navigation cache

TARGET: Ioniq 5 Gen5W_L EU 260128. Investigation: 2026-11-16–17.

**Outcome:** a targeted, owner-approved cache quarantine and MOF restart restored
request processing and successful BlueLink background server transactions on
the car. Explicit phone-app remote-action, navigation-search, and next-boot
checks were still pending at the close of the captured investigation.

## Impact and initial picture

The owner reported BlueLink unavailable while custom WiFi/SSH was installed.
An earlier capture showed a WiFi default route taking precedence over cellular.
The V22/V23 Wifi1b work corrected that interference and retained the Pi's USB
SSH access. However, BlueLink remained unavailable even with a correct modem
default route and two configured cellular contexts.

These were **two distinct issues**. The evidence does not prove the old WiFi
route caused the later cache failure, or that the route fix alone would have
restored BlueLink. V23 was a bundle-version bump over V22's networking code.

## What actually failed

The current boot's MOF startup log contained:

```text
MOF Service failed to start! ParseException:
data dataDecryption failed result[-74] keyExist[1]tried[4]
```

`MOFManager` remained alive but had only four threads, no observed external TCP
session, and a backed-up `/dev/mqueue/mof_recv_q` (44,352 bytes). TMS producers
could still report successful queue sends, hiding the failed consumer startup.

Static inspection of the matching shipped binaries identified the saved-state
read in `MOFHeader::NaviDataRead`:

1. Open `/app/data/Mofgen2/lastnavidata.dat`.
2. Read its bytes and request decryption through `libmofgen2`/CryptoService.
3. The crypto wrapper retries a failed operation and throws `ParseException`.
4. The startup exception handler logs the failure; normal MOF processing never
   starts, although the wrapper/main process stays alive.

Read-only invocation of the vendor's session-bus `dataDecryption` for the exact
77-byte file, using MOF app ID 204 and key type 1, returned **`-74`**. The regional
MOF configuration had initialized successfully, so deleting or replacing that
configuration was not justified. The cache reader's missing-file branch logs
the absence and continues, giving a narrow recovery path.

**Proximate root cause:** unreadable encrypted navigation cache aborts MOF
initialization. **Unresolved upstream cause:** why that cache no longer decrypted.
The evidence does not distinguish ciphertext damage from a key/state mismatch.
No evidence establishes that SCOURGE created or corrupted it.

## Repair and observed result

After explicit approval, on the parked owner's vehicle:

- Verified the file hash against the diagnostic snapshot and refused an
  existing backup destination.
- Stopped only `mof.service`, rechecked the hash, and renamed the cache to an
  adjacent `.bluelink-repair-20261117.bak` file. Original bytes were retained.
- Restarted only `mof.service`; no binary, key, provisioning, or routing changes.

| Before | After |
|---|---|
| Four MOF threads; startup exception | Workers started: 22 threads initially, 12 later |
| Request queue 44,352 bytes | Queue drained to zero and stayed empty in follow-up |
| No external MOF TCP session observed | External established server connections appeared |
| No demonstrated successful server transactions | Valid token; speed-camera/weather HTTP 200; traffic/diagnostics HTTP 204 |

Some old queued requests referenced files already gone and produced local
`FileNotFoundException`/400 responses. Those do not negate the fresh successful
200/204 transactions, but are another reason to test a new user request rather
than treating replayed queue contents as a complete acceptance test.

## Investigation errors and corrections

### We overinterpreted public-network tests

Failed TCP/ICMP to public addresses was incorrectly presented as proof of a
suspended SIM, expired subscription, or modem forwarding failure. Carrier DNS
worked, but neither that nor public-address timeouts established BlueLink
endpoint reachability. The actual Hyundai endpoints later answered over the
same cellular path. A restricted APN is consistent with this; its precise
policy was not proven.

The earlier source-bound context-2 test also did not prove context-2 egress:
binding a source address does not replace route selection.

### We did not initially verify probe tools and return codes

The head unit lacked `timeout`; tests wrapped in it never ran and initially
produced false failure claims. `socat -T` alone also did not bound connect time
as assumed. A watcher later treated any exited TCP child as success without
checking its status. Its `RECOVERED` criterion would not have been reliable.
No such recovery was observed, but the scripts must not be reused as validated
health checks. A future probe must verify prerequisites, bound the operation,
check the actual exit code, test a relevant endpoint, and distinguish timeout
from refusal and command failure.

### Access failures were mixed with application failures

The reverse tunnel changed destination: a host-side listener eventually
forwarded to the **Pi's** SSH, not directly to the car. Its key matched the Pi's
already-known key. The car used SSH port **2224**. A second hop over USB then
worked with the car's known key.

A listening reverse port or an ARP reply did not establish a working car SSH
session. A silent forwarded socket did not establish a completed remote TCP
handshake. Earlier claims that the car was necessarily rebooting, mid-install,
or powered off were not justified by those observations.

The detached V23 reapply log was terminated and had no completion result. Its
pre-install status already showed the V23 build ID. Therefore that launch was
not evidence of a completed install or the cause of a subsequent outage.

### Process existence and intermediate state logs were false health signals

`MOFManager` was alive, provisioning reported full enrollment, and MQTT logged
a `ConnectSuccess` **prestep**. None proved service initialization or broker
connection. The decisive evidence was the startup exception, exact-file crypto
failure, queue backlog, and their reversal after the targeted repair.

## Repository follow-up

- Land the existing V22/V23 modem-safe route and USB-SSH changes on `main`,
  preserving the newer log-retention fixes already there.
- Document the diagnostic layers and the narrow manual repair in the
  [BlueLink guide](architecture-and-diagnostics.md).
- Do **not** add blanket cache deletion, key regeneration, provisioning reset,
  or automatic modem cycling to package installation.
- Combined builds after merging newer `main` changes still need their normal
  offline gates and car acceptance; the observations above describe the V23
  car build, not an untested new bundle.

## Evidence and scope

Raw logs and captures remain private, outside tracked source; they contain
vehicle/account/location information. This report is a sanitized analytical
summary, not a vendored OEM artifact or decompiled source listing.

Matching `MOFManager` SHA256:
`84e3fd380feda27a6bd9971b2e24589900d1bef9d7729ce591181373b9323872`.
Static evidence came from the matching captured binary and `libmofgen2`;
runtime checks used the owner's Pi reverse tunnel and car USB SSH.

Relevant retained REA records:

- `ev_5d7cb2d5e51a8defd8d8eeb83f6efebf2f17e7f2ebc9beb2afd6902f986d917f`:
  saved-navigation cache reader, missing-file branch, decryption call.
- `ev_a9986b64f3d75e81e452636bd3c4dcd478b5ac42dc4b8c4781c203db0418e805`:
  crypto D-Bus request, failure retries, exception propagation.
- `ev_06c95bc57712436e88d0188d3cacd5bb6e7d7f9bdfff92ab49505a7344f9963b`:
  regional configuration and CryptoService destination.

Static analysis explains control flow; it does not by itself establish runtime
success. The live exact-file failure and post-repair HTTP responses are the
corroborating observations. No phone-app remote-command success or reboot
persistence is inferred from them.
