#!/usr/bin/env python3
"""Fail a build when embedded HTTPS OTA credentials are unusable or unsafe."""

from __future__ import annotations

import argparse
import datetime as dt
import hmac
import os
import shutil
import ssl
import stat
import subprocess
from pathlib import Path


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--certificate", type=Path, required=True)
    parser.add_argument("--private-key", type=Path, required=True)
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--viewer-token-file", type=Path,
                        help="require a distinct, private viewer token")
    parser.add_argument(
        "--minimum-valid-days",
        type=int,
        default=30,
        help="minimum remaining certificate validity (default: 30 days)",
    )
    return parser.parse_args()


def _run(openssl: str, arguments: list[str], input_data: bytes | None = None) -> bytes:
    result = subprocess.run(
        [openssl, *arguments],
        input=input_data,
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        raise ValueError(f"OpenSSL rejected OTA credentials during {' '.join(arguments[:2])}")
    return result.stdout


def _require_private_permissions(path: Path) -> None:
    if os.name != "posix":
        return
    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & 0o077:
        raise ValueError(f"private path has group/other permissions: {path} ({mode:04o})")


def _parse_openssl_time(value: str) -> dt.datetime:
    _, encoded = value.strip().split("=", 1)
    parsed = dt.datetime.strptime(encoded, "%b %d %H:%M:%S %Y %Z")
    return parsed.replace(tzinfo=dt.timezone.utc)


def validate_tokens(token_file: Path, viewer_token_file: Path | None = None) -> None:
    paths = [token_file] if viewer_token_file is None else [token_file, viewer_token_file]
    values = []
    for path in paths:
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"token must be a regular, non-symlink file: {path}")
        _require_private_permissions(path.parent)
        _require_private_permissions(path)
        token = path.read_bytes()
        if not 32 <= len(token) <= 128 or any(not 0x21 <= byte <= 0x7E for byte in token):
            raise ValueError("OTA token must be exactly 32-128 printable ASCII bytes")
        values.append(token)
    if len(values) == 2 and hmac.compare_digest(values[0], values[1]):
        raise ValueError("admin and viewer tokens must be distinct")


def validate(
    certificate: Path,
    private_key: Path,
    token_file: Path,
    minimum_valid_days: int,
    viewer_token_file: Path | None = None,
) -> None:
    if minimum_valid_days < 0:
        raise ValueError("minimum-valid-days cannot be negative")
    for path in (certificate, private_key, token_file):
        if not path.is_file():
            raise FileNotFoundError(f"OTA credential not found: {path}")

    _require_private_permissions(private_key.parent)
    _require_private_permissions(private_key)
    validate_tokens(token_file, viewer_token_file)

    certificate_pem = certificate.read_text(encoding="ascii")
    if certificate_pem.count("-----BEGIN CERTIFICATE-----") != 1:
        raise ValueError("OTA certificate file must contain exactly one PEM certificate")
    ssl.PEM_cert_to_DER_cert(certificate_pem)

    key_pem = private_key.read_text(encoding="ascii")
    if not key_pem.startswith("-----BEGIN PRIVATE KEY-----"):
        raise ValueError("OTA private key must be unencrypted PKCS#8 PEM")

    openssl = shutil.which("openssl")
    if openssl is None:
        raise RuntimeError("openssl is required to validate OTA credentials")

    _run(openssl, ["pkey", "-in", str(private_key), "-check", "-noout"])
    certificate_public_pem = _run(
        openssl, ["x509", "-in", str(certificate), "-pubkey", "-noout"]
    )
    certificate_public_der = _run(
        openssl, ["pkey", "-pubin", "-outform", "DER"], certificate_public_pem
    )
    key_public_der = _run(
        openssl, ["pkey", "-in", str(private_key), "-pubout", "-outform", "DER"]
    )
    if not hmac.compare_digest(certificate_public_der, key_public_der):
        raise ValueError("OTA certificate and private key do not match")

    public_description = _run(
        openssl,
        ["pkey", "-pubin", "-text_pub", "-noout"],
        certificate_public_pem,
    ).decode("ascii", errors="replace")
    if "ASN1 OID: prime256v1" not in public_description:
        raise ValueError("OTA credentials must use a P-256 EC key")

    purpose = _run(
        openssl, ["x509", "-in", str(certificate), "-purpose", "-noout"]
    ).decode("ascii", errors="replace")
    if "SSL server : Yes" not in purpose:
        raise ValueError("OTA certificate is not valid for TLS server authentication")

    dates = _run(
        openssl, ["x509", "-in", str(certificate), "-startdate", "-enddate", "-noout"]
    ).decode("ascii")
    date_lines = dates.splitlines()
    if len(date_lines) != 2:
        raise ValueError("could not read OTA certificate validity dates")
    not_before = _parse_openssl_time(date_lines[0])
    not_after = _parse_openssl_time(date_lines[1])
    now = dt.datetime.now(dt.timezone.utc)
    if now < not_before:
        raise ValueError("OTA certificate is not valid yet")
    minimum_end = now + dt.timedelta(days=minimum_valid_days)
    if not_after < minimum_end:
        raise ValueError(
            f"OTA certificate expires in less than {minimum_valid_days} days"
        )


def main() -> int:
    args = _arguments()
    try:
        validate(
            args.certificate,
            args.private_key,
            args.token_file,
            args.minimum_valid_days,
            args.viewer_token_file,
        )
    except (OSError, RuntimeError, UnicodeError, ValueError, ssl.SSLError) as error:
        print(f"OTA credential validation failed: {error}")
        return 1
    print("OTA credentials valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
