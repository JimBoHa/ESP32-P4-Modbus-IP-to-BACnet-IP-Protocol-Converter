# ESP32-P4 Modbus IP to BACnet IP Protocol Converter

Version **0.4.0** converts read-only Modbus TCP points into discoverable
BACnet/IP objects on the **Waveshare ESP32-P4-POE-ETH**. Its web interface lets
you select a built-in Kohler MPAC 1500 ATS profile or upload your own CSV point
map, set the Modbus target and BACnet identity, and save the configuration to
flash. Saving restarts the gateway to apply a complete new object list.

Built-in profiles cover the **older Section 13 MPAC 1500 map**: all 164 points,
or 24 status/electrical points. This is the transfer-switch controller found
on the generator installation, not a generic generator engine register map.
Custom CSV supports up to **128 points**, FC01/02/03/04, scaling, byte order,
integer/float values, bits, states and text. See
[web setup and CSV format](docs/WEB_CONFIGURATION.md) and download the
[example CSV](main/web/template.csv).

The board matches [Amazon ASIN B0FN4FX21T](https://www.amazon.com/dp/B0FN4FX21T).
[Waveshare documentation](https://docs.waveshare.com/ESP32-P4-ETH) specifies
32 MB flash/PSRAM and 100 Mbps Ethernet. Version 0.3.0 was deployed to the
physical board through signed Ethernet OTA and reached a valid boot state.
Real ATS polling and an independent BACnet client passed, including discovery,
1,683 property reads and COV across all four point types. The actual HTTPS
console also validated and saved a three-point custom CSV, reconnected after
restart, and published its readings over BACnet; the full ATS profile was
restored afterward and standard-port broadcast discovery passed. Metasys UI
commissioning, power-loss/cable-pull testing and a 24-hour soak remain pending.
See [validation](docs/VALIDATION.md) for the exact evidence and limits.

Version 0.4.0 also passed signed Ethernet deployment on that board. A controlled
proxy fault verified exact error details, synchronized UTC, recovery and
retention after a saved-history reboot. The original ATS settings were restored
and the same 172 BACnet objects remained discoverable.

## Connections

```text
Kohler ATS Ethernet ── building LAN ── ESP32-P4 RJ45 ── BACnet/IP clients
                                           │
                                     USB data cable
                                           │
                                      Raspberry Pi
                                 (flash / serial console)
```

For a new factory installation, connect the board to the LAN and use its USB-C
programming/data connection to the Pi. An existing compatible signed updater
can be migrated over Ethernet as described in [OTA.md](docs/OTA.md). There is **no GPIO field wiring** for this application. PoE and USB
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

- ATS presets use **FC03**. Custom maps support **FC01/02/03/04 reads**.
  Each connection/request has a 1,200 ms total deadline. Transactions are
  serialized with at least 250 ms after completion before the next request;
  failures trigger bounded backoff.
- For the ATS presets, identity checks establish controller type 23, the commissioned firmware
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

Open **http://GATEWAY_IP/** on the default build or **https://GATEWAY_IP/**
on the signed-update build for profile selection, connection settings, CSV
validation/upload and live point quality. `/api/status` and `/api/points`
provide JSON. Settings and the uploaded CSV persist in a dedicated NVS flash
partition. Rejected input leaves the active and saved configuration unchanged.
Changing profiles or point identifiers requires refreshing field-point
discovery in Metasys; removed objects may need removal from its cached list.

The default factory build serves HTTP without a login; access to port 80 permits
configuration changes. Its firmware installation and updates use USB. The
optional [signed Ethernet update build](docs/OTA.md) serves HTTPS, requires an
admin bearer token for configuration changes and CSV validation, and redirects
HTTP to HTTPS. It supports application-only migration from the documented
existing dual-slot updater. Anonymous diagnostic reads remain available.
Field-device and BACnet point writes remain disabled in both builds.

The **Errors** tab and `/api/errors` retain the latest 32 failed Modbus
transactions with their target, function, zero-based register range, error,
exception/socket details, transaction ID, configuration revision and completion
time. UTC is recorded after SNTP synchronization; earlier errors retain their
boot ID and uptime without inventing a calendar date. Successful reads do not
clear history. A background task saves history to flash at most once per
30 seconds; a sudden restart can lose entries not yet saved. The page shows
pending saves and storage errors. BACnet object identifiers are unchanged.
See [error history and clock setup](docs/WEB_CONFIGURATION.md) for retention
limits and clock configuration.

## Build and configuration

Use **ESP-IDF v5.5.4**, target `esp32p4`, and the bundled BACnet Stack **1.6.0**.
Clone the source and its pinned dependencies:

```sh
git clone --recurse-submodules https://github.com/JimBoHa/ESP32-P4-Modbus-IP-to-BACnet-IP-Protocol-Converter.git
cd ESP32-P4-Modbus-IP-to-BACnet-IP-Protocol-Converter
```

For an existing checkout, run `git submodule update --init --recursive`.
The source defaults use DHCP and the documentation-only Modbus address
`192.0.2.81`; set the actual target through the web interface after flashing.
Unit **41** is the protocol setup default and must match the controller.

Optionally create a private `sdkconfig.site` for initial defaults as shown in
[COMMISSIONING.md](docs/COMMISSIONING.md#configure-and-build-from-source), then:

```sh
export IDF_PATH='/absolute/path/to/esp-idf-v5.5.4'
. "$IDF_PATH/export.sh"
export SDKCONFIG_DEFAULTS='sdkconfig.defaults' # add ;sdkconfig.site if created
idf.py set-target esp32p4
idf.py menuconfig
idf.py build
```

The **ATS Modbus to BACnet gateway** menu seeds initial defaults for target,
unit, expected firmware, optional MAC fingerprint and BACnet identity/port.
It also sets additional I-Am recipients and DHCP/static addressing. `sdkconfig.site`, the
generated `sdkconfig`, and build output are ignored by Git. Defaults seed new
configurations; changing a defaults file does not overwrite an existing
`sdkconfig`. Review the generated configuration before rebuilding. Saved web
settings take precedence over these build defaults; reflashing the application preserves them.

The current defaults select **pre-v3 ESP32-P4 silicon with minimum revision 1**.
Read the actual chip revision before flashing. A rev0 or rev3 board requires an
appropriate rebuild; never bypass an incompatible-image check with `--force`.
[Waveshare's revision guidance](https://docs.waveshare.com/ESP32-P4-ETH/FAQ)
explains the separate build configurations.

The default factory layout, introduced in 0.2.0 and retained in 0.3.0, has a
256 KiB configuration partition at `0x410000`, after the
unchanged factory application partition. When upgrading from 0.1.0, flash the
**partition table and application** using `idf.py flash`; an app-only update
cannot create this partition. No erase of the whole flash is required. The
older private 0.1.0 package does not include the web/CSV features. The signed
OTA layout and legacy-device migration use different offsets; follow
[OTA.md](docs/OTA.md) instead of these USB/factory instructions for that build.

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

The loopback suite verifies read-function request bytes, fragmented responses,
invalid headers/PDUs, exception responses, early disconnects, connection refusal,
and absolute deadlines under silent or slowly transmitting peers. Invalid
responses cannot overwrite prior register values.

See [COMMISSIONING.md](docs/COMMISSIONING.md) for the physical bring-up and
[REFERENCES.md](docs/REFERENCES.md) for the pinned source lineage and primary
hardware/protocol documentation.
