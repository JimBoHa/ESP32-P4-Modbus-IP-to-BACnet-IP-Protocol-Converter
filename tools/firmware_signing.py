#!/usr/bin/env python3
"""Pinned RSA-3072 Secure Boot v2 signature verification; no eFuse operations.

Wire format: ESP-IDF v5.5.4 secure-boot-v2 documentation. Cryptographic
verification and key generation use OpenSSL, not a custom RSA implementation.
Production images are signed by ESP-IDF/espsecure during the build.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PRIVATE_KEY = ROOT / "secrets" / "firmware_signing_key.pem"
DEFAULT_PUBLIC_KEY = ROOT / "main" / "ota_signing_public_key.pem"
if Path(__file__).with_name("ota_signing_public_key.pem").is_file():
    DEFAULT_PUBLIC_KEY = Path(__file__).with_name("ota_signing_public_key.pem")
SECTOR_BYTES = 4096
BLOCK_BYTES = 1216


def _openssl(arguments: list[str], data: bytes | None = None) -> bytes:
    result = subprocess.run(
        ["openssl", *arguments], input=data if data is not None else b"",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False,
    )
    if result.returncode:
        # Never echo key material, command output, or sensitive input on failure.
        raise ValueError("OpenSSL key/signature operation failed")
    return result.stdout


def _private_mode(path: Path) -> None:
    if path.is_symlink() or not path.is_file():
        raise ValueError("signing private key must be a regular, non-symlink file")
    if os.name == "posix" and stat.S_IMODE(path.stat().st_mode) & 0o077:
        raise ValueError("signing private key requires mode 0600")


def _exclusive_write(path: Path, data: bytes) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(path, flags, 0o600), "wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def public_der(path: Path) -> bytes:
    # rsa rejects non-RSA keys. Canonical SubjectPublicKeyInfo is used as the pin.
    der = _openssl(["rsa", "-pubin", "-in", str(path), "-pubout", "-outform", "DER"])
    if len(der) != 422:
        raise ValueError("signing public key must be RSA-3072 with exponent 65537")
    return der


def check_key(private_key: Path, public_key: Path) -> None:
    _private_mode(private_key)
    derived = _openssl(["rsa", "-in", str(private_key), "-passin", "pass:",
                        "-pubout", "-outform", "DER"])
    if not hmac.compare_digest(derived, public_der(public_key)):
        raise ValueError("signing private key does not match the pinned public key")


def generate_key(private_key: Path, public_key: Path, backup_dir: Path | None) -> None:
    for target in (private_key, public_key):
        if target.exists() or target.is_symlink():
            raise ValueError(f"refusing to overwrite existing signing key: {target}")
    if backup_dir is not None and (backup_dir.exists() or backup_dir.is_symlink()):
        raise ValueError("signing backup directory must be new")
    private_key.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if private_key.parent.is_symlink():
        raise ValueError("signing secrets directory must not be a symlink")
    if os.name == "posix" and stat.S_IMODE(private_key.parent.stat().st_mode) & 0o077:
        raise ValueError("signing secrets directory requires mode 0700")
    public_key.parent.mkdir(parents=True, exist_ok=True)
    private = _openssl(["genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:3072"])
    public = _openssl(["pkey", "-pubout"], private)
    _exclusive_write(private_key, private)
    _exclusive_write(public_key, public)
    check_key(private_key, public_key)
    if backup_dir is not None:
        backup_dir.mkdir(mode=0o700)
        _exclusive_write(backup_dir / "firmware_signing_key.pem", private)
        _exclusive_write(backup_dir / "ota_signing_public_key.pem", public)
        _exclusive_write(backup_dir / "README.txt", (
            "PRIVATE SIGNING KEY BACKUP. Keep offline or in an approved secret store.\n"
            "This directory is permission-restricted, not encrypted. Never publish it.\n"
            "Losing this key prevents future signed OTA updates; physical recovery remains available.\n"
        ).encode("ascii"))


def _der(tag: int, data: bytes) -> bytes:
    length = len(data)
    encoded = length.to_bytes((length.bit_length() + 7) // 8 or 1, "big")
    return bytes([tag]) + (bytes([length]) if length < 128 else bytes([0x80 | len(encoded)]) + encoded) + data


def _rsa_public_der(modulus: int, exponent: int) -> bytes:
    def integer(value: int) -> bytes:
        encoded = value.to_bytes((value.bit_length() + 7) // 8, "big")
        return _der(2, (b"\0" if encoded[0] & 0x80 else b"") + encoded)
    key = _der(0x30, integer(modulus) + integer(exponent))
    algorithm = bytes.fromhex("300d06092a864886f70d0101010500")
    return _der(0x30, algorithm + _der(3, b"\0" + key))


def verify_image_signature(image: bytes, unsigned_end: int, public_key: Path) -> str:
    """Verify exactly one pinned RSA-PSS block; return the SDK public-key digest."""
    offset = len(image) - SECTOR_BYTES
    if offset <= 0 or offset % SECTOR_BYTES or unsigned_end > offset:
        raise ValueError("firmware has no complete aligned signature sector")
    if any(byte != 0xFF for byte in image[unsigned_end:offset]):
        raise ValueError("firmware has invalid signature padding")
    block = image[offset : offset + BLOCK_BYTES]
    if block[:4] != b"\xe7\x02\0\0":
        raise ValueError("firmware requires an RSA-3072 Secure Boot v2 signature")
    if struct.unpack_from("<I", block, 1196)[0] != zlib.crc32(block[:1196]):
        raise ValueError("firmware signature block CRC mismatch")
    if block[1200:] != bytes(16) or image[offset + BLOCK_BYTES:] != b"\xff" * (SECTOR_BYTES - BLOCK_BYTES):
        raise ValueError("firmware must contain exactly one signature in the first block")
    if not hmac.compare_digest(hashlib.sha256(image[:offset]).digest(), block[4:36]):
        raise ValueError("firmware signed image digest mismatch")
    modulus = int.from_bytes(block[36:420], "little")
    exponent = int.from_bytes(block[420:424], "little")
    if modulus.bit_length() != 3072 or not modulus & 1 or exponent != 65537:
        raise ValueError("firmware signing key must be RSA-3072 with exponent 65537")
    # Validate SDK hardware-acceleration parameters as well as the actual key.
    if int.from_bytes(block[424:808], "little") != pow(2, 6144, modulus) or int.from_bytes(block[808:812], "little") != (-pow(modulus, -1, 1 << 32)) % (1 << 32):
        raise ValueError("firmware signing key acceleration parameters are invalid")
    trusted_der = public_der(public_key)
    if not hmac.compare_digest(_rsa_public_der(modulus, exponent), trusted_der):
        raise ValueError("firmware signature uses an untrusted signing key")
    with tempfile.TemporaryDirectory(prefix="p4-signature-") as temporary:
        directory = Path(temporary)
        key_path, signature_path = directory / "public.der", directory / "signature.bin"
        _exclusive_write(key_path, trusted_der)
        _exclusive_write(signature_path, block[812:1196][::-1])
        _openssl(["dgst", "-sha256", "-verify", str(key_path), "-keyform", "DER",
                  "-signature", str(signature_path), "-sigopt", "rsa_padding_mode:pss",
                  "-sigopt", "rsa_pss_saltlen:32", "-sigopt", "rsa_mgf1_md:sha256"], image[:offset])
    return hashlib.sha256(block[36:812]).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("generate-key", "check-key"))
    parser.add_argument("--private-key", type=Path, default=DEFAULT_PRIVATE_KEY)
    parser.add_argument("--public-key", type=Path, default=DEFAULT_PUBLIC_KEY)
    parser.add_argument("--backup-dir", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "generate-key":
            generate_key(args.private_key, args.public_key, args.backup_dir)
        else:
            check_key(args.private_key, args.public_key)
        print("Signing key verified; private material was not displayed.")
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(2, f"Signing key operation failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
