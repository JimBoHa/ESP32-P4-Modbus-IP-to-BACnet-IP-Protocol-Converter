# Third-party components and reference code

The BACnet implementation is bacnet-stack 1.6.0, commit
`9bc3cfa07aab98852de432fa24079f4b4b6b7eed`, maintained as the
`third_party/bacnet-stack` submodule. Preserve its per-file notices and
`license/` directory. Its component files include GPL-2.0-or-later with
the GCC linking exception, MIT and Apache-2.0 notices; see the source files
and included license texts for the applicable terms.

`main/bip_port.*`, BACnet application patterns and component configuration
derive from JimBoHa's ESP32-S3-PoE-ETH-8DI-8RO-C-BACnet-IP-Firmware project.
Its original project-owned code is licensed under 0BSD. Relevant source
files retain their SPDX identifiers; the port header's MIT notice is retained.

The P4 Ethernet hardware profile and initialization approach were referenced
from JimBoHa's esp32-p4-bacnet-switches project and Waveshare's board
documentation. No production credentials, TLS keys or firmware binaries
from either reference project are included.

ESP-IDF 5.5.4 supplies the Ethernet, FreeRTOS, lwIP, cJSON and platform
libraries. Preserve the licenses distributed with ESP-IDF when distributing
builds. This repository does not change those component license terms.

References and exact upstream source revisions are listed in
[docs/REFERENCES.md](docs/REFERENCES.md).

## 0BSD permission text for the reused project code

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
