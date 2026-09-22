# Validation record

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

The new web/CSV version has not been flashed on an ESP32-P4. Physical USB,
Ethernet, flash persistence under power interruption, task/heap stability and
Metasys commissioning remain pending. The prior real-ATS evidence below belongs
to version 0.1.0; it is not a claim that the new web paths ran on field hardware.

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

## Not yet tested

The ESP32-P4 is not attached yet. USB identity/revision detection, recovery
backup, flashing, real Ethernet PHY/DHCP/link recovery, sustained memory/task
behavior, PoE/power interruption, and Metasys device/field-point discovery
remain hardware acceptance work. Native tests and a successful target build
do not establish those results. No BTL certification is claimed.

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
