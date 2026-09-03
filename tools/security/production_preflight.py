#!/usr/bin/env python3
"""Read-only preflight for EyeCare production security artifacts."""

from __future__ import annotations

import argparse
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec, rsa
except ModuleNotFoundError as exc:
    raise SystemExit(
        "missing Python dependency 'cryptography'; run in the reviewed "
        "offline provisioning environment"
    ) from exc


REQUIRED_CONFIG = {
    "CONFIG_EYECARE_PRODUCTION_LOCK": "y",
    "CONFIG_SECURE_BOOT_V2_ENABLED": "y",
    "CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME": "y",
    "CONFIG_SECURE_FLASH_ENCRYPTION_AES256": "y",
    "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE": "y",
    "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE": "y",
    "CONFIG_PARTITION_TABLE_OFFSET": "0x10000",
}

EXPECTED_PARTITIONS = {
    "nvs": (0x11000, 0x6000, False),
    "phy_init": (0x17000, 0x1000, False),
    "factory": (0x20000, 0x100000, False),
    "storage": (0x120000, 0xE00000, True),
    "nvs_keys": (0xF20000, 0x1000, True),
}

PARTITION_ENTRY = struct.Struct("<HBBII16sI")
PARTITION_MAGIC = 0x50AA
PARTITION_FLAG_ENCRYPTED = 1
PRIVATE_MARKERS = (
    b"OPENSSH PRIVATE KEY",
    b"BEGIN PRIVATE KEY",
    b"BEGIN RSA PRIVATE KEY",
    b"BEGIN EC PRIVATE KEY",
)


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def passed(message: str) -> None:
    print(f"PASS: {message}")


def parse_sdkconfig(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


def check_config(path: Path) -> None:
    values = parse_sdkconfig(path)
    wrong = {
        key: (values.get(key), expected)
        for key, expected in REQUIRED_CONFIG.items()
        if values.get(key) != expected
    }
    if wrong:
        fail(f"production sdkconfig mismatch: {wrong}")
    passed("production security config")


def parse_partitions(path: Path) -> dict[str, tuple[int, int, bool]]:
    data = path.read_bytes()
    result: dict[str, tuple[int, int, bool]] = {}
    for offset in range(0, len(data), PARTITION_ENTRY.size):
        entry = data[offset : offset + PARTITION_ENTRY.size]
        if len(entry) != PARTITION_ENTRY.size:
            break
        magic, _type, _subtype, address, size, raw_name, flags = (
            PARTITION_ENTRY.unpack(entry)
        )
        if magic != PARTITION_MAGIC:
            break
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        result[name] = (
            address,
            size,
            bool(flags & PARTITION_FLAG_ENCRYPTED),
        )
    return result


def check_partitions(path: Path) -> None:
    actual = parse_partitions(path)
    for name, expected in EXPECTED_PARTITIONS.items():
        if actual.get(name) != expected:
            fail(
                f"partition {name}: actual={actual.get(name)}, expected={expected}"
            )
    passed("production partition offsets and encryption flags")


def load_keys(
    unlock_key_path: Path, secure_boot_key_path: Path
) -> tuple[ec.EllipticCurvePrivateKey, rsa.RSAPrivateKey]:
    unlock_key = serialization.load_ssh_private_key(
        unlock_key_path.read_bytes(), password=None
    )
    if not isinstance(unlock_key, ec.EllipticCurvePrivateKey) or not isinstance(
        unlock_key.curve, ec.SECP256R1
    ):
        fail("unlock key is not ECDSA P-256")
    secure_boot_key = serialization.load_pem_private_key(
        secure_boot_key_path.read_bytes(), password=None
    )
    if not isinstance(secure_boot_key, rsa.RSAPrivateKey):
        fail("Secure Boot key is not RSA")
    if secure_boot_key.key_size != 3072:
        fail(f"Secure Boot RSA key is {secure_boot_key.key_size} bits, not 3072")
    passed("ECDSA P-256 unlock key and RSA-3072 Secure Boot key")
    return unlock_key, secure_boot_key


def check_embedded_public_key(
    header_path: Path, unlock_key: ec.EllipticCurvePrivateKey
) -> None:
    expected = unlock_key.public_key().public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    header = header_path.read_text(encoding="utf-8")
    actual = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", header))
    if actual != expected:
        fail("embedded unlock public key does not match the signing key")
    passed("embedded unlock public key matches private signing key")


def check_ignored(project: Path, paths: list[Path]) -> None:
    for path in paths:
        result = subprocess.run(
            ["git", "check-ignore", "--quiet", str(path)],
            cwd=project,
            check=False,
        )
        if result.returncode != 0:
            fail(f"private key path is not git-ignored: {path}")
    passed("private key paths are git-ignored")


def check_binary_markers(paths: list[Path]) -> None:
    for path in paths:
        data = path.read_bytes()
        if any(marker in data for marker in PRIVATE_MARKERS):
            fail(f"private-key marker found in {path}")
    passed("production binaries contain no private-key PEM/OpenSSH markers")


def verify_secure_boot_signatures(
    project: Path, key_path: Path, binaries: list[Path]
) -> None:
    idf_path_value = os.environ.get("IDF_PATH")
    if not idf_path_value:
        fail("IDF_PATH is not set; activate ESP-IDF v5.4.4 first")
    espsecure = (
        Path(idf_path_value)
        / "components"
        / "esptool_py"
        / "esptool"
        / "espsecure.py"
    )
    if not espsecure.is_file():
        fail(f"espsecure.py not found under IDF_PATH: {espsecure}")
    for binary in binaries:
        result = subprocess.run(
            [
                sys.executable,
                str(espsecure),
                "verify_signature",
                "--version",
                "2",
                "--keyfile",
                str(key_path),
                str(binary),
            ],
            cwd=project,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if result.returncode != 0:
            fail(f"Secure Boot signature invalid for {binary}:\n{result.stdout}")
    passed("bootloader and application RSA signatures")


def main() -> None:
    script = Path(__file__).resolve()
    default_project = script.parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=default_project)
    parser.add_argument("--build-dir", type=Path, default=Path("build-production-check"))
    parser.add_argument("--sdkconfig", type=Path, default=Path("sdkconfig.production"))
    args = parser.parse_args()

    project = args.project.resolve()
    build = (project / args.build_dir).resolve()
    sdkconfig = (project / args.sdkconfig).resolve()
    unlock_key_path = project / "info/HTML/key/eyecare_unlock_ecdsa_p256"
    secure_boot_key_path = project / "info/HTML/key/secure_boot_signing_key.pem"
    partition_table = build / "partition_table/partition-table.bin"
    app = build / "template-app.bin"
    bootloader = build / "bootloader/bootloader.bin"

    required_files = [
        sdkconfig,
        partition_table,
        app,
        bootloader,
        unlock_key_path,
        secure_boot_key_path,
        project / "main/include/unlock_public_key.h",
    ]
    missing = [str(path) for path in required_files if not path.is_file()]
    if missing:
        fail(f"required file(s) missing: {missing}")

    check_config(sdkconfig)
    check_partitions(partition_table)
    unlock_key, _secure_boot_key = load_keys(
        unlock_key_path, secure_boot_key_path
    )
    check_embedded_public_key(project / "main/include/unlock_public_key.h", unlock_key)
    check_ignored(project, [unlock_key_path, secure_boot_key_path])
    check_binary_markers([app, bootloader])
    verify_secure_boot_signatures(project, secure_boot_key_path, [app, bootloader])
    passed("production preflight complete (no device was accessed)")


if __name__ == "__main__":
    main()
