# Validation record

## Version 0.4.0: persistent Modbus error history, 2026-09-24

- Twelve native test executables and twelve TCP loopback tests pass with
  ASan/UBSan. Eight focused history scenarios cover ring replacement and reboot
  reload, unknown UTC, concurrent events during a save, the 30-second write
  limit, write failures/recovery, corrupt/incompatible storage preservation,
  unavailable storage/worker, and failure of each JSON allocation.
- Poller regressions verify exact request metadata for ATS and custom FC01–04
  errors. The expected old-map identity exception remains excluded from both
  the failure counter and retained history.
- Twenty-one Chromium scenarios pass, including empty and retained history,
  unknown UTC, storage errors, a stale diagnostics endpoint without blocking
  point updates, safe rendering of strings, and JSON downloads. Native HTTP
  and HTTPS harnesses exercise the new anonymous read-only route.
- Source review checked the history mutex, snapshot generation during flash
  writes, SNTP initialization, and configuration restoration across a lost
  HTTP response in the opt-in physical test.

## Version 0.3.0: optional signed HTTPS updates, 2026-09-22

- ESP-IDF 5.5.4 signed OTA and default factory ESP32-P4 target builds pass.
  Public defaults retain the factory/HTTP/USB layout; the optional signed
  dual-slot HTTPS build uses the migration procedure in [OTA.md](OTA.md).
- Eleven native test executables and twelve TCP loopback tests pass. The OTA
  request harness covers 24 isolated cases, with startup validation also
  tested. The HTTPS integration harness covers the production web handlers
  with mutation authorization enabled; these remain host tests.
- Sixteen Chromium browser scenarios pass, adding protected CSV validation
  and configuration saves, anonymous reads, missing/invalid-key behavior,
  exact bearer headers, memory-only credentials, and clearing credentials on
  reload or page exit. Desktop and mobile screenshots were inspected.
- Independent source review checked the root integration: early rollback
  deadline, task/HTTPS/network health conditions, configuration errors blocking
  pending-image acceptance, separated persistent configuration, shared HTTPS
  mutation gating, and numeric-address HTTP redirects.

### Physical Ethernet deployment and real ATS checks

The signed 0.3.0 image built from source `d13fdfed031e` was installed over
Ethernet on the physical ESP32-P4. The signed file is **921,600 bytes**. Its ESP application-image digest is
`c33c3d893073d906fa2c1d1faf5faae26bf3bc6f72bc9f9246dd917f2ba86ccd`.
The complete signed-file SHA-256 is
`a7bd222645a5ad48b35b08e60d6d34918057ac18006e6bb08c9c8b9b989a4c64`.
The device reported **ota_0, VALID**; the previous application in ota_1 was
preserved. This is actual device evidence, separate from the host tests above.

- Configuration loaded with an empty `config_error`, and the old MPAC1500
  controller profile verified. The initial snapshot contained 164 points:
  144 good and 20 faulted, unverified or inapplicable. It recorded 53 Modbus
  requests, 53 successful replies and no failures.
- The physical ATS returned line voltages of **488.5 / 486.5 / 486.5 V** and
  frequency **60 Hz**. These are protocol readings, not a contemporaneous
  manual display comparison.
- An independent BACpypes3 client on the LAN passed discovery and the indexed
  list of 172 objects, **1,683 ReadPropertyMultiple properties**, and COV for
  all four point types. Unverified current retained its fault qualification.
- Chromium exercised the actual HTTPS console: it loaded the full 164-point
  profile, accepted the memory-only administrator key, validated a three-point
  custom CSV preview, saved it, and automatically reconnected after restart.
  The page rendered three good points; the resulting screenshot was inspected.
- An independent BACnet check of that custom profile found 11 objects (three
  custom points plus eight gateway/device objects). AI 2101 reported 488.5 V,
  AI 2102 reported 60 Hz, and BI 2103 reported preferred source available as
  active, all with `no-fault-detected` reliability. The full 164-point ATS
  profile was then restored by authenticated save/reboot. Final configuration
  revision **4** has no retained test CSV (`custom_point_count: 0`), and
  controller verification passed again. This proves persistence across a
  commanded restart, not an interrupted flash write.
- After restoration, a standard-port LocalBroadcast Who-Is on UDP 47808
  discovered Device 75181. The final probe confirmed the restored 172 objects,
  488.5 V and 60 Hz with `no-fault-detected` reliability after initial polling
  completed. The final status at 32 seconds uptime showed 29 requests, 29
  successful replies and no failures.
- Actual HTTPS requests with anonymous or viewer credentials could not mutate
  configuration, validate a CSV, upload firmware or reboot: the device returned
  401/403 responses with no mutations.

Physical power-loss/cable-pull testing, configuration persistence under power
interruption, a 24-hour soak and Metasys UI commissioning remain untested.
Acceptance of this signed image does not prove a forced physical rollback.
Raw site addresses and credential-bearing reports remain outside public docs.

## Version 0.2.0: web profiles and CSV maps, 2026-09-22

- ESP-IDF 5.5.4 ESP32-P4 build passes. The image fits the existing 4 MiB
  application partition. A separate 256 KiB NVS configuration partition is
  included in the new partition table.
- Eight native C test executables pass ASan/UBSan with assertions enabled.
  They cover the existing ATS decoder/scheduler, read-function packets,
  dynamic BACnet catalogs, custom CSV decoding/polling, configuration parsing,
  production HTTP handlers and an integrated custom-map path.
- Twelve Python TCP loopback tests pass for FC01/02/03/04, fragmented replies,
  malformed frames, exceptions and absolute deadlines.
- The integrated custom-map test imports six points and makes 19 actual
  localhost TCP requests (18 valid and one incorrect transaction ID). It
  checks all four read functions, 14-object BACnet discovery, changing values,
  fault quality, last-value retention and recovery through production code.
  BACnet transport is in memory; no field device receives this test traffic.
- Configuration tests check malformed/oversized/nested JSON, exact CSV rules,
  128-point capacity, byte order, scaling, duplicate identifiers/names/states,
  read-only operation and rejected-input preservation.
- The HTTP test compiles production gateway_web.c against host HTTP/NVS/timer
  adapters. It verifies fragmented uploads, validation errors, stale-revision
  conflicts, storage errors, CSV persistence/reload and commit-before-restart.
  This tests application logic, not physical flash power-loss behavior.
- Dynamic BACnet tests exercise arbitrary AI/BI/MSI/CSV identifiers, indexed
  object lists, reliability/COV and replacement of prior catalogs. The
  electrical preset retains ATS sensing qualification and core health logic.
- Eleven Chromium browser scenarios pass against the embedded HTML and a
  loopback API fixture: profile choice, CSV validation/preview, invalid and
  oversized upload rejection, exact save payloads, saved-map retention,
  revision conflicts, connection recovery, repairable configuration errors,
  search/filter and keyboard/mobile behavior. Desktop and mobile screenshots
  were inspected. No JavaScript errors or external requests were observed.
  These browser fixtures do not replace the production backend tests above.

At completion of the 0.2.0 test record, that version had not been flashed on
an ESP32-P4. Its tests established host behavior only. The physical 0.3.0
evidence above supersedes that limitation for the paths explicitly exercised;
power-loss, sustained-operation and Metasys UI checks remain separate.

## Version 0.1.0 baseline, 2026-09-22

## Passed before hardware installation

- ESP-IDF **5.5.4**, `esp32p4`, Waveshare Ethernet profile, 32 MB flash,
  pre-v3 silicon with minimum revision 1: firmware builds successfully.
- Four native C test executables pass with AddressSanitizer and Undefined
  Behavior Sanitizer, assertions enabled, and immediate sanitizer failure.
  These cover the 164-point decoder, identity/poll scheduling, FC03 framing,
  and the production BACnet handlers. CTest totals are executables, not a
  claimed count of individual assertions.
- The ten Python/loopback Modbus tests pass. They exercise fragmented replies,
  strict header/PDU validation, unchanged output on failure, silent/truncated
  peers, and one absolute deadline across connect/send/receive. The pure C
  packet suite is also invoked here and overlaps CTest coverage.
- BACnet handler tests cover all 172 object identifiers, indexed Object_List,
  names and Property_List, CSV reliability, MSI state text, write denial,
  discovery addressing, quality changes, 256 subscriptions, overflow refusal,
  queued delivery with 32 concurrent transactions, expiry and retry recovery.
- Generated C tables match the sample-free source catalog.

## Real ATS data through the native production code

The workstation executable uses the same Modbus client, scheduler, point
decoder and BACnet service code compiled into firmware. The native BACnet
listener and independent BACpypes3 0.0.108 client were confined to loopback.
Only normal FC03 reads reached the physical ATS.

The final repeat test passed:

- Directed and range-filtered discovery; 172 complete and indexed objects.
- **1,683** ReadPropertyMultiple properties across all object types.
- Live normal-source voltage of **490.5 V**, with good reliability.
- Confirmed COV on AI, MSI and CSV, and unconfirmed COV on BI; both value and
  status flags received. A same-value write to the test gateway was denied.
- A deliberately paused **local test proxy** caused communication-failure
  reliability and a confirmed fault notification while retaining the last
  voltage. Restoring the proxy cleared the fault and generated a confirmed
  recovery notification. The ATS itself was not stopped or reconfigured.
- 40 FC03 requests reached the ATS. The one induced timeout was confined to
  the local proxy. The native process exited zero with no sanitizer findings.

An earlier run passed the BACnet reads but exposed undefined intermediate
pointer arithmetic in the decoder. That run was invalidated. The fix computes
the register-relative index before pointer addition. A regression test now
acquires all 23 blocks and validates every point boundary. The results above
belong to the corrected-source repeat.

Raw site reports and addresses remain in the ignored `private/` folder.

## Remaining hardware acceptance

The physical 0.3.0 Ethernet deployment is recorded above. New-board USB
installation, cable-pull/DHCP recovery, sustained memory/task behavior, power
interruption during configuration writes, and Metasys device/field-point UI
commissioning are not established by these results. No BTL certification is
claimed.

## Reproduction

Local tests, with no field traffic:

```sh
python3 tools/run_host_tests.py
```

Independent read-only discovery/property probe after commissioning:

```sh
python3 -m venv .venv-test
.venv-test/bin/pip install -r requirements-test.txt
.venv-test/bin/python tools/probe_gateway.py \
  --address CLIENT_IP:47820 --instance 3999999 \
  --target GATEWAY_IP --device 75181 --output private/field-probe.json
```

Choose an unused client port and instance. This probe includes temporary COV
subscriptions which it cancels on exit; it does not write point values unless
the explicit `--check-write-denial` option is supplied. `--expect-live` adds
site-specific 400–550 V and unverified-current checks.

The bounded native live-path test is opt-in and contacts the specified ATS:

```sh
.venv-test/bin/python tools/test_live_path.py \
  --address 127.0.0.1:47821 --instance 3999998 \
  --ats ATS_IP --native build-host/native_gateway \
  --output private/new-live-path-test
```

The output directory must be new. Ports 47819–47821 must be free on loopback.
Fault injection affects only that test's proxy; it does not interrupt other
clients. Do not run it concurrently with another copy of this test.

The physical error-history test requires a signed-HTTPS gateway already using
an ATS preset, a synchronized clock, and a reachable test computer. It saves
the original settings, temporarily points the gateway at a local read-only
proxy, and restarts it. One response receives an incorrect transaction ID;
the source receives only ordinary reads. It checks the recorded error,
successful-read retention and flash save, restores the original target, then
checks retention after the restoration reboot. Gateway readings can briefly
fault during this test. BACnet object identifiers are not changed.

```sh
python3 tools/test_device_error_history.py \
  --host GATEWAY_IP --cert main/ota_server_cert.pem \
  --token-file /private/path/ota_token.txt \
  --proxy-bind TEST_COMPUTER_IP --proxy-port 15020 \
  --output private/new-error-history-test
```

Use a new output directory. TCP 15020 must be reachable from the gateway.
The proxy accepts only that gateway's IPv4 address and forwards FC03 reads
to its existing source. Cleanup retries configuration saves across a pending
restart, including a lost save response. If the test computer itself stops,
use `original-config.json` in the output directory to restore the source
through the gateway web interface. Reports contain site addresses and raw
responses; keep them private. This test does not establish power-loss behavior.
