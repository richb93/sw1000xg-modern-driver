#!/usr/bin/env python3
"""Extract known DSP/MPR assets from Yamaha yswds.sys without executing it."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path


SUPPORTED_SHA256 = "0e475f6af8a4e23af5d5fd3533533fc50433ae10e98aaab9d96368cda35eb4b4"

# Virtual addresses are for image base 0x10000 in yswds.sys 1.01.0014.1.
# Adjacent public symbols retained in the binary establish the boundaries.
REGIONS = {
    # Assets used by the ordinary (non-ASIO) SW1000XG reset path.
    "sw1000_startup": {
        "global_register_00.bin": (0x13AE4, 0x0012),
        "global_register_01.bin": (0x15E60, 0x0012),
        "global_register_02.bin": (0x181DC, 0x0012),
        "global_register_03.bin": (0x1A558, 0x0012),
        "global_register_04.bin": (0x1C8D4, 0x0012),
        "bootstrap_zero_a.bin": (0x138C0, 0x0100),
        "mpr_00.bin": (0x12DBC, 0x0500),
        "mpr_01.bin": (0x128BC, 0x0500),
        "mpr_02.bin": (0x123BC, 0x0500),
        "mpr_03.bin": (0x11C3C, 0x0500),
        "mpr_04.bin": (0x1213C, 0x0280),
        "mpr_05.bin": (0x1123C, 0x0500),
        "mpr_06.bin": (0x1173C, 0x0500),
        "mpr_07.bin": (0x137BC, 0x0100),
        "mpr_08.bin": (0x13AB4, 0x0018),
        "mpr_09.bin": (0x13A04, 0x0080),
        "mpr_10.bin": (0x13904, 0x0100),
        # This begins one word before bootstrap_zero_a, as in the original.
        "bootstrap_zero_b.bin": (0x138BC, 0x0100),
        "cescr.bin": (0x13A84, 0x0018),
    },
    "dsp32_base": {
        "dsp000.bin": (0x23DC0, 0x1180),
        "deq000.bin": (0x24F40, 0x0A00),
        "bcr000.bin": (0x25940, 0x0500),
        "mod000.bin": (0x25E40, 0x0160),
        "localc000.bin": (0x25FA0, 0x0038),
        "dsp30.bin": (0x25FD8, 0x0020),
    },
    "dsp32_mel": {
        "dsp000MEL.bin": (0x25FF8, 0x1180),
        "deq000MEL.bin": (0x27178, 0x0A00),
        "bcr000MEL.bin": (0x27B78, 0x0500),
        "mod000MEL.bin": (0x28078, 0x0160),
        "localc000MEL.bin": (0x281D8, 0x0038),
        "dsp30MEL.bin": (0x28210, 0x0020),
    },
    "dsp16_base": {
        "dsp000_16.bin": (0x28230, 0x1180),
        "deq000_16.bin": (0x293B0, 0x0A00),
        "bcr000_16.bin": (0x29DB0, 0x0500),
        "mod000_16.bin": (0x2A2B0, 0x02E0),
        "localc000_16.bin": (0x2A590, 0x0038),
        "dsp30_16.bin": (0x2A5C8, 0x0020),
    },
    "dsp16_mel": {
        "dsp000MEL_16.bin": (0x2A5E8, 0x1180),
        "deq000MEL_16.bin": (0x2B768, 0x0A00),
        "bcr000MEL_16.bin": (0x2C168, 0x0500),
        "mod000MEL_16.bin": (0x2C668, 0x02E0),
        "localc000MEL_16.bin": (0x2C948, 0x0038),
        "dsp30MEL_16.bin": (0x2C980, 0x0020),
    },
    "tables": {
        "mpr_ptr_ds2416.bin": (0x30528, 0x00DC),
        "mpr_ptr_sw1000.bin": (0x30604, 0x00DC),
        "fgRam000.bin": (0x30720, 0x0100),
        "fgTimer000.bin": (0x30820, 0x0080),
        "fgRam000MEL.bin": (0x308A0, 0x0100),
        "fgTimer000MEL.bin": (0x309A0, 0x0090),
    },
}


class ExtractError(Exception):
    pass


def u16(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise ExtractError("truncated PE structure")
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise ExtractError("truncated PE structure")
    return struct.unpack_from("<I", data, offset)[0]


def parse_pe(data: bytes) -> dict:
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ExtractError("input is not an MZ executable")
    pe = u32(data, 0x3C)
    if pe + 24 > len(data) or data[pe : pe + 4] != b"PE\0\0":
        raise ExtractError("input has no valid PE header")
    machine = u16(data, pe + 4)
    section_count = u16(data, pe + 6)
    optional_size = u16(data, pe + 20)
    optional = pe + 24
    if u16(data, optional) != 0x10B:
        raise ExtractError("expected a PE32 image")
    image_base = u32(data, optional + 28)
    entry_rva = u32(data, optional + 16)
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        offset = section_table + index * 40
        if offset + 40 > len(data):
            raise ExtractError("truncated PE section table")
        name = data[offset : offset + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size = u32(data, offset + 8)
        virtual_address = u32(data, offset + 12)
        raw_size = u32(data, offset + 16)
        raw_offset = u32(data, offset + 20)
        if raw_offset + raw_size > len(data):
            raise ExtractError(f"section {name!r} extends beyond the input")
        sections.append(
            {
                "name": name,
                "virtual_size": virtual_size,
                "rva": virtual_address,
                "raw_size": raw_size,
                "raw_offset": raw_offset,
            }
        )
    return {
        "machine": machine,
        "image_base": image_base,
        "entry_rva": entry_rva,
        "sections": sections,
    }


def va_to_file(pe: dict, va: int, size: int) -> tuple[int, str]:
    rva = va - pe["image_base"]
    if rva < 0:
        raise ExtractError(f"VA 0x{va:X} is below image base")
    for section in pe["sections"]:
        start = section["rva"]
        available = section["raw_size"]
        if start <= rva and rva + size <= start + available:
            return section["raw_offset"] + rva - start, section["name"]
    raise ExtractError(f"VA 0x{va:X} size 0x{size:X} is not backed by one PE section")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def extract(source: Path, destination: Path) -> dict:
    data = source.read_bytes()
    source_hash = digest(data)
    if source_hash != SUPPORTED_SHA256:
        raise ExtractError(
            "unsupported yswds.sys: SHA-256 is "
            f"{source_hash}; expected {SUPPORTED_SHA256}"
        )
    pe = parse_pe(data)
    if pe["machine"] != 0x14C or pe["image_base"] != 0x10000:
        raise ExtractError("supported hash has unexpected PE architecture or image base")

    destination.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": 1,
        "tool": "sw1000xg-payload-extractor",
        "source": {
            "filename": source.name,
            "sha256": source_hash,
            "size": len(data),
            "known_version": "1.01.0014.1",
        },
        "pe": {
            "machine": "i386",
            "image_base": pe["image_base"],
            "entry_rva": pe["entry_rva"],
        },
        "warning": (
            "Research extraction only. Variant names preserve Yamaha public-symbol names; "
            "MEL meaning still requires verification. The sw1000_startup group is the "
            "statically recovered ordinary-startup subset; use only with the documented "
            "loader protocol and validate on sacrificial hardware."
        ),
        "groups": {},
    }

    for group_name, files in REGIONS.items():
        group_dir = destination / group_name
        group_dir.mkdir(exist_ok=True)
        records = []
        combined = bytearray()
        for filename, (va, size) in files.items():
            file_offset, section_name = va_to_file(pe, va, size)
            payload = data[file_offset : file_offset + size]
            if len(payload) != size:
                raise ExtractError(f"short extraction for {filename}")
            (group_dir / filename).write_bytes(payload)
            combined.extend(payload)
            records.append(
                {
                    "file": f"{group_name}/{filename}",
                    "virtual_address": f"0x{va:08X}",
                    "file_offset": f"0x{file_offset:08X}",
                    "section": section_name,
                    "size": size,
                    "sha256": digest(payload),
                }
            )
        combined_name = f"{group_name}.combined.bin"
        combined_bytes = bytes(combined)
        (destination / combined_name).write_bytes(combined_bytes)
        manifest["groups"][group_name] = {
            "components": records,
            "combined_file": combined_name,
            "combined_size": len(combined_bytes),
            "combined_sha256": digest(combined_bytes),
        }

    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    (destination / "manifest.json").write_bytes(manifest_bytes)
    return manifest


def verify(source: Path, extracted: Path) -> None:
    data = source.read_bytes()
    if digest(data) != SUPPORTED_SHA256:
        raise ExtractError("source hash is not the supported yswds.sys version")
    pe = parse_pe(data)
    manifest = json.loads((extracted / "manifest.json").read_text("utf-8"))
    if manifest["source"]["sha256"] != digest(data):
        raise ExtractError("manifest source hash does not match source")
    for group_name, group in manifest["groups"].items():
        combined = bytearray()
        for record in group["components"]:
            path = extracted / record["file"]
            payload = path.read_bytes()
            if len(payload) != record["size"] or digest(payload) != record["sha256"]:
                raise ExtractError(f"extracted file failed verification: {path}")
            va = int(record["virtual_address"], 16)
            offset, _ = va_to_file(pe, va, record["size"])
            if payload != data[offset : offset + record["size"]]:
                raise ExtractError(f"extracted file differs from source: {path}")
            combined.extend(payload)
        combined_path = extracted / group["combined_file"]
        combined_bytes = combined_path.read_bytes()
        if combined_bytes != bytes(combined) or digest(combined_bytes) != group["combined_sha256"]:
            raise ExtractError(f"combined group failed verification: {group_name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    extract_parser = subparsers.add_parser("extract", help="extract the supported driver")
    extract_parser.add_argument("source", type=Path, help="path to yswds.sys")
    extract_parser.add_argument("destination", type=Path, help="new or existing output directory")
    verify_parser = subparsers.add_parser("verify", help="verify a previous extraction")
    verify_parser.add_argument("source", type=Path, help="path to the same yswds.sys")
    verify_parser.add_argument("extracted", type=Path, help="extraction directory")
    args = parser.parse_args()
    try:
        if args.command == "extract":
            manifest = extract(args.source, args.destination)
            print(f"Extracted {sum(len(g['components']) for g in manifest['groups'].values())} components")
            print(f"Manifest: {args.destination / 'manifest.json'}")
        else:
            verify(args.source, args.extracted)
            print("Verification succeeded")
        return 0
    except (ExtractError, OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
