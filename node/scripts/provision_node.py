#!/usr/bin/env python3
"""Provision one ATtiny3224 factory key in USERROW and export its QR credential."""

from __future__ import annotations

import argparse
import csv
import os
import secrets
import sys
from datetime import datetime, timezone
from pathlib import Path

FACTORY_SIZE = 32
KEY_SIZE = 16
UID_SIZE = 10


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_factory_record(key: bytes) -> bytes:
    if len(key) != KEY_SIZE or not any(key):
        raise ValueError("factory key must contain 16 non-zero-combined bytes")
    record = bytearray(FACTORY_SIZE)
    record[0:4] = b"RSFC"
    record[4] = 1
    record[5] = 1
    record[8:24] = key
    crc = crc16_ccitt(record[:24])
    record[24:26] = crc.to_bytes(2, "little")
    record[26] = (~record[24]) & 0xFF
    record[27] = (~record[25]) & 0xFF
    return bytes(record)


def decode_factory_record(record: bytes) -> bytes | None:
    if len(record) != FACTORY_SIZE or record[:4] != b"RSFC" or record[4:8] != b"\x01\x01\x00\x00":
        return None
    if record[28:] != b"\x00" * 4:
        return None
    crc = int.from_bytes(record[24:26], "little")
    if record[26] != ((~record[24]) & 0xFF) or record[27] != ((~record[25]) & 0xFF):
        return None
    if crc != crc16_ccitt(record[:24]) or not any(record[8:24]):
        return None
    return record[8:24]


def locate_pymcuprog() -> Path:
    platformio_home = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    libs = platformio_home / "packages" / "framework-arduino-megaavr-megatinycore" / "tools" / "libs"
    if not (libs / "pymcuprog").is_dir():
        raise RuntimeError(f"bundled pymcuprog was not found under {libs}")
    return libs


def write_user_row(backend, record: bytes) -> None:
    """Erase and write the complete USERROW page through SerialUPDI.

    pymcuprog routes a generic USER_ROW write through its flash page-write
    implementation.  On the ATtiny3224 that command does not erase the page,
    so it cannot restore zero bits left by an earlier provisioning attempt.
    Use the NVM controller's atomic erase-and-write command instead.
    """
    from pymcuprog.serialupdi import constants

    device_model = backend.programmer.device_model
    device_model.avr.nvm.write_nvm(
        device_model.dut.userrow_address,
        bytearray(record),
        # USERROW is byte-addressable.  Word access through some SerialUPDI
        # adapters writes only the low byte of each word, leaving every odd
        # address erased (0xFF).
        use_word_access=False,
        nvmcommand=constants.UPDI_V0_NVMCTRL_CTRLA_ERASE_WRITE_PAGE,
    )


def provision_over_updi(port: str, baud: int, force: bool) -> tuple[bytes, bytes]:
    sys.path.insert(0, str(locate_pymcuprog()))
    from pymcuprog.backend import Backend, SessionConfig
    from pymcuprog.deviceinfo.memorynames import MemoryNames
    from pymcuprog.toolconnection import ToolSerialConnection

    backend = Backend()
    backend.connect_to_tool(ToolSerialConnection(serialport=port))
    session = SessionConfig("attiny3224")
    session.interface = "updi"
    session.interface_speed = baud
    session.special_options = {
        "chip-erase-locked-device": False,
        "user-row-locked-device": False,
    }
    try:
        backend.start_session(session)
        backend.read_device_id()
        # SIGROW_SERNUM0 is mapped at SIGROW base + 3 on ATtiny3224.
        uid = bytes(backend.programmer.device_model.avr.read_data(0x1103, UID_SIZE))
        current = bytes(backend.read_memory(MemoryNames.USER_ROW, 0, FACTORY_SIZE)[0].data)
        if decode_factory_record(current) is not None and not force:
            raise RuntimeError("node is already provisioned; use --force to replace its factory key")
        key = secrets.token_bytes(KEY_SIZE)
        record = encode_factory_record(key)
        write_user_row(backend, record)
        verified = bytes(backend.read_memory(MemoryNames.USER_ROW, 0, FACTORY_SIZE)[0].data)
        if verified != record or decode_factory_record(verified) != key:
            raise RuntimeError(
                "USERROW verification failed "
                f"(expected {record.hex().upper()}, read {verified.hex().upper()})"
            )
        return uid, key
    finally:
        backend.end_session()
        backend.disconnect_from_tool()


def export_credentials(uid: bytes, key: bytes, output: Path, no_qr: bool) -> str:
    uid_hex = uid.hex().upper()
    key_hex = key.hex().upper()
    uri = (
        "web+opensmartkit:pair?v=1&family=sense"
        f"&uid={uid_hex}&key={key_hex}"
    )
    output.mkdir(parents=True, exist_ok=True)
    credential_path = output / f"{uid_hex}.txt"
    credential_path.write_text(
        f"OSK Sense Node\nUID: {uid_hex}\nFactory key: {key_hex}\nURI: {uri}\n",
        encoding="utf-8",
    )
    manifest_path = output / "manifest.csv"
    new_manifest = not manifest_path.exists()
    with manifest_path.open("a", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        if new_manifest:
            writer.writerow(("uid", "factory_key", "provisioned_at"))
        writer.writerow((uid_hex, key_hex, datetime.now(timezone.utc).isoformat()))
    if not no_qr:
        try:
            import qrcode
            import qrcode.image.svg
        except ImportError as error:
            raise RuntimeError(
                "QR dependency is missing; install scripts/requirements-provisioning.txt "
                "or rerun with --no-qr"
            ) from error
        image = qrcode.make(uri, image_factory=qrcode.image.svg.SvgPathImage, border=4)
        image.save(output / f"{uid_hex}.svg")
    return uri


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM11", help="SerialUPDI port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, default=Path("provisioned_nodes"))
    parser.add_argument("--force", action="store_true", help="replace an existing valid key")
    parser.add_argument("--no-qr", action="store_true", help="export text and manifest only")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        uid, key = provision_over_updi(args.port, args.baud, args.force)
        uri = export_credentials(uid, key, args.output, args.no_qr)
    except Exception as error:
        print(f"Provisioning failed: {error}", file=sys.stderr)
        return 1
    print(f"Provisioned UID {uid.hex().upper()}")
    print(f"Credential: {uri}")
    print(f"Artifacts: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
