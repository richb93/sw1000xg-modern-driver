#!/usr/bin/env python3
"""Generate a private C asset unit from an extractor sw1000_startup directory."""

import argparse
import hashlib
import json
import struct
from pathlib import Path

EXPECTED_SOURCE = "0e475f6af8a4e23af5d5fd3533533fc50433ae10e98aaab9d96368cda35eb4b4"
MPR_WORDS = [0x140,0x140,0x140,0x140,0x0A0,0x140,0x140,0x040,0x006,0x020,0x040]

def words(path: Path, count: int) -> list[int]:
    data = path.read_bytes()
    if len(data) != count * 4:
        raise ValueError(f"{path}: expected {count * 4} bytes, got {len(data)}")
    return list(struct.unpack(f"<{count}I", data))

def byte_array(name: str, data: bytes) -> str:
    values = ",".join(f"0x{x:02X}" for x in data)
    return f"static const uint8_t {name}[{len(data)}] = {{{values}}};\n"

def word_array(name: str, values: list[int]) -> str:
    rows = []
    for i in range(0, len(values), 8):
        rows.append("    " + ", ".join(f"0x{x:08X}u" for x in values[i:i+8]))
    return f"static const uint32_t {name}[{len(values)}] = {{\n" + ",\n".join(rows) + "\n};\n"

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("extraction", type=Path, help="extractor output directory")
    parser.add_argument("output", type=Path, help="generated .c path")
    args = parser.parse_args()
    manifest = json.loads((args.extraction / "manifest.json").read_text("utf-8"))
    if manifest["source"]["sha256"] != EXPECTED_SOURCE:
        raise ValueError("manifest is not for the supported Yamaha driver")
    root = args.extraction / "sw1000_startup"
    out = ['#include "sw1000xg_assets.generated.h"\n']
    for i in range(5):
        data = (root / f"global_register_{i:02d}.bin").read_bytes()
        if len(data) != 18: raise ValueError("invalid global record size")
        out.append(byte_array(f"global_{i}", data))
    out.append(word_array("bootstrap_a", words(root / "bootstrap_zero_a.bin", 64)))
    for i, count in enumerate(MPR_WORDS):
        out.append(word_array(f"mpr_{i:02d}", words(root / f"mpr_{i:02d}.bin", count)))
    out.append(word_array("bootstrap_b", words(root / "bootstrap_zero_b.bin", 64)))
    out.append(word_array("cescr", words(root / "cescr.bin", 6)))
    out.append("static const swxg_startup_assets assets = {\n")
    out.append("    {global_0,global_1,global_2,global_3,global_4},\n")
    out.append("    bootstrap_a,\n")
    out.append("    {" + ",".join(f"mpr_{i:02d}" for i in range(11)) + "},\n")
    out.append("    bootstrap_b, cescr\n};\n")
    out.append("const swxg_startup_assets *SwxgGetStartupAssets(void) { return &assets; }\n")
    text = "".join(out)
    args.output.write_text(text, "utf-8")
    print(f"generated {args.output} ({len(text.encode())} bytes, sha256 {hashlib.sha256(text.encode()).hexdigest()})")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
