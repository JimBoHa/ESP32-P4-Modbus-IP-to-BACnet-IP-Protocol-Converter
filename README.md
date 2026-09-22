# ESP32-P4 MPAC 1500 Modbus-to-BACnet gateway

Version **0.1.0** reads a Kohler **MPAC 1500 using the older Section 13 register
map** and publishes its data as read-only BACnet/IP objects. The gateway runs on
the **Waveshare ESP32-P4-POE-ETH**, the board identified by
[Amazon ASIN B0FN4FX21T](https://www.amazon.com/dp/B0FN4FX21T). Waveshare lists this
model in its [ESP32-P4-ETH documentation](https://docs.waveshare.com/ESP32-P4-ETH),
with 32 MB NOR flash, 32 MB PSRAM, and 100 Mbps Ethernet.

The firmware builds for ESP32-P4 and its production protocol code has passed
a native workstation test against the real ATS: 172 BACnet objects, 1,683
property reads, COV, and local-path failure/recovery. It has **not yet been
flashed or commissioned on the physical ESP32-P4**. Board detection, flash
backup, P4 Ethernet operation, and actual Metasys discovery remain part of
[commissioning](docs/COMMISSIONING.md). See [validation](docs/VALIDATION.md).

## Connections

```text
Kohler ATS Ethernet ── building LAN ── ESP32-P4 RJ45 ── BACnet/IP clients
                                           │
                                     USB data cable
                                           │
                                      Raspberry Pi
                                 (flash / serial console)
```

Connect the board to the LAN and use its USB-C programming/data connection to
the Pi. There is **no GPIO field wiring** for this application. PoE and USB
connections follow the board manufacturer's instructions. The firmware uses
the onboard IP101 PHY with MDC GPIO31, MDIO GPIO52, reset GPIO51, and PHY address
1; these are internal board connections, not terminals to wire to the ATS.

## Published data and behavior

[docs/points.csv](docs/points.csv) is the complete point list, with Modbus wire
offsets, encoding, units, scan periods, stale limits, and stable BACnet object
identifiers. The 164 catalog points are **109 Analog Inputs, 26 Binary Inputs,
15 Multi-state Inputs, and 14 CharacterString Values**. Their instances are
1001–1164; use the object's **type and instance together** when importing points.
Multi-state present values are one based and include state text.

Additional objects provide gateway uptime, received BACnet packets, good/fault
point counts, Modbus health, and Ethernet state. The default Device instance is
**75181** and the Network Port instance is **1**. Choose a unique device instance
for each gateway on the BACnet network.

- The only Modbus function implemented is **FC03, Read Holding Registers**.
  Each connection/request has a 1,200 ms total deadline. Transactions are
  serialized with at least 250 ms after completion before the next request;
  failures trigger bounded backoff.
- Identity checks must establish controller type 23, the commissioned firmware
  word, and the old-map profile before the catalog is polled. Newer MPAC maps
  are rejected. Identity is checked again after communication loss and
  periodically during operation.
- Freshness and measurement qualification are separate. Failed/stale values
  retain their prior reading with fault quality. Readable current, L-N voltage,
  phase-angle, optional-I/O, and historical values can remain unverified or
  inapplicable; receiving a register does not establish sensor validity.
- BACnet discovery, ReadProperty, ReadPropertyMultiple, and COV are provided.
  All published objects are read-only through BACnet, including controller
  settings whose native Modbus access is read/write. No transfer, exercise,
  reset, relay, or settings-write commands are implemented.

HTTP provides JSON on port 80: `/api/status` reports gateway/profile health and
`/api/points` reports the 164 points with values and quality reasons. `/` returns
the status JSON. There is no graphical web interface or configuration page in
this version. Configuration and updates use a source build and USB; OTA is not
implemented.

## Build and configuration

Use **ESP-IDF v5.5.4**, target `esp32p4`, and the bundled BACnet Stack **1.6.0**.
For a fresh Git checkout, initialize its pinned stack submodule with
`git submodule update --init --recursive`.
The source defaults use DHCP and the documentation-only Modbus address
`192.0.2.81`; replace the ATS address for deployment. Unit **41** is the protocol
setup default and must match the controller.

Create a private `sdkconfig.site` as shown in
[COMMISSIONING.md](docs/COMMISSIONING.md#configure-and-build-from-source), then:

```sh
export IDF_PATH='/absolute/path/to/esp-idf-v5.5.4'
. "$IDF_PATH/export.sh"
export SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.site'
idf.py set-target esp32p4
idf.py menuconfig
idf.py build
```

The **ATS Modbus to BACnet gateway** menu configures the ATS IPv4 address, port,
unit, expected firmware, optional MAC fingerprint, BACnet identity/port,
additional I-Am recipients, and DHCP/static addressing. `sdkconfig.site`, the
generated `sdkconfig`, and build output are ignored by Git. Defaults seed new
configurations; changing a defaults file does not overwrite an existing
`sdkconfig`. Review the generated configuration before rebuilding.

The current defaults select **pre-v3 ESP32-P4 silicon with minimum revision 1**.
Read the actual chip revision before flashing. A rev0 or rev3 board requires an
appropriate rebuild; never bypass an incompatible-image check with `--force`.
[Waveshare's revision guidance](https://docs.waveshare.com/ESP32-P4-ETH/FAQ)
explains the separate build configurations.

The initial private delivery folder is named `ESP32-P4-ATS-Gateway-v0.1.0`.
Use its manifest, `SHA256SUMS`, and flashing instructions for that particular
image. Its site configuration is separate from this reusable source tree.

## Local verification

These tests use native compilation and local test traffic; they do not contact
an ATS or the building LAN:

```sh
git submodule update --init --recursive
cmake -S tests -B build-host -DENABLE_SANITIZERS=ON
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
python3 -m unittest discover -s tests -p 'test_modbus_loopback.py' -v
```

The loopback suite verifies complete FC03 request bytes, fragmented responses,
invalid headers/PDUs, exception responses, early disconnects, connection refusal,
and absolute deadlines under silent or slowly transmitting peers. Invalid
responses cannot overwrite prior register values.

See [COMMISSIONING.md](docs/COMMISSIONING.md) for the physical bring-up and
[REFERENCES.md](docs/REFERENCES.md) for the pinned source lineage and primary
hardware/protocol documentation.
