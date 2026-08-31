#!/usr/bin/env python3
"""Generate P-256 SSH keys and shared EyeCare unlock tokens.

The token is not device-bound: any device that can verify the P-256 signature
against the embedded public key accepts the same token. Only the private
signing key (offline) can produce a valid token.
"""

from __future__ import annotations

import argparse
import os
import secrets
import struct
from pathlib import Path

try:
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
except ModuleNotFoundError as exc:
    raise SystemExit(
        "missing Python dependency 'cryptography'; install it in the "
        "offline provisioning environment"
    ) from exc

MAGIC = b"EYEUNLK1"
VERSION = 1
KEY_ID = 1
PAYLOAD = struct.Struct("<8sBB2x16s")


def load_p256_private(path: Path) -> ec.EllipticCurvePrivateKey:
    key = serialization.load_ssh_private_key(path.read_bytes(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(
        key.curve, ec.SECP256R1
    ):
        raise SystemExit(
            "unlock signing requires an ECDSA P-256 OpenSSH key; "
            "Ed25519 is not supported by ESP-IDF 5.4.4 Mbed TLS"
        )
    return key


def load_p256_public(path: Path) -> ec.EllipticCurvePublicKey:
    key = serialization.load_ssh_public_key(path.read_bytes())
    if not isinstance(key, ec.EllipticCurvePublicKey) or not isinstance(
        key.curve, ec.SECP256R1
    ):
        raise SystemExit("unlock verification requires an ECDSA P-256 SSH public key")
    return key


def verify_token_data(
    public_key: ec.EllipticCurvePublicKey,
    token: bytes,
) -> None:
    payload_size = PAYLOAD.size
    if len(token) <= payload_size + 1:
        raise SystemExit("token is truncated")
    magic, version, key_id, _nonce = PAYLOAD.unpack(token[:payload_size])
    if magic != MAGIC or version != VERSION or key_id != KEY_ID:
        raise SystemExit("token header is invalid")
    signature_size = token[payload_size]
    signature = token[payload_size + 1 :]
    if not 1 <= signature_size <= 72 or len(signature) != signature_size:
        raise SystemExit("token signature length is invalid")
    try:
        public_key.verify(
            signature, token[:payload_size], ec.ECDSA(hashes.SHA256())
        )
    except InvalidSignature as exc:
        raise SystemExit("token signature is invalid") from exc


def generate_key(path: Path) -> None:
    if path.exists() or path.with_name(path.name + ".pub").exists():
        raise SystemExit(f"refusing to overwrite existing key: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    private_data = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.OpenSSH,
        serialization.NoEncryption(),
    )
    public_data = key.public_key().public_bytes(
        serialization.Encoding.OpenSSH,
        serialization.PublicFormat.OpenSSH,
    )
    path.write_bytes(private_data)
    path.with_name(path.name + ".pub").write_bytes(
        public_data + b" eyecare-production-unlock\n"
    )
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    print(f"generated private key: {path}")
    print(f"generated public key:  {path}.pub")


def issue_token(key_path: Path, output: Path) -> None:
    key = load_p256_private(key_path)
    payload = PAYLOAD.pack(MAGIC, VERSION, KEY_ID, secrets.token_bytes(16))
    signature = key.sign(payload, ec.ECDSA(hashes.SHA256()))
    if len(signature) > 72:
        raise SystemExit("unexpected P-256 DER signature length")
    output.parent.mkdir(parents=True, exist_ok=True)
    token = payload + bytes([len(signature)]) + signature
    verify_token_data(key.public_key(), token)
    output.write_bytes(token)
    print(f"issued shared unlock token: {output}")


def verify_token(public_key_path: Path, token_path: Path) -> None:
    verify_token_data(load_p256_public(public_key_path), token_path.read_bytes())
    print("valid unlock token (shared, not device-bound)")


def export_public(key_path: Path, output: Path) -> None:
    key = load_p256_private(key_path)
    der = key.public_key().public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    rows = []
    for offset in range(0, len(der), 12):
        values = der[offset : offset + 12]
        rows.append("    " + ", ".join(f"0x{value:02x}" for value in values) + ",")
    text = """#pragma once

#include <stddef.h>
#include <stdint.h>

/* Generated public key only; never place the matching private key in firmware. */
static const uint8_t EYECARE_UNLOCK_PUBLIC_KEY_DER[] = {
%s
};

static const size_t EYECARE_UNLOCK_PUBLIC_KEY_DER_LEN =
    sizeof(EYECARE_UNLOCK_PUBLIC_KEY_DER);
""" % "\n".join(rows)
    output.write_text(text, encoding="utf-8", newline="\n")
    print(f"exported public-key header: {output}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    generate = sub.add_parser("generate-key")
    generate.add_argument("--private-key", type=Path, required=True)

    issue = sub.add_parser("issue")
    issue.add_argument("--private-key", type=Path, required=True)
    issue.add_argument("--output", type=Path, required=True)

    export = sub.add_parser("export-public")
    export.add_argument("--private-key", type=Path, required=True)
    export.add_argument("--output", type=Path, required=True)

    verify = sub.add_parser("verify")
    verify.add_argument("--public-key", type=Path, required=True)
    verify.add_argument("--token", type=Path, required=True)

    args = parser.parse_args()
    if args.command == "generate-key":
        generate_key(args.private_key)
    elif args.command == "issue":
        issue_token(args.private_key, args.output)
    elif args.command == "export-public":
        export_public(args.private_key, args.output)
    else:
        verify_token(args.public_key, args.token)


if __name__ == "__main__":
    main()
