#!/usr/bin/env python3
"""Expand docs/startup-recipe.json into MMIO writes and compare a trace.

This is a second, independent implementation of the recipe. A trace comes
from the host harness (tests/trace_startup.c) or from the KMDF debug build's
kernel-debugger output; any text before "SWXG " on a line is ignored, so a
raw debugger log can be passed directly.

Reads are hardware-dependent (busy polls) and are not compared, only
summarised. Without --extraction, data words that come from Yamaha payload
files are wildcards; with it, every value is checked exactly.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

PORT0 = 0x3FF00
TRPIF = 0x3FF04
PORT1 = 0x3FF10
DSP_WINDOWS = [0x3F000, 0x3F100, 0x3F200, 0x3F300, 0x3F400]
DIT_DATA = 1 << 27
DIT_LATCH = 1 << 28
DIT_CLOCK = 1 << 29
ANY = None  # wildcard value
MASK = 0xFFFFFFFF


def num(value) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


class Expander:
    def __init__(self, extraction: Path | None):
        self.root = extraction / "sw1000_startup" if extraction else None
        self.port1 = 0
        self.out: list[tuple] = []  # ("W", offset, value|None) or ("D", us)

    def words(self, name: str, count: int) -> list:
        if self.root is None:
            return [ANY] * count
        data = (self.root / name).read_bytes()
        if len(data) != count * 4:
            raise ValueError(f"{name}: expected {count * 4} bytes")
        return list(struct.unpack(f"<{count}I", data))

    def write(self, offset: int, value) -> None:
        self.out.append(("W", offset, value))

    def write_port1(self, value: int) -> None:
        self.port1 = value & MASK
        self.write(PORT1, self.port1)

    def set_port1_bit(self, mask: int, enabled: bool) -> None:
        value = (self.port1 | mask) if enabled else (self.port1 & ~mask)
        # Yamaha's helpers preserve bit 31 from the old shadow.
        value = (value & 0x7FFFFFFF) | (self.port1 & 0x80000000)
        self.write_port1(value)

    def send_words(self, window: int, selector: int, destination: int,
                   words: list) -> None:
        base = DSP_WINDOWS[window]
        while words:
            chunk, words = words[:32], words[32:]
            for i, word in enumerate(chunk):
                self.write(base + 4 * i, word)
            self.write(base + 0x80, ((len(chunk) << 16) | (destination >> 16)) & MASK)
            self.write(base + 0x84, ((destination << 16) | selector) & MASK)
            destination += len(chunk)

    def set_ram(self, address: int, value: int) -> None:
        base = DSP_WINDOWS[0]
        high = address >> 16
        self.write(base + 0x80, (0x01010000 + high) & MASK)
        self.write(base, value)
        self.write(base + 0x80, (0x00010000 + high) & MASK)
        self.write(base + 0x84, ((address << 16) + 0x0F00) & MASK)

    def global_record(self, window: int, name: str) -> None:
        base = DSP_WINDOWS[window]
        if self.root is None:
            values = [ANY, ANY, ANY]
        else:
            record = (self.root / name).read_bytes()
            if len(record) != 18:
                raise ValueError(f"{name}: expected 18 bytes")

            def s16(offset: int) -> int:
                return struct.unpack_from("<h", record, offset)[0]

            values = [((s16(8) << 16) + s16(10)) & MASK,
                      s16(14) & MASK,
                      (s16(16) << 16) & MASK]
        # +0x8E is rounded down to +0x8C by the original's 32-bit access.
        for offset, value in zip((0x88, 0x8C, 0x90), values):
            self.write(base + offset, value)

    def dit_bit(self, bit: bool) -> None:
        self.set_port1_bit(DIT_DATA, bit)
        self.set_port1_bit(DIT_CLOCK, True)
        self.set_port1_bit(DIT_CLOCK, False)

    def dit(self, mode: int, value: int) -> None:
        self.set_port1_bit(DIT_LATCH, False)
        if mode in (0, 2):
            for shift in range(31, -1, -1):
                self.dit_bit(bool(value >> shift & 1))
            self.dit_bit(mode == 2)
            self.dit_bit(False)
        elif mode == 1:
            for shift in range(3, -1, -1):
                self.dit_bit(bool(value >> shift & 1))
            self.dit_bit(False)
            self.dit_bit(True)
        else:
            raise ValueError(f"unknown dit mode {mode}")
        self.set_port1_bit(DIT_LATCH, True)

    def expand(self, recipe: dict) -> list[tuple]:
        for op in recipe["operations"]:
            kind = op["op"]
            window = op.get("window", op.get("dsp", 0))
            if kind == "write32":
                self.write(num(op["offset"]), num(op["value"]))
            elif kind == "write_port1":
                self.write_port1(num(op["value"]))
            elif kind == "delay_ms":
                self.out.append(("D", num(op["value"]) * 1000))
            elif kind == "delay_us":
                self.out.append(("D", num(op["value"])))
            elif kind == "set_port1_bit":
                self.set_port1_bit(1 << op["bit"], bool(op["value"]))
            elif kind == "clear_port1_bit":
                self.set_port1_bit(1 << op["bit"], False)
            elif kind == "mask_port1":
                self.write_port1(self.port1 & num(op["and"]))
            elif kind == "clear_initial_timestamp":
                pass
            elif kind == "write_global_records":
                for i, name in enumerate(op["files"]):
                    self.global_record(i, name)
            elif kind == "set_dsp_word":
                self.send_words(window, num(op["target_selector"]),
                                num(op["destination"]), [num(op["value"])])
            elif kind == "send_words":
                self.send_words(window, num(op.get("target_selector", 0)),
                                num(op["destination"]),
                                self.words(op["file"], op["words"]))
            elif kind == "send_mpr_bank":
                for slot, (name, count) in enumerate(
                        zip(op["files"], op["word_counts"])):
                    self.send_words(window, slot << 8, num(op["destination"]),
                                    self.words(name, count))
            elif kind == "set_ram":
                self.set_ram(num(op["address"]), num(op["value"]))
            elif kind == "dit":
                self.dit(op["mode"], num(op["value"]))
            else:
                raise ValueError(f"unknown recipe op {kind!r}")
        return self.out


TRACE_LINE = re.compile(r"SWXG\s+(W|R|D|END)\b\s*(.*)")


def parse_trace(text: str) -> tuple[list[tuple], dict]:
    actual: list[tuple] = []
    info = {"reads": 0, "read_entries": 0, "end": None}
    for line_no, line in enumerate(text.splitlines(), 1):
        match = TRACE_LINE.search(line)
        if not match:
            continue
        kind, fields = match.group(1), match.group(2).split()
        try:
            if kind == "W":
                actual.append(("W", int(fields[0], 16), int(fields[1], 16), line_no))
            elif kind == "R":
                info["read_entries"] += 1
                info["reads"] += int(fields[2]) if len(fields) > 2 else 1
            elif kind == "D":
                actual.append(("D", int(fields[0]) * 1000, line_no))
            else:
                info["end"] = (int(fields[0]), int(fields[1]))
        except (IndexError, ValueError):
            raise ValueError(f"trace line {line_no}: cannot parse {line!r}")
    return actual, info


def describe(entry: tuple | None) -> str:
    if entry is None:
        return "(nothing)"
    if entry[0] == "D":
        return f"delay >= {entry[1]} us"
    value = "*" if entry[2] is ANY else f"{entry[2]:08X}"
    return f"write {entry[1]:05X} = {value}"


def compare(expected: list[tuple], actual: list[tuple], info: dict) -> int:
    if info["end"] is None:
        print("FAIL: trace has no 'SWXG END' line (truncated or startup did not finish)")
        return 1
    result, dropped = info["end"]
    if dropped:
        print(f"FAIL: trace dropped {dropped} entries; enlarge the trace buffer")
        return 1
    for index in range(max(len(expected), len(actual))):
        want = expected[index] if index < len(expected) else None
        got = actual[index] if index < len(actual) else None
        ok = want is not None and got is not None and want[0] == got[0]
        if ok and want[0] == "W":
            ok = want[1] == got[1] and (want[2] is ANY or want[2] == got[2])
        elif ok:
            ok = got[1] >= want[1]
        if not ok:
            where = f" (trace line {got[-1]})" if got is not None else ""
            print(f"FAIL at operation {index}{where}:")
            print(f"  expected {describe(want)}")
            print(f"  actual   {describe(got[:3] if got else None)}")
            return 1
    if result != 0:
        print(f"FAIL: startup returned {result} after a matching sequence")
        return 1
    wildcards = sum(1 for e in expected if e[0] == "W" and e[2] is ANY)
    print(f"OK: {len(expected)} operations match "
          f"({wildcards} payload values unchecked); "
          f"{info['reads']} reads in {info['read_entries']} entries not compared")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("expected", "compare"))
    parser.add_argument("trace", nargs="?", type=Path,
                        help="trace or debugger log (compare only; '-' for stdin)")
    parser.add_argument("--recipe", type=Path,
                        default=Path(__file__).resolve().parent.parent / "docs" / "startup-recipe.json")
    parser.add_argument("--extraction", type=Path,
                        help="extract_yswds.py output directory, for exact payload values")
    args = parser.parse_intermixed_args()

    recipe = json.loads(args.recipe.read_text("utf-8"))
    expected = Expander(args.extraction).expand(recipe)
    if args.command == "expected":
        for entry in expected:
            if entry[0] == "D":
                print(f"SWXG D >={entry[1]}us")
            else:
                value = "*" if entry[2] is ANY else f"{entry[2]:08X}"
                print(f"SWXG W {entry[1]:05X} {value}")
        return 0
    if args.trace is None:
        parser.error("compare needs a trace file")
    text = sys.stdin.read() if str(args.trace) == "-" else args.trace.read_text("utf-8", "replace")
    actual, info = parse_trace(text)
    return compare(expected, actual, info)


if __name__ == "__main__":
    raise SystemExit(main())
