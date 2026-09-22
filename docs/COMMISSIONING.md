# Commission the ESP32-P4 ATS gateway

This procedure applies to gateway **v0.1.0**, a Waveshare
**ESP32-P4-POE-ETH** with 32 MB flash, and the older MPAC 1500 register map.
The software has been exercised on the host; this new board's USB identity,
flash contents, Ethernet link, ATS communication, and Metasys behavior still
need to be checked on hardware. Preserve the observations from those checks
alongside the delivered build manifest.

## Prepare the board and Pi

Connect the board's RJ45 port to the intended building LAN. Connect the board's
USB-C programming port to a Raspberry Pi with a **data-capable** cable; a
charge-only cable will power the board without exposing a serial device. Follow
Waveshare's power instructions for the fitted PoE module. The application needs
no GPIO, relay, serial-fieldbus, or meter-signal connections.

Use the actual board's labeling and
[Waveshare hardware description](https://docs.waveshare.com/ESP32-P4-ETH) to
identify its programming connector. The separate USB OTG header is not the
programming connection described here. The onboard Ethernet configuration is:

| Setting | Value |
|---|---|
| Ethernet controller | ESP32-P4 EMAC / RMII |
| PHY | IP101 |
| PHY address | 1 |
| MDC | GPIO31 |
| MDIO | GPIO52 |
| PHY reset | GPIO51 |

Record the ATS's configured IPv4 address, TCP port, Modbus unit, controller type,
and firmware. Also select an unused BACnet device instance. The gateway defaults
are TCP 502, Modbus unit 41, Device 75181, and BACnet/IP UDP 47808; these defaults
do not establish that they are correct or unique at another site.

## Inspect and back up before flashing

Use the ESP-IDF **v5.5.4** environment, which includes the flashing tools:

```sh
export IDF_PATH='/absolute/path/to/esp-idf-v5.5.4'
. "$IDF_PATH/export.sh"
idf.py --version
ls -l /dev/serial/by-id/
```

Set `ATS_USB` to the new board's persistent USB path. Close other serial monitors
before running esptool. If the board does not enter download mode automatically,
hold BOOT, press and release RESET, then release BOOT.

```sh
ATS_USB='/dev/serial/by-id/REPLACE_WITH_THIS_BOARD'
ATS_BACKUP="$HOME/ats-gateway-backups/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$ATS_BACKUP"

python -m esptool --chip esp32p4 --port "$ATS_USB" flash-id \
  | tee "$ATS_BACKUP/chip-and-flash.txt"
python -m esptool --chip esp32p4 --port "$ATS_USB" read-mac \
  | tee "$ATS_BACKUP/mac.txt"
python -m esptool --chip esp32p4 --port "$ATS_USB" get-security-info \
  | tee "$ATS_BACKUP/security-info.txt"
```

Check the detected chip revision and that flash identification reports **32 MB**.
The board model's published specification has been checked, but the delivered
unit must still be identified. Current `sdkconfig.defaults` selects the pre-v3
family with **minimum revision 1**, intended for rev1/rev2 silicon. Rev0 and rev3
require a compatible rebuild. Do not use `--force` to bypass a mismatch; see
[Waveshare's chip-revision guidance](https://docs.waveshare.com/ESP32-P4-ETH/FAQ).
Do not alter eFuses to make an image fit. If security settings prevent reading
or normal flashing, preserve their report and resolve the board's provisioning
before proceeding.

Read the existing flash and save its checksum:

```sh
python -m esptool --chip esp32p4 --port "$ATS_USB" \
  read-flash 0 ALL "$ATS_BACKUP/original-flash.bin"
sha256sum "$ATS_BACKUP/original-flash.bin" > "$ATS_BACKUP/SHA256SUMS"
ls -lh "$ATS_BACKUP/original-flash.bin"
```

On the expected board this backup is 33,554,432 bytes. Keep it outside the source
tree and retain the identity/security reports with it. Flash reads and tool
syntax are documented by
[Espressif](https://docs.espressif.com/projects/esptool/en/latest/esp32p4/esptool/basic-commands.html).

## Configure and build from source

If ESP-IDF is not installed, follow the
[v5.5.4 Linux/macOS setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/get-started/linux-macos-setup.html).
Select version **v5.5.4** and install the `esp32p4` toolchain. This repository
pins the BACnet stack as a Git submodule. In a fresh source checkout, run
`git submodule update --init --recursive` before building; no BACnet package
from a different release is needed.

In the repository, create an ignored file named `sdkconfig.site`. Replace the
placeholder address before building:

```text
CONFIG_GW_MODBUS_HOST="REPLACE_WITH_ATS_IPV4"
CONFIG_GW_MODBUS_PORT=502
CONFIG_GW_MODBUS_UNIT=41
CONFIG_GW_EXPECTED_FIRMWARE=515
CONFIG_GW_EXPECTED_MAC_FRAGMENT=0
CONFIG_GW_DEVICE_INSTANCE=75181
CONFIG_GW_DEVICE_NAME="Kohler-MPAC1500-Gateway"
CONFIG_GW_BACNET_PORT=47808
CONFIG_GW_HOSTNAME="kohler-ats-gateway"
CONFIG_GW_STATIC_IP=""
CONFIG_GW_IAM_PEER1=""
CONFIG_GW_IAM_PEER2=""
```

`515` is the raw firmware word `0x0203` for version 2.03. Do not change this
expectation merely to bypass a rejected controller: first establish that the
actual controller uses the supported old map. The optional MAC check compares
the low 15 bits of the old-map MAC register; zero disables that extra identity
check. It is an equipment fingerprint, not authentication.

DHCP is enabled when `CONFIG_GW_STATIC_IP` is empty. A DHCP reservation is useful
for a stable gateway address. For a static address, set that option to the
gateway's own IPv4 address and configure `CONFIG_GW_NETMASK` and
`CONFIG_GW_ROUTER`. This address belongs to the ESP32 gateway; it is distinct
from `CONFIG_GW_MODBUS_HOST`, which identifies the ATS.

The optional `CONFIG_GW_IAM_PEER1` and `CONFIG_GW_IAM_PEER2` accept supervisor IPv4
addresses for additional unicast I-Am announcements on the configured BACnet
port. Leave them empty when unnecessary. They do not implement a BBMD or make
broadcast discovery cross routed subnets automatically.

```sh
export IDF_PATH='/absolute/path/to/esp-idf-v5.5.4'
. "$IDF_PATH/export.sh"
export SDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.site'
idf.py set-target esp32p4
idf.py menuconfig
idf.py build
```

In `menuconfig`, review **ATS Modbus to BACnet gateway**, flash size, and the
actual chip-revision selection. The quoted semicolon in `SDKCONFIG_DEFAULTS`
combines the board defaults with the later site overrides. Existing `sdkconfig`
values take precedence over defaults: for an existing checkout, use menuconfig
to update those values or preserve the old configuration before generating a
fresh one. `set-target` is for the initial target selection and can regenerate
configuration. These behaviors are covered in the
[ESP-IDF build-system guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-guides/build-system.html#custom-sdkconfig-defaults).

Review the effective settings without publishing private addresses:

```sh
rg 'CONFIG_GW_|CONFIG_ESP32P4_REV_|CONFIG_ESP32P4_SELECTS_REV_|CONFIG_ESPTOOLPY_FLASHSIZE' sdkconfig
```

The source defaults contain a documentation-only ATS address. A reusable build
with those defaults will not reach the real controller. Site files and binaries
can reveal configured addresses, so keep the private build package separate
from the reusable source.

## Flash the matching image

The initial delivery folder, **ESP32-P4-ATS-Gateway-v0.1.0**, contains the private
site-configured `initial-flash.bin`, a manifest, checksums, and exact flashing
instructions. Verify `SHA256SUMS` and confirm that its configuration and supported
chip revision match this installation. Follow that package's instructions for
the merged image; do not substitute an app-only binary at the merged-image
offset.

For your own source build, after the identity, backup, and configuration checks:

```sh
idf.py -p "$ATS_USB" flash monitor
```

ESP-IDF uses the build's own partition/offset arguments. Exit the monitor with
**Ctrl+]**. The partition layout currently contains a factory application and
does not provide OTA slots. Subsequent v0.1.0 updates are USB updates; retain
the working binary and configuration before replacing them.

## Verify Ethernet and the old-map profile

Record the serial boot log, firmware version, detected flash/PSRAM, Ethernet
link state, MAC, and acquired IP address. DHCP is the default. The Pi is the
programming and observation host; the ESP32 itself performs Modbus polling and
BACnet serving after boot.

From a host on the allowed LAN, inspect the JSON endpoints using the gateway's
actual address:

```sh
ATS_GATEWAY='REPLACE_WITH_GATEWAY_IPV4'
curl --fail "http://$ATS_GATEWAY/api/status"
curl --fail "http://$ATS_GATEWAY/api/points"
```

`/api/status` should report firmware `0.1.0`, `ethernet_up: true`, and eventually
`profile_verified: true`. Check `profile_status` when verification fails. Before
catalog reads, the gateway checks controller type, the expected firmware, the
new-map MAC range, and then the old-map fingerprint. A newer map or mismatched
identity leaves the catalog faulted instead of interpreting it as old-map data.

Check that request/success counters advance and response age stays current.
Some points can correctly remain faulted: inspect each point's `quality` and
`quality_reason`. The JSON quality names distinguish good data, communication
failure, unverified measurement, and not-applicable data. In particular, this
version does not automatically qualify CT/current sensing or L-N sensing from
successful TCP responses.

Use the point list's scan/stale columns when judging freshness. It defines 23
blocks with target periods from 2 to 60 seconds; serialization, controller
response time, identity checks, and backoff can extend actual intervals.
The 1,200 ms Modbus timeout covers connect, send, and the complete fragmented
response, not each received fragment separately.

## Verify BACnet and Metasys

Discover the configured Device instance on UDP 47808 (or the selected port).
Check Device identity and the Network Port address/netmask, then import objects
using [points.csv](points.csv). Instances 1001–1164 belong to the 164 typed ATS
objects; do not reinterpret every entry as an Analog Input.

| Additional object | Instance | Meaning |
|---|---:|---|
| Analog Input | 9001 | Gateway uptime, seconds |
| Analog Input | 9002 | Received BACnet packets |
| Analog Input | 9003 | Good ATS point count |
| Analog Input | 9004 | Faulted ATS point count |
| Binary Input | 9001 | Fresh, qualified core Modbus overview available |
| Binary Input | 9002 | Ethernet link available |
| Network Port | 1 | BACnet/IP interface configuration |

Verify representative voltage, frequency, status, settings, and text points
against the controller and its configuration. Read the Reliability and
Status_Flags properties together with Present_Value. Retained values during a
fault are historical observations, not fresh measurements. Modbus health does
not imply all optional points are fitted or qualified.

Exercise ReadProperty, ReadPropertyMultiple, indexed Object_List, COV subscribe,
renewal, and cancellation from the intended BACnet client. Confirm initial COV
values and quality changes after a controlled loss/recovery of the gateway's
network connection. Record discovery and recovery behavior in Metasys. The
implementation supports up to 256 subscriptions and eight subscriber addresses;
it does not persist subscriptions across a gateway reboot, so supervisors must
renew or resubscribe. Confirm the site's expected behavior after power recovery.

Read-only enforcement is covered by local tests. This gateway is intended to
observe the ATS: commissioning does not require a transfer, exercise, alarm
reset, or controller settings change. HTTP is diagnostic JSON without login or
TLS; make it reachable only from the intended management/building LAN.

If Ethernet is up but BACnet discovery fails, check the selected port, duplicate
device instances, VLAN/routing boundaries, firewall rules, and supervisor
discovery scope. If identity passes but points fault, inspect their quality
reasons and the ATS's options/sensing setup before changing decoder assumptions.
