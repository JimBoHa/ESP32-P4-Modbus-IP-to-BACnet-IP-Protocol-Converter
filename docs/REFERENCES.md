# Source lineage and technical references

This gateway is a separate **v0.2.0** project. The reference firmware projects
provided hardware and BACnet implementation patterns; their working hardware
history does not constitute validation of this new Modbus gateway.

## Pinned firmware sources

| Source | Revision | Use in this project |
|---|---|---|
| [JimBoHa/esp32-p4-bacnet-switches](https://github.com/JimBoHa/esp32-p4-bacnet-switches/tree/9b2f89c6b582528309a28bcc97045e1353631566) | `9b2f89c6b582528309a28bcc97045e1353631566` | Waveshare P4 Ethernet configuration and platform bring-up reference. Its switch-input application is not part of the ATS gateway. |
| [JimBoHa/ESP32-S3-PoE-ETH-8DI-8RO-C-BACnet-IP-Firmware](https://github.com/JimBoHa/ESP32-S3-PoE-ETH-8DI-8RO-C-BACnet-IP-Firmware/tree/97c46a33dcc39ceee794c962dfbc0d94efd2788a) | `97c46a33dcc39ceee794c962dfbc0d94efd2788a` | BACnet Stack integration, object/service patterns, discovery, and COV recovery reference. Its digital-output/relay behavior is not included. |
| [BACnet Stack 1.6.0](https://github.com/bacnet-stack/bacnet-stack/tree/9bc3cfa07aab98852de432fa24079f4b4b6b7eed) | `9bc3cfa07aab98852de432fa24079f4b4b6b7eed` | Vendored in `third_party/bacnet-stack`; selected sources are listed in `components/bacnet_stack/bacnet_sources.cmake`. Keep upstream license notices with the source. |
| [Espressif ESP-IDF v5.5.4](https://github.com/espressif/esp-idf/tree/v5.5.4) | Tag `v5.5.4` | ESP32-P4 toolchain, Ethernet/lwIP, RTOS, USB console, HTTP, and platform runtime. |

The new application supplies its own read-only FC01/02/03/04 TCP client, pinned ATS catalog,
decoder, profile gate, polling schedule, and read-only object integration. The
catalog source is [tools/ats_catalog.json](../tools/ats_catalog.json); generated
outputs are [main/ats_map.inc](../main/ats_map.inc) and
[docs/points.csv](points.csv).

## Hardware

- [Requested product listing, ASIN B0FN4FX21T](https://www.amazon.com/dp/B0FN4FX21T)
  identifies the Waveshare ESP32-P4-POE-ETH purchase model.
- [Waveshare ESP32-P4-ETH family documentation](https://docs.waveshare.com/ESP32-P4-ETH)
  includes the ESP32-P4-POE-ETH variant and lists its 32 MB NOR flash, 32 MB
  PSRAM, Ethernet, programming connector, and BOOT/RESET controls.
- [Waveshare board resources](https://docs.waveshare.com/ESP32-P4-ETH/Resources-And-Documents)
  provides the board schematics and manufacturer examples. The gateway's
  Ethernet settings are IP101, PHY address 1, MDC GPIO31, MDIO GPIO52, and reset
  GPIO51, matching the pinned P4 reference implementation.
- [Waveshare chip-revision FAQ](https://docs.waveshare.com/ESP32-P4-ETH/FAQ)
  distinguishes pre-v3 and v3 silicon builds and directs users to rebuild for a
  revision mismatch rather than force flashing. The actual supplied board's
  revision and flash size must be read before programming it.

## Protocol and build documentation

- [Kohler TP-6113, 12/21](https://resources.kohler.com/power/kohler/industrial/pdf/tp6113.pdf),
  **Section 13, MPAC 1500 ATS Controller**, is the register-map reference. The
  project uses the older map and verifies controller identity/profile before
  publishing its catalog. A controller with a newer map is not automatically
  compatible. Catalog offsets are zero-based FC03 wire offsets, without a
  leading `4` register-area notation.
- [Modbus TCP/IP Implementation Guide V1.0b](https://www.modbus.org/docs/Modbus_Messaging_Implementation_Guide_V1_0b.pdf)
  defines MBAP transaction/protocol/unit identifiers and TCP message framing.
  The client validates those fields and the requested read-function response before committing
  received words.
- [ESP-IDF v5.5.4 build-system guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-guides/build-system.html)
  describes `SDKCONFIG_DEFAULTS`, precedence of generated configuration, and
  flash argument files.
- [ESP-IDF v5.5.4 configuration reference](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/kconfig-reference.html)
  documents target revision, flash, memory, and Ethernet settings.
- [Espressif esptool commands](https://docs.espressif.com/projects/esptool/en/latest/esp32p4/esptool/basic-commands.html)
  documents flash identification, flash backup, and image writing. Use the
  esptool environment supplied with the pinned IDF and the delivered image's
  manifest/flash arguments.

- [ESP-IDF NVS storage](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/storage/nvs_flash.html)
  documents named NVS partitions, blob storage and commit behavior used by the
  web settings store.
- [ESP-IDF HTTP server](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/protocols/esp_http_server.html)
  documents request-body handling and response APIs.
- [cJSON 1.7.19](https://github.com/DaveGamble/cJSON/tree/c859b25da02955fef659d658b8f324b5cde87be3)
  is pinned for native configuration tests, matching ESP-IDF's component.

## Evidence boundaries

Native decoder tests and loopback protocol tests establish behavior for their
test inputs. A successful target build establishes compilation/linking for its
configuration. Neither establishes that a particular board boots, the ATS has
the assumed sensing/options, a measurement matches its physical input, or a
supervisor imports and trends the points correctly. Record those checks during
[commissioning](COMMISSIONING.md), preserving point quality and any remaining
measurement qualifications.
