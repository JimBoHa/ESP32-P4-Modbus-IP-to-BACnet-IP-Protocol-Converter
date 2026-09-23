#!/usr/bin/env python3
"""Pinned-certificate client for the ESP32-P4 authenticated HTTPS OTA API."""

from __future__ import annotations

import argparse
import dataclasses
import getpass
import hashlib
import hmac
import http.client
import json
import os
import ssl
import stat
import struct
import sys
import time
from pathlib import Path

try:
    from .firmware_signing import DEFAULT_PUBLIC_KEY, verify_image_signature
except ImportError:  # Direct script execution adds tools/, not the repository root.
    from firmware_signing import DEFAULT_PUBLIC_KEY, verify_image_signature


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CERTIFICATE = PROJECT_ROOT / "main" / "ota_server_cert.pem"
DEFAULT_TOKEN_FILE = PROJECT_ROOT / "secrets" / "ota_token.txt"
DEFAULT_PROJECT = "esp32_p4_bacnet_switches"
ESP_IMAGE_MAGIC = 0xE9
ESP32_P4_CHIP_ID = 18
ESP_APP_DESC_MAGIC = 0xABCD5432
ESP_IMAGE_HEADER_BYTES = 24
ESP_SEGMENT_HEADER_BYTES = 8
ESP_APP_DESC_BYTES = 256
MAX_OTA_IMAGE_BYTES = 0x400000
MAX_HTTP_RESPONSE_BYTES = 1024 * 1024
MAX_ESP_IMAGE_SEGMENTS = 16
ESP_IMAGE_CHECKSUM_MAGIC = 0xEF
EXPECTED_FLASH_SIZE_CODE = 0x50  # 32 MB


@dataclasses.dataclass(frozen=True)
class FirmwareMetadata:
    size: int
    file_sha256: str
    image_sha256: str
    project: str
    version: str
    signed: bool = False
    signing_key_sha256: str | None = None
    unsigned_size: int = 0
    image: bytes = dataclasses.field(default=b"", repr=False, compare=False)


def _decode_app_field(value: bytes, name: str) -> str:
    encoded = value.split(b"\0", 1)[0]
    if not encoded:
        raise ValueError(f"firmware {name} is empty")
    try:
        return encoded.decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError(f"firmware {name} is not ASCII") from error


def _inspect_firmware(
    path: Path, public_key: Path = DEFAULT_PUBLIC_KEY, *, require_signature: bool = False,
) -> FirmwareMetadata:
    with path.open("rb") as source:
        image = source.read(MAX_OTA_IMAGE_BYTES + 1)
    if len(image) > MAX_OTA_IMAGE_BYTES:
        raise ValueError("firmware image exceeds the 4 MiB OTA partition")
    descriptor_offset = ESP_IMAGE_HEADER_BYTES + ESP_SEGMENT_HEADER_BYTES
    minimum_size = descriptor_offset + ESP_APP_DESC_BYTES + hashlib.sha256().digest_size
    if len(image) < minimum_size:
        raise ValueError("firmware image is truncated")
    if image[0] != ESP_IMAGE_MAGIC:
        raise ValueError("file is not an ESP application image")
    segment_count = image[1]
    if not 1 <= segment_count <= MAX_ESP_IMAGE_SEGMENTS:
        raise ValueError(
            f"firmware segment count {segment_count} is outside the valid 1-16 range"
        )
    chip_id = struct.unpack_from("<H", image, 12)[0]
    if chip_id != ESP32_P4_CHIP_ID:
        raise ValueError(f"firmware targets chip ID {chip_id}, not ESP32-P4")
    if image[3] & 0xF0 != EXPECTED_FLASH_SIZE_CODE:
        raise ValueError("firmware is not configured for this board's 32 MB flash")
    if image[23] != 1:
        raise ValueError("firmware image is missing its appended validation hash")

    digest_size = hashlib.sha256().digest_size
    digest_offset = len(image) - digest_size
    position = ESP_IMAGE_HEADER_BYTES
    checksum = ESP_IMAGE_CHECKSUM_MAGIC
    descriptor_offset = 0
    first_segment_size = 0
    for segment_index in range(segment_count):
        if position + ESP_SEGMENT_HEADER_BYTES > digest_offset:
            raise ValueError("firmware segment header is truncated")
        segment_size = struct.unpack_from("<I", image, position + 4)[0]
        segment_data_offset = position + ESP_SEGMENT_HEADER_BYTES
        segment_end = segment_data_offset + segment_size
        if segment_end > digest_offset:
            raise ValueError("firmware segment data is truncated")
        if segment_index == 0:
            descriptor_offset = segment_data_offset
            first_segment_size = segment_size
        for byte in image[segment_data_offset:segment_end]:
            checksum ^= byte
        position = segment_end

    checksum_offset = position + (15 - (position % 16))
    digest_offset = checksum_offset + 1
    unsigned_end = digest_offset + digest_size
    if unsigned_end > len(image):
        raise ValueError("firmware has an invalid checksum or digest position")
    if image[checksum_offset] != checksum:
        raise ValueError("firmware ESP image checksum does not match its segments")
    if first_segment_size < ESP_APP_DESC_BYTES:
        raise ValueError("firmware first segment has no complete application descriptor")
    descriptor_magic = struct.unpack_from("<I", image, descriptor_offset)[0]
    if descriptor_magic != ESP_APP_DESC_MAGIC:
        raise ValueError("firmware application descriptor is invalid")

    appended_digest = image[digest_offset:unsigned_end]
    computed_digest = hashlib.sha256(image[:digest_offset]).digest()
    if not hmac.compare_digest(appended_digest, computed_digest):
        raise ValueError("firmware appended validation hash does not match its contents")

    signed = len(image) != unsigned_end
    if require_signature and not signed:
        raise ValueError("unsigned firmware refused; a pinned firmware signature is required")
    signing_key_sha256 = verify_image_signature(image, unsigned_end, public_key) if signed else None

    version = _decode_app_field(image[descriptor_offset + 16 : descriptor_offset + 48], "version")
    project = _decode_app_field(image[descriptor_offset + 48 : descriptor_offset + 80], "project")
    return FirmwareMetadata(
        size=len(image),
        file_sha256=hashlib.sha256(image).hexdigest(),
        image_sha256=appended_digest.hex(),
        project=project,
        version=version,
        signed=signed,
        signing_key_sha256=signing_key_sha256,
        unsigned_size=unsigned_end,
        image=image,
    )


def _read_token(path: Path | None) -> str:
    if path is not None:
        try:
            if os.name == "posix":
                mode = stat.S_IMODE(path.stat().st_mode)
                if mode & 0o077:
                    raise ValueError(
                        f"OTA token file has group/other permissions: {path} "
                        f"({mode:04o}); require mode 0600"
                    )
            token = path.read_text(encoding="ascii").strip()
        except FileNotFoundError:
            token = ""
    else:
        token = ""
    if not token:
        token = getpass.getpass("OTA bearer token: ").strip()
    if not 32 <= len(token) <= 128 or any(
        not 0x21 <= ord(character) <= 0x7E for character in token
    ):
        raise ValueError("OTA token must contain 32-128 printable ASCII characters")
    return token


def _certificate_der(path: Path) -> bytes:
    pem = path.read_text(encoding="ascii")
    return ssl.PEM_cert_to_DER_cert(pem)


def _connection(
    host: str,
    port: int,
    certificate: Path,
    timeout: float,
) -> http.client.HTTPSConnection:
    expected_der = _certificate_der(certificate)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    connection = http.client.HTTPSConnection(
        host, port=port, timeout=timeout, context=context
    )
    connection.connect()
    if connection.sock is None:
        connection.close()
        raise ssl.SSLError("TLS connection did not provide a peer socket")
    peer_der = connection.sock.getpeercert(binary_form=True)
    if peer_der is None or not hmac.compare_digest(peer_der, expected_der):
        connection.close()
        raise ssl.SSLCertVerificationError(
            "device certificate does not match the pinned certificate"
        )
    return connection


def _show_response(response: http.client.HTTPResponse) -> tuple[int, object | None]:
    payload = _read_response(response).decode("utf-8", errors="replace")
    print(f"HTTP {response.status} {response.reason}")
    decoded: object | None = None
    try:
        decoded = json.loads(payload)
        print(json.dumps(decoded, indent=2, sort_keys=True))
    except json.JSONDecodeError:
        if payload:
            print(payload)
    return (0 if 200 <= response.status < 300 else 1), decoded


def _read_response(response: http.client.HTTPResponse) -> bytes:
    payload = response.read(MAX_HTTP_RESPONSE_BYTES + 1)
    if len(payload) > MAX_HTTP_RESPONSE_BYTES:
        raise ValueError("device response exceeds the 1 MiB safety limit")
    return payload


def _fetch_status(
    host: str,
    port: int,
    certificate: Path,
    timeout: float,
    token: str,
) -> dict[str, object]:
    connection = _connection(host, port, certificate, timeout)
    try:
        connection.request(
            "GET",
            "/ota/status",
            headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
        )
        response = connection.getresponse()
        payload = _read_response(response)
        if not 200 <= response.status < 300:
            raise http.client.HTTPException(
                f"status check returned HTTP {response.status} {response.reason}"
            )
        decoded = json.loads(payload)
        if not isinstance(decoded, dict):
            raise ValueError("status response is not a JSON object")
        return decoded
    finally:
        connection.close()


def _status(args: argparse.Namespace, token: str) -> int:
    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request(
            "GET",
            "/ota/status",
            headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
        )
        result, _ = _show_response(connection.getresponse())
        return result
    finally:
        connection.close()


def _validate_diagnostics_report(report: object, token: str) -> dict[str, object]:
    """Reject malformed reports and accidental credential exports before saving."""
    sections = ("status", "active_configuration", "saved_configuration",
                "active_network_configuration", "saved_network_configuration",
                "confirmed_network_configuration")
    if (
        not isinstance(report, dict)
        or report.get("schema") != 1
        or report.get("report_type") != "esp32-p4-diagnostics"
        or any(not isinstance(report.get(name), dict) for name in sections)
    ):
        raise ValueError("invalid diagnostics report document")
    forbidden_keys = {"token", "ota_token", "ota_viewer_token", "admin_token", "viewer_token",
                      "bearer_token", "private_key", "ota_server_key"}

    def inspect(value: object, depth: int = 0) -> None:
        if depth > 32:
            raise ValueError("diagnostics report nesting exceeds safety limit")
        if isinstance(value, dict):
            for key, item in value.items():
                if str(key).lower() in forbidden_keys:
                    raise ValueError("diagnostics report contains a credential field")
                inspect(key, depth + 1)
                inspect(item, depth + 1)
        elif isinstance(value, list):
            for item in value:
                inspect(item, depth + 1)
        elif isinstance(value, str) and (
            (token and token in value)
            or "PRIVATE KEY-----" in value
            or "Bearer " in value
        ):
            raise ValueError("diagnostics report contains credential material")

    inspect(report)
    return report


def _diagnostics_report(args: argparse.Namespace, token: str) -> int:
    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request("GET", "/diagnostics/report", headers={
            "Authorization": f"Bearer {token}", "Accept": "application/json"})
        response = connection.getresponse()
        payload = _read_response(response)
        if response.status != 200:
            raise http.client.HTTPException(
                f"diagnostics report returned HTTP {response.status}")
        report = _validate_diagnostics_report(json.loads(payload), token)
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
        with os.fdopen(os.open(args.output, flags, 0o600), "w", encoding="utf-8") as stream:
            stream.write(json.dumps(report, indent=2, sort_keys=True) + "\n")
        print(f"Diagnostics report saved: {args.output}")
        print("Contains site configuration and network addresses; review before sharing.")
        return 0
    finally:
        connection.close()


def _config_get(args: argparse.Namespace, token: str) -> int:
    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request(
            "GET",
            "/config",
            headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
        )
        result, decoded = _show_response(connection.getresponse())
        if result == 0 and args.output is not None:
            if not isinstance(decoded, dict):
                raise ValueError("configuration response is not a JSON object")
            args.output.write_text(
                json.dumps(decoded, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(f"Saved configuration to {args.output}")
        return result
    finally:
        connection.close()


def _config_put(args: argparse.Namespace, token: str) -> int:
    configuration: Path = args.configuration
    try:
        decoded = json.loads(configuration.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"configuration is not valid JSON: {error}") from error
    if not isinstance(decoded, dict):
        raise ValueError("configuration must be a JSON object")
    body = json.dumps(decoded, separators=(",", ":")).encode("utf-8")
    if not 1 <= len(body) <= 4096:
        raise ValueError("encoded configuration must be 1-4096 bytes")

    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request(
            "PUT",
            "/config",
            body=body,
            headers={
                "Authorization": f"Bearer {token}",
                "Content-Type": "application/json",
                "Content-Length": str(len(body)),
                "Accept": "application/json",
            },
        )
        result, _ = _show_response(connection.getresponse())
        return result
    finally:
        connection.close()


def _network_get(args: argparse.Namespace, token: str) -> int:
    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request(
            "GET",
            "/network/config",
            headers={"Authorization": f"Bearer {token}", "Accept": "application/json"},
        )
        result, decoded = _show_response(connection.getresponse())
        if result == 0 and args.output is not None:
            if not isinstance(decoded, dict):
                raise ValueError("network configuration response is not a JSON object")
            args.output.write_text(
                json.dumps(decoded, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(f"Saved network configuration to {args.output}")
        return result
    finally:
        connection.close()


def _network_put(args: argparse.Namespace, token: str) -> int:
    configuration: Path = args.configuration
    try:
        decoded = json.loads(configuration.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"network configuration is not valid JSON: {error}") from error
    if not isinstance(decoded, dict):
        raise ValueError("network configuration must be a JSON object")
    body = json.dumps(decoded, separators=(",", ":")).encode("utf-8")
    if not 1 <= len(body) <= 4096:
        raise ValueError("encoded network configuration must be 1-4096 bytes")

    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request(
            "PUT",
            "/network/config",
            body=body,
            headers={
                "Authorization": f"Bearer {token}",
                "Content-Type": "application/json",
                "Content-Length": str(len(body)),
                "Accept": "application/json",
            },
        )
        result, _ = _show_response(connection.getresponse())
        return result
    finally:
        connection.close()


def _network_confirm(args: argparse.Namespace, token: str) -> int:
    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request(
            "POST",
            "/network/config/confirm",
            body=b"",
            headers={
                "Authorization": f"Bearer {token}",
                "Content-Length": "0",
                "Accept": "application/json",
            },
        )
        result, _ = _show_response(connection.getresponse())
        return result
    finally:
        connection.close()


def _wait_for_reboot(
    args: argparse.Namespace,
    token: str,
    before: dict[str, object],
) -> int:
    before_boot = _nested_integer(before.get("system"), "boot_count")
    before_hash = before.get("image_sha256")
    before_partition = before.get("partition")
    if (
        before_boot is None
        or not isinstance(before_hash, str)
        or not isinstance(before_partition, str)
    ):
        raise ValueError(
            "pre-reboot status lacks boot count, image identity, or partition"
        )
    expected_boot = (before_boot + 1) & 0xFFFFFFFF

    post_host = args.post_host or args.host
    deadline = time.monotonic() + args.reboot_timeout
    last_error = "device did not return"
    while time.monotonic() < deadline:
        time.sleep(args.poll_interval)
        try:
            status = _fetch_status(
                post_host,
                args.port,
                args.cert,
                min(args.timeout, 10.0),
                token,
            )
        except (
            OSError,
            UnicodeError,
            ValueError,
            ssl.SSLError,
            http.client.HTTPException,
        ) as error:
            last_error = str(error)
            continue

        current_boot = _nested_integer(status.get("system"), "boot_count")
        if current_boot == before_boot:
            last_error = "device has not rebooted yet"
            continue
        if current_boot is None:
            last_error = "post-reboot status lacks a boot count"
            continue
        if current_boot != expected_boot:
            raise ValueError(
                f"unexpected post-reboot boot count {current_boot}; "
                f"expected {expected_boot}"
            )
        if status.get("image_sha256") != before_hash:
            raise ValueError("device rebooted into a different firmware image")
        if status.get("partition") != before_partition:
            raise ValueError("device rebooted into a different OTA partition")
        if status.get("state") != "valid":
            last_error = f"rebooted image state is {status.get('state')!r}"
            continue
        print(
            f"Reboot verified: boot count {before_boot} -> {current_boot}, "
            f"version {status.get('version')}, image SHA-256 {before_hash}"
        )
        return 0
    raise TimeoutError(
        f"reboot was not verified within {args.reboot_timeout:.0f} seconds: {last_error}"
    )


def _reboot(args: argparse.Namespace, token: str) -> int:
    before = _fetch_status(args.host, args.port, args.cert, args.timeout, token)
    before_boot = _nested_integer(before.get("system"), "boot_count")
    before_hash = before.get("image_sha256")
    before_partition = before.get("partition")
    if (
        before_boot is None
        or not isinstance(before_hash, str)
        or not isinstance(before_partition, str)
    ):
        raise ValueError(
            "pre-reboot status lacks boot count, image identity, or partition"
        )

    connection = _connection(args.host, args.port, args.cert, args.timeout)
    accepted: object | None = None
    request_result = 2
    try:
        connection.request(
            "POST",
            "/system/reboot",
            body=b"",
            headers={
                "Authorization": f"Bearer {token}",
                "Content-Length": "0",
                "Accept": "application/json",
            },
        )
        request_result, accepted = _show_response(connection.getresponse())
    except (OSError, ssl.SSLError, http.client.HTTPException):
        print("Reboot response was interrupted; checking the device")
    finally:
        connection.close()

    if request_result == 1:
        return 1
    accepted_boot = (
        accepted.get("boot_count") if isinstance(accepted, dict) else None
    )
    if request_result == 0 and (
        not isinstance(accepted, dict)
        or accepted.get("accepted") is not True
        or accepted.get("rebooting") is not True
        or not isinstance(accepted_boot, int)
        or isinstance(accepted_boot, bool)
        or accepted_boot != before_boot
        or accepted.get("image_sha256") != before_hash
    ):
        raise ValueError("device returned an invalid reboot acceptance record")
    return _wait_for_reboot(args, token, before)


def _upload(args: argparse.Namespace, token: str) -> int:
    firmware: Path = args.firmware
    metadata = _inspect_firmware(
        firmware, getattr(args, "signing_public_key", DEFAULT_PUBLIC_KEY),
        require_signature=not getattr(args, "allow_unsigned_legacy", False),
    )
    if metadata.project != args.project:
        raise ValueError(
            f"firmware project is {metadata.project!r}, expected {args.project!r}"
        )

    post_certificate = args.post_cert or args.cert
    if not args.no_wait and not post_certificate.is_file():
        raise FileNotFoundError(f"post-reboot certificate not found: {post_certificate}")
    post_token = token
    if not args.no_wait and args.post_token_file is not None:
        post_token = _read_token(args.post_token_file)

    before = _fetch_status(args.host, args.port, args.cert, args.timeout, token)
    if before.get("security", {}).get("software_signature_verification") is True:
        if not metadata.signed or before.get("ota_policy", {}).get("signing_key_sha256") != metadata.signing_key_sha256:
            raise ValueError("firmware signature does not match the running device's trusted key")
    if before.get("project") != metadata.project:
        raise ValueError(
            f"connected device project is {before.get('project')!r}, "
            f"expected {metadata.project!r}"
        )
    if before.get("state") != "valid":
        raise ValueError(
            f"running firmware state is {before.get('state')!r}, expected 'valid'"
        )
    if (
        before.get("image_sha256") == metadata.image_sha256
        and not args.allow_same_image
    ):
        raise ValueError(
            "device already runs this exact firmware image; "
            "use --allow-same-image to reflash intentionally"
        )
    print(
        f"Uploading {firmware} ({metadata.size} bytes, version {metadata.version}, "
        f"file SHA-256 {metadata.file_sha256}, image SHA-256 {metadata.image_sha256})"
    )

    connection = _connection(args.host, args.port, args.cert, args.timeout)
    accepted: object | None = None
    upload_result = 2
    try:
        connection.putrequest("POST", "/ota")
        connection.putheader("Authorization", f"Bearer {token}")
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(metadata.size))
        connection.putheader("X-Firmware-Project", args.project)
        connection.putheader("Accept", "application/json")
        connection.endheaders()
        for offset in range(0, metadata.size, 16384):
            connection.send(metadata.image[offset : offset + 16384])
        upload_result, accepted = _show_response(connection.getresponse())
    except (OSError, ssl.SSLError, http.client.HTTPException):
        if args.no_wait:
            raise
        print("Upload response was interrupted; checking the rebooted device")
    finally:
        connection.close()

    if upload_result == 1:
        return 1
    if upload_result == 0:
        accepted_partition = (
            accepted.get("partition") if isinstance(accepted, dict) else None
        )
        if (
            not isinstance(accepted, dict)
            or accepted.get("accepted") is not True
            or accepted.get("rebooting") is not True
            or accepted.get("version") != metadata.version
            or accepted_partition not in {"ota_0", "ota_1"}
        ):
            raise ValueError("device returned success without an OTA acceptance record")
        if accepted.get("image_sha256") != metadata.image_sha256:
            raise ValueError("device accepted image hash does not match uploaded firmware")
        if accepted_partition == before.get("partition"):
            raise ValueError("device accepted firmware into its running OTA partition")
    else:
        accepted_partition = None
    if args.no_wait:
        return upload_result

    return _wait_for_deployment(
        args,
        post_certificate,
        post_token,
        metadata,
        before,
        accepted_partition,
    )


def _nested_integer(value: object, key: str) -> int | None:
    if not isinstance(value, dict):
        return None
    nested = value.get(key)
    return nested if isinstance(nested, int) and not isinstance(nested, bool) else None


def _wait_for_deployment(
    args: argparse.Namespace,
    certificate: Path,
    token: str,
    metadata: FirmwareMetadata,
    before: dict[str, object] | None,
    expected_partition: str | None = None,
) -> int:
    before_boot = (
        _nested_integer(before.get("system"), "boot_count")
        if before is not None
        else None
    )
    before_partition = before.get("partition") if before is not None else None
    deadline = time.monotonic() + args.reboot_timeout
    last_error = "device did not return"
    while time.monotonic() < deadline:
        time.sleep(args.poll_interval)
        try:
            status = _fetch_status(
                args.host,
                args.port,
                certificate,
                min(args.timeout, 10.0),
                token,
            )
        except (
            OSError,
            UnicodeError,
            ValueError,
            ssl.SSLError,
            http.client.HTTPException,
        ) as error:
            last_error = str(error)
            continue

        system = status.get("system")
        current_boot = _nested_integer(system, "boot_count")
        rebooted = (
            before_boot is None
            or (current_boot is not None and current_boot > before_boot)
            or status.get("partition") != before_partition
        )
        current_hash = status.get("image_sha256")
        state = status.get("state")
        if rebooted and current_hash == metadata.image_sha256 and state == "valid":
            if status.get("project") != metadata.project:
                raise ValueError("deployed firmware reports a different project")
            if status.get("version") != metadata.version:
                raise ValueError("deployed firmware reports a different version")
            if metadata.signed and (
                status.get("security", {}).get("software_signature_verification") is not True
                or status.get("ota_policy", {}).get("signing_key_sha256") != metadata.signing_key_sha256
            ):
                raise ValueError("deployed firmware did not confirm the expected signing policy and trusted key")
            if (
                expected_partition is not None
                and status.get("partition") != expected_partition
            ):
                raise ValueError("deployed firmware is running from an unexpected partition")
            print(
                f"Deployment verified: version {status.get('version')}, "
                f"partition {status.get('partition')}, image SHA-256 {current_hash}"
            )
            return 0
        if rebooted and current_hash == metadata.image_sha256:
            last_error = f"new image is present but state is {state!r}"
        elif rebooted:
            raise ValueError(
                "device rebooted into a different image (rollback or wrong binary)"
            )
        else:
            last_error = "device has not rebooted yet"
    raise TimeoutError(
        f"deployment was not verified within {args.reboot_timeout:.0f} seconds: {last_error}"
    )


def _input_self_test(args: argparse.Namespace, token: str) -> int:
    connection = _connection(args.host, args.port, args.cert, args.timeout)
    try:
        connection.request(
            "POST",
            "/diagnostics/input-self-test",
            body=b"",
            headers={
                "Authorization": f"Bearer {token}",
                "Content-Length": "0",
                "Accept": "application/json",
            },
        )
        result, _ = _show_response(connection.getresponse())
        return result
    finally:
        connection.close()


def _add_connection_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--host", required=True, help="device IPv4 address or hostname")
    parser.add_argument("--port", type=int, default=443, help="HTTPS port (default: 443)")
    parser.add_argument(
        "--cert",
        type=Path,
        default=DEFAULT_CERTIFICATE,
        help=f"pinned device certificate (default: {DEFAULT_CERTIFICATE})",
    )
    parser.add_argument(
        "--token-file",
        type=Path,
        default=DEFAULT_TOKEN_FILE,
        help=f"bearer-token file (default: {DEFAULT_TOKEN_FILE})",
    )
    parser.add_argument(
        "--timeout", type=float, default=300.0, help="network timeout in seconds"
    )


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    status = commands.add_parser("status", help="read authenticated OTA status")
    _add_connection_arguments(status)

    report = commands.add_parser(
        "diagnostics-report", help="download a credential-free diagnostics report")
    _add_connection_arguments(report)
    report.add_argument("--output", type=Path, required=True,
                        help="new private JSON file (existing files are never overwritten)")

    config_get = commands.add_parser(
        "config-get", help="read the saved persistent configuration"
    )
    _add_connection_arguments(config_get)
    config_get.add_argument(
        "--output", type=Path, help="also write the JSON object to this file"
    )

    config_put = commands.add_parser(
        "config-put", help="validate and save a complete configuration JSON file"
    )
    _add_connection_arguments(config_put)
    config_put.add_argument("configuration", type=Path, help="configuration JSON file")

    network_get = commands.add_parser(
        "network-get", help="read the saved Ethernet address configuration"
    )
    _add_connection_arguments(network_get)
    network_get.add_argument(
        "--output", type=Path, help="also write the JSON object to this file"
    )

    network_put = commands.add_parser(
        "network-put",
        help="validate and stage a complete Ethernet address configuration",
    )
    _add_connection_arguments(network_put)
    network_put.add_argument("configuration", type=Path, help="configuration JSON file")

    network_confirm = commands.add_parser(
        "network-confirm", help="confirm the active network trial before it rolls back"
    )
    _add_connection_arguments(network_confirm)

    self_test = commands.add_parser(
        "input-self-test",
        help="classify GPIO lines using safe internal weak pulls",
    )
    _add_connection_arguments(self_test)

    reboot = commands.add_parser(
        "reboot",
        help="restart the controller and verify the same image returns",
    )
    _add_connection_arguments(reboot)
    reboot.add_argument(
        "--post-host",
        help="address or hostname expected after reboot (default: current host)",
    )
    reboot.add_argument(
        "--reboot-timeout",
        type=float,
        default=120.0,
        help="seconds to wait for the restarted device (default: 120)",
    )
    reboot.add_argument(
        "--poll-interval",
        type=float,
        default=2.0,
        help="seconds between post-reboot checks (default: 2)",
    )

    upload = commands.add_parser("upload", help="upload an ESP-IDF application image")
    _add_connection_arguments(upload)
    upload.add_argument("firmware", type=Path, help="application .bin from idf.py build")
    upload.add_argument("--signing-public-key", type=Path, default=DEFAULT_PUBLIC_KEY,
                        help="pinned RSA-3072 firmware signing public key")
    upload.add_argument("--allow-unsigned-legacy", action="store_true",
                        help="explicitly permit unsigned images only for a device without signature enforcement")
    upload.add_argument(
        "--project",
        default=DEFAULT_PROJECT,
        help=f"required firmware project name (default: {DEFAULT_PROJECT})",
    )
    upload.add_argument(
        "--no-wait",
        action="store_true",
        help="return after upload acceptance without verifying reboot and rollback state",
    )
    upload.add_argument(
        "--allow-same-image",
        action="store_true",
        help="permit reflashing the exact image already running",
    )
    upload.add_argument(
        "--post-cert",
        type=Path,
        help="pinned certificate expected after reboot (for credential rotation)",
    )
    upload.add_argument(
        "--post-token-file",
        type=Path,
        help="bearer token expected after reboot (for credential rotation)",
    )
    upload.add_argument(
        "--reboot-timeout",
        type=float,
        default=120.0,
        help="seconds to wait for a healthy, validated image (default: 120)",
    )
    upload.add_argument(
        "--poll-interval",
        type=float,
        default=2.0,
        help="seconds between post-reboot checks (default: 2)",
    )
    return parser.parse_args()


def main() -> int:
    args = _arguments()
    try:
        if not args.cert.is_file():
            raise FileNotFoundError(f"certificate not found: {args.cert}")
        token = _read_token(args.token_file)
        if not 1 <= args.port <= 65535:
            raise ValueError("port must be between 1 and 65535")
        if args.timeout <= 0:
            raise ValueError("timeout must be positive")
        if args.command == "status":
            return _status(args, token)
        if args.command == "diagnostics-report":
            return _diagnostics_report(args, token)
        if args.command == "config-get":
            return _config_get(args, token)
        if args.command == "config-put":
            if not args.configuration.is_file():
                raise FileNotFoundError(
                    f"configuration not found: {args.configuration}"
                )
            return _config_put(args, token)
        if args.command == "network-get":
            return _network_get(args, token)
        if args.command == "network-put":
            if not args.configuration.is_file():
                raise FileNotFoundError(
                    f"configuration not found: {args.configuration}"
                )
            return _network_put(args, token)
        if args.command == "network-confirm":
            return _network_confirm(args, token)
        if args.command == "input-self-test":
            return _input_self_test(args, token)
        if args.command == "reboot":
            if args.reboot_timeout <= 0 or args.poll_interval <= 0:
                raise ValueError("reboot-timeout and poll-interval must be positive")
            return _reboot(args, token)
        if not args.firmware.is_file():
            raise FileNotFoundError(f"firmware not found: {args.firmware}")
        if args.reboot_timeout <= 0 or args.poll_interval <= 0:
            raise ValueError("reboot-timeout and poll-interval must be positive")
        return _upload(args, token)
    except (
        OSError,
        TimeoutError,
        UnicodeError,
        ValueError,
        ssl.SSLError,
        http.client.HTTPException,
    ) as error:
        print(f"OTA client error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
