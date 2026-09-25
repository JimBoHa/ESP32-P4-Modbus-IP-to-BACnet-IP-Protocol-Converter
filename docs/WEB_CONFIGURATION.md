# Configure the gateway in a browser

Open the gateway’s HTTPS address in a browser for builds with signed Ethernet
updates. Generic builds without that feature may serve HTTP instead. The console is self-contained: it
uses no external scripts, fonts, analytics, or CDN. Overview, Live points, and Errors
refresh every five seconds. A lost connection leaves the last received values
visible, marks updates paused, and retries with a bounded delay. The point’s
reported quality and reason remain visible separately from browser connectivity.

The Configuration tab loads the saved settings once. Background polling does
not overwrite unsaved edits. “Discard changes” restores the loaded settings.

## Administrator access

The signed-update build requires an administrator key for **CSV validation**
and **Save and restart**. Enter it in the Configuration tab’s masked Admin key
field. The key stays in this page’s memory and is sent as a bearer authorization
header only on those protected POST requests. It is not part of the saved
configuration or CSV, and the page never puts it in browser storage, cookies,
a URL, or displayed text. **Forget key**, reloading, or leaving the page clears
it; a successful gateway restart within this open page retains it.

Status, points, error history, profile lists, and configuration reads remain available without
a key. Builds reporting `authentication_required:false` omit the key panel and
authorization header. `X-Gateway-Request: 1` remains required on mutations in
both modes; it does not replace administrator authentication.

## Error history

The **Errors** tab retains the newest 32 failed Modbus requests, newest first.
Successful reads do not clear it. Each event identifies the source host, port,
unit, function, zero-based wire offset, quantity, transaction ID, configuration
revision, profile, acquisition phase, and failure details. The Modbus exception
code, socket system error number, and elapsed milliseconds distinguish a
controller exception from a connection, timeout, or malformed-response failure.
The expected old-map identity exception is not a failed request.

Events always include their original boot ID and uptime in milliseconds. UTC is
recorded only after the gateway synchronizes its clock; earlier events retain
`utc_ms:null`, displayed as **UTC unavailable**. A later clock synchronization
does not invent timestamps for those events. Event sequence orders the history
even if the clock changes.

`CONFIG_GW_NTP_SERVER` selects the SNTP server at build time (menuconfig).
It defaults to Cloudflare's documented `162.159.200.1`, using UDP port 123;
the numeric address also works on static-IP networks without DNS. An empty
value disables time synchronization. `clock_synchronized` means at least one
successful synchronization during the current boot; afterward the gateway
keeps time locally between updates. It does not assert current server reachability.
See [Cloudflare's NTP instructions](https://developers.cloudflare.com/time-services/ntp/usage/).

History saves run asynchronously at most every 30 seconds. Saved records survive
restarts; a sudden restart can lose unflushed changes. Pending changes and storage
errors appear in the tab. The total and overwritten counters describe this
history, including saved prior boots; the Overview request counters describe the
current boot. Firmware installed before this feature cannot supply past details.

**Download JSON snapshot** exports the most recently received history, also
available at read-only `GET /api/errors`. If that endpoint fails, the tab marks
its retained snapshot stale while Overview and Live points keep updating when
their endpoints work. The download retains the same snapshot until a refresh
succeeds. There is no clear-history endpoint, and this feature does not change
the BACnet point catalog.

## Profiles and connections

- **MPAC1500 full:** the supported older Section 13 automatic transfer switch
  register map.
- **MPAC1500 electrical:** 24 status/electrical points using the same controller
  decoder and identity checks. Both presets retain all ATS polling and identity
  checks; this preset reduces only the points published to BACnet.
- **Custom:** a validated CSV map, with up to 128 points and a 32 KiB file limit.

The built-in profiles describe a Kohler MPAC1500 **ATS**, not a generator
engine controller. Set the Modbus source’s IPv4 address, TCP port, and unit ID.
The Modbus address belongs to the source device, not the gateway itself.
TCP port must be 1–65535; unit ID may be 0–255 for Modbus TCP gateways.

For built-in profiles, enter the expected raw firmware word and optional low
15-bit MAC fragment in decimal. Firmware 2.03 is word 515 (`0x0203`). A MAC
fragment of zero disables that extra identity comparison. These fields must
describe the intended controller; changing them does not qualify a different
controller or register map.

Set the BACnet device name, a device instance unique on the BACnet network,
and the UDP port (normally 47808). The device name accepts 1–95 printable
ASCII characters and cannot begin with the reserved prefix `Gateway-`.

## Custom CSV workflow

1. Select Custom and download the template from the console.
2. Edit the CSV using zero-based Modbus wire addresses and the source device’s
   documented data types, byte order, scaling, and units.
3. Choose the CSV file. The browser submits it to `/api/validate`; this has no
   configuration effect.
4. Resolve validation errors and review the returned point-name/type/instance
   preview.
5. Click **Save and restart** to store the settings and validated CSV.

The exact header is:

```csv
instance,name,object_type,function,address,data_type,byte_order,scale,offset,units,bit,states,length,poll_ms,description
```

Supported object types are AI, BI, MSI, and CSV. Functions 1, 2, 3, and 4 are
read operations. Data types are `u16`, `s16`, `u32`, `s32`, `f32`, `bit`,
`ascii`, and `bool`. Byte order is `AB`/`BA` for 16-bit and ASCII data,
`ABCD`/`BADC`/`CDAB`/`DCBA` for 32-bit data, and empty for `bool`.
MSI labels are separated by `|` (up to 16 states), with at most 63 characters
per label and 512 characters for the combined field. Point names accept up to
63 characters and descriptions up to 127; every CSV field is limited to 512
characters. ASCII length is at most 20 characters. Poll intervals range from
1,000 to 3,600,000 milliseconds. The stale limit in milliseconds is
`max(3 × poll_ms, 3 × point_count × 250 + 1200, 5000)`.
The gateway validates compatible combinations and numeric limits. The `units`
field uses BACnet Engineering Units numbers: volts = `5`, hertz = `27`, and
degrees Celsius = `62`.

The Saved map link downloads the stored CSV. Saving other settings without
uploading a new file preserves an existing custom map. Selecting “Keep saved
map” discards only the browser’s pending replacement.

## Save and reconnect

Saving validates the entire configuration, stores it, and restarts the
gateway. The browser waits for the revised configuration and then resumes
status polling. If the connection closes before the save response arrives,
reload the page to inspect stored settings before submitting again.

After changing profiles or a point map, refresh BACnet device and point
discovery in Metasys. Removed or changed point identifiers may require BAS
mapping changes. A browser preview does not prove Modbus communication,
physical equipment readings, or Metasys import.

All configuration mutations use JSON with `X-Gateway-Request: 1`. The header is
a request-boundary check, not a login credential. The page renders API and CSV
strings as text; it does not execute embedded HTML.

## Firmware updates over Ethernet

Use [`tools/ota_client.py`](../tools/ota_client.py) for signed Ethernet updates;
only its `status`, `upload`, and `reboot` commands apply to this converter.
[OTA.md](OTA.md) gives the exact build and upload procedure. Configuration CSV
upload changes the register map and does
not install firmware. This console does not upload firmware images or expose
the signing key or administrator credential.

## Browser verification

`tests/test_web_ui.mjs` serves the real embedded HTML on loopback with mocked
API responses and exercises the configuration workflow in headless Chromium.
Install Playwright and its Chromium browser, then run:

```sh
npm install --no-save playwright
npx playwright install chromium
node tests/test_web_ui.mjs /tmp/esp32-p4-web-ui
```

An existing install can be selected with `PLAYWRIGHT_MODULE` (absolute path to
Playwright’s `index.mjs`) and `CHROMIUM_EXECUTABLE` (optional browser executable).
The test writes desktop/mobile screenshots and a JSON report. It uses loopback
only and verifies UI behavior; native backend tests validate the production
configuration parser and storage, and hardware commissioning remains separate.
