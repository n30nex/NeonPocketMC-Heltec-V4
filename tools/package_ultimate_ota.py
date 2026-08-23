#!/usr/bin/env python3
"""Build and verify signed NeonPocket Ultimate Web OTA packages."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import subprocess
import tempfile


PREFIX = struct.Struct("<4sHH16s8s16s41sI32s")
HEADER = struct.Struct("<4sHH16s8s16s41sI32s64s")
MAGIC = b"NPU2"
FORMAT_VERSION = 1


def fixed(value: str, size: int, label: str) -> bytes:
    raw = value.encode("ascii")
    if len(raw) >= size:
        raise ValueError(f"{label} must be shorter than {size} ASCII bytes")
    return raw + bytes(size - len(raw))


def run_openssl(*args: str) -> bytes:
    result = subprocess.run(
        ["openssl", *args], check=True, capture_output=True
    )
    return result.stdout


def sign(prefix: bytes, private_key: pathlib.Path) -> bytes:
    with tempfile.NamedTemporaryFile(suffix=".bin") as source:
        source.write(prefix)
        source.flush()
        signature = run_openssl(
            "pkeyutl", "-sign", "-rawin", "-inkey", str(private_key),
            "-in", source.name,
        )
    if len(signature) != 64:
        raise ValueError(f"unexpected Ed25519 signature length: {len(signature)}")
    return signature


def verify_signature(prefix: bytes, signature: bytes, public_key: pathlib.Path) -> None:
    with tempfile.NamedTemporaryFile(suffix=".bin") as source, \
            tempfile.NamedTemporaryFile(suffix=".sig") as sig_file:
        source.write(prefix)
        source.flush()
        sig_file.write(signature)
        sig_file.flush()
        subprocess.run(
            ["openssl", "pkeyutl", "-verify", "-rawin", "-pubin",
             "-inkey", str(public_key), "-in", source.name,
             "-sigfile", sig_file.name],
            check=True, capture_output=True,
        )


def pack(args: argparse.Namespace) -> None:
    application = args.app.read_bytes()
    digest = hashlib.sha256(application).digest()
    prefix = PREFIX.pack(
        MAGIC,
        FORMAT_VERSION,
        HEADER.size,
        fixed(args.target, 16, "target"),
        fixed(args.mode, 8, "mode"),
        fixed(args.version, 16, "version"),
        fixed(args.git_sha, 41, "git SHA"),
        len(application),
        digest,
    )
    package = prefix + sign(prefix, args.key) + application
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(package)
    print(f"wrote {args.output} ({len(package)} bytes)")
    print(f"sha256 {hashlib.sha256(package).hexdigest()}")


def verify(args: argparse.Namespace) -> None:
    package = args.package.read_bytes()
    if len(package) < HEADER.size:
        raise ValueError("package is shorter than the fixed header")
    fields = HEADER.unpack_from(package)
    magic, version, header_size, target, mode, release, git_sha, length, digest, signature = fields
    target = target.split(b"\0", 1)[0].decode("ascii")
    mode = mode.split(b"\0", 1)[0].decode("ascii")
    if magic != MAGIC or version != FORMAT_VERSION or header_size != HEADER.size:
        raise ValueError("invalid Ultimate OTA header")
    if target != args.target or mode != args.mode:
        raise ValueError(f"wrong target/mode: {target}/{mode}")
    application = package[HEADER.size:]
    if len(application) != length:
        raise ValueError("application length does not match header")
    if hashlib.sha256(application).digest() != digest:
        raise ValueError("application SHA-256 mismatch")
    verify_signature(package[:PREFIX.size], signature, args.public_key)
    print(
        f"verified {args.package}: {target}/{mode} "
        f"{release.split(bytes(1), 1)[0].decode('ascii')} "
        f"{git_sha.split(bytes(1), 1)[0].decode('ascii')}"
    )


def self_test() -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        private_key = root / "private.pem"
        public_key = root / "public.pem"
        app = root / "firmware.bin"
        package = root / "firmware.npu"
        run_openssl("genpkey", "-algorithm", "ED25519", "-out", str(private_key))
        run_openssl("pkey", "-in", str(private_key), "-pubout", "-out", str(public_key))
        app.write_bytes(b"\xE9" + bytes(range(1, 128)))
        pack(argparse.Namespace(
            app=app, output=package, key=private_key, target="heltec_rcc6",
            mode="web", version="2.1.0-rc.1", git_sha="1" * 40,
        ))
        verify(argparse.Namespace(
            package=package, public_key=public_key,
            target="heltec_rcc6", mode="web",
        ))
        corrupted = bytearray(package.read_bytes())
        corrupted[-1] ^= 1
        package.write_bytes(corrupted)
        try:
            verify(argparse.Namespace(
                package=package, public_key=public_key,
                target="heltec_rcc6", mode="web",
            ))
        except (ValueError, subprocess.CalledProcessError):
            print("self-test rejected corrupted payload")
        else:
            raise AssertionError("corrupted payload was accepted")


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    pack_parser = commands.add_parser("pack")
    pack_parser.add_argument("--app", type=pathlib.Path, required=True)
    pack_parser.add_argument("--output", type=pathlib.Path, required=True)
    pack_parser.add_argument("--key", type=pathlib.Path, required=True)
    pack_parser.add_argument("--version", required=True)
    pack_parser.add_argument("--git-sha", required=True)
    pack_parser.add_argument("--target", default="heltec_rcc6")
    pack_parser.add_argument("--mode", default="web")
    pack_parser.set_defaults(func=pack)

    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("--package", type=pathlib.Path, required=True)
    verify_parser.add_argument("--public-key", type=pathlib.Path, required=True)
    verify_parser.add_argument("--target", default="heltec_rcc6")
    verify_parser.add_argument("--mode", default="web")
    verify_parser.set_defaults(func=verify)

    self_test_parser = commands.add_parser("self-test")
    self_test_parser.set_defaults(func=lambda _: self_test())
    return root


def main() -> None:
    args = parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
