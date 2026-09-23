# Signed Ethernet firmware updates

Version 0.3.0 adds an optional signed HTTPS updater. The converter can replace
`esp32-p4-bacnet-switches` over Ethernet when the existing device has that
project's working dual-slot updater and the operator holds its existing TLS,
admin and RSA signing credentials. An application update preserves the
bootloader, partition table and old running slot.

## Existing-device migration

The supported legacy layout is NVS at `0x9000` (24 KiB), OTA data at `0xf000`
(8 KiB), PHY data at `0x11000` (4 KiB), and two 4 MiB applications at `0x20000`
and `0x420000`. The converter registers 256 KiB of configuration NVS at
`0x820000` using ESP-IDF's RAM partition registration API on every boot.
The on-flash partition table is not rewritten.

On first migration the full new region must be erased. The firmware checks
all existing partitions for overlap and bounds, scans the region without
writing, then commits an ownership marker under `gw_migration` in the original
NVS before initializing the new region. Later boots require the same marker.
Unknown data, incompatible layout, flash errors or invalid saved configuration
prevent startup validation; the existing bootloader can return to its old app.
No migration code erases the old application, original configuration namespaces,
bootloader or partition table.

`GW_PROJECT_FAMILY=esp32_p4_bacnet_switches` deliberately retains the old signed
updater's application-family check. The actual application remains identified
as the Modbus-to-BACnet converter in its web status and BACnet model. Version
numbers across these two applications are independent. The firmware's secure
version cannot decrease; hardware anti-rollback is not enabled.

## Build with existing credentials

Use ESP-IDF 5.5.4 and the same board/silicon settings as the running device.
Keep private keys and tokens in protected, ignored storage. The repository
includes public certificate and signing-key pins only; it contains no matching
private credentials. When installing your own identity, replace the public
pins to match your own private files. Do not regenerate an existing device's
credentials during a remote migration.

Expected private filenames are `ota_server_key.pem`, `ota_token.txt`,
`ota_viewer_token.txt`, and `firmware_signing_key.pem`. The two distinct tokens
must contain 32–128 printable characters without a newline. Private files need
mode 0600 and their directory mode 0700. The build validates the TLS pair,
tokens, certificate lifetime and RSA-3072 signing key against the public pins.

Create `private/ota-site.defaults` with the Modbus target, unique BACnet identity
and an absolute `CONFIG_SECURE_BOOT_SIGNING_KEY` path. Existing saved web
settings override those defaults. Then, from the activated IDF environment:

```sh
export ESP32_P4_OTA_SECRETS_DIR=/absolute/private/credentials
idf.py -B build-ota \
  -D SDKCONFIG=private/sdkconfig.ota \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ota.defaults;private/ota-site.defaults' \
  -D GW_PROJECT_FAMILY=esp32_p4_bacnet_switches build
```

For this remote migration, send **only** the signed application image using
HTTPS. Do not send a merged flash image or write the generated partition table.
The target accepts the image only with the existing admin token, same project
family, valid software signature, compatible chip and sufficient inactive slot.

```sh
python3 tools/ota_client.py upload \
  --host DEVICE_IP \
  --cert main/ota_server_cert.pem \
  --token-file /absolute/private/credentials/ota_token.txt \
  --signing-public-key main/ota_signing_public_key.pem \
  --project esp32_p4_bacnet_switches \
  build-ota/esp32_p4_bacnet_switches.bin
```

The client pins the exact server certificate before sending the token. It
checks the signed image locally and checks the target's signing-key hash, then
waits for the exact uploaded image, version, partition and `valid` state.
Do not treat an accepted upload alone as successful deployment.

The imported reference client also has legacy switch-specific commands.
Only `status`, `upload`, and `reboot` apply to this converter; the converter's
configuration and data routes are `/api/config`, `/api/status`, `/api/points`,
and the other routes documented in [web setup](WEB_CONFIGURATION.md).

## Startup validation and access

A new pending image starts a 60-second reset deadline before fallible startup
work. After an initial 10 seconds it needs five consecutive one-second healthy
samples: valid configuration/storage, Ethernet, BACnet initialized and bound,
HTTPS ready, and recent Modbus/BACnet task heartbeats. A failed or stalled boot
resets while still pending, so the old bootloader rolls back. A responding ATS
is tested separately and is not required to keep management reachable.

HTTP port 80 redirects to the numeric local HTTPS address without using an
untrusted Host header. HTTPS mutations require the admin token; viewer tokens
cannot change configuration, upload or reboot. Anonymous reads remain enabled.
No mutation is accepted during pending validation, another upload or a scheduled
restart. Updates keep the same signed-OTA capability for the next release.

For a new board initially flashed with the dual-slot layout, erased OTA metadata
causes one verified boot selection and restart before normal pending-image
health validation. The default factory-only HTTP build does not use this path.

## Physical deployment evidence

On 2026-09-22, the signed 0.3.0 converter built from `d13fdfed031e` was deployed
over Ethernet to the physical board. The 921,600-byte image reached **ota_0,
VALID**, preserving the previous ota_1 application. Its SHA-256 was
`c33c3d893073d906fa2c1d1faf5faae26bf3bc6f72bc9f9246dd917f2ba86ccd`.
Real ATS polling and an independent LAN BACnet probe passed after the update.
The actual HTTPS console also saved a three-point custom CSV and reconnected
after restart. Its BACnet values passed an independent check, then the full
ATS profile was restored by authenticated save/reboot. Final revision 4 cleared
the temporary CSV; standard-port broadcast BACnet discovery and readings passed
after startup polling completed.
Anonymous and viewer mutation attempts returned 401/403 without changes.

This successful migration does not establish power-loss recovery, a forced
physical rollback, cable-pull behavior, a 24-hour soak or Metasys UI
commissioning. See [validation](VALIDATION.md) for the detailed evidence.

ESP-IDF references: [OTA and rollback](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/system/ota.html),
[partition API source](https://github.com/espressif/esp-idf/blob/v5.5.4/components/esp_partition/include/esp_partition.h).
