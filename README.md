# SW1000XG modern driver project

Clean-room research and an experimental modern Windows driver for the Yamaha
SW1000XG PCI synthesizer (`PCI\VEN_1073&DEV_1000`). The immediate goal is a
stable Windows 10/11 x64 MIDI driver for the card's onboard 32-part XG tone
generator. PCM, ASIO, and complete legacy-driver parity are later goals.

## Current status

- The Windows 2000/XP driver has been inspected statically; it was never loaded
  or executed during analysis.
- PCI-UART registers, logical synth-port framing, interrupt bits, ordinary DSP
  startup, MPR payloads, DSP RAM writes, and the digital serial interface have
  been reconstructed.
- A platform-neutral C hardware core implements the recovered startup sequence.
- Fake-MMIO tests cover the full sequence and its failure paths.
- An x64 KMDF diagnostic initializer maps the PCI BAR and invokes the core. It
  intentionally exposes no user-mode interface and no MIDI/audio endpoint yet.
- The KMDF source has not yet been compiled with the WDK or run on real hardware.

This is experimental kernel code. Do not install it on a primary machine.

## Repository layout

```text
src/hardware/     Platform-neutral MMIO protocol core
src/kmdf/         Windows KMDF diagnostic initializer and INF
tests/            Host-side fake-MMIO tests
tools/            Driver payload extractor and private asset generator
docs/             Reverse-engineering evidence and validation instructions
```

## Host-side tests

On macOS or Linux:

```text
make test
```

## Private Yamaha assets

This repository does not contain Yamaha's driver or extracted payload bytes. An
owner supplies their own `yswds.sys` version 1.01.0014.1 and extracts it locally:

```text
python3 tools/extract_yswds.py extract /path/to/yswds.sys work/extracted
python3 tools/generate_assets.py work/extracted src/kmdf/sw1000xg_assets.generated.c
```

The generated source is ignored by Git and must not be committed. See
[payload-extractor.md](docs/payload-extractor.md) and obtain appropriate legal
advice before distributing a binary containing Yamaha-derived data.

## Windows build and first test

See [windows-build-and-validation.md](docs/windows-build-and-validation.md).
The safe progression is INF validation, WDK compilation and static analysis,
test signing, then a sacrificial PC with kernel debugging. The first build should
only initialize the PCI device; SWXG1 MIDI comes after initialization traces
match the documented recipe.

## Documentation

- [Reverse-engineering report](docs/reverse-engineering-report.md)
- [DSP/MPR loader protocol](docs/loader-protocol.md)
- [Machine-readable startup recipe](docs/startup-recipe.json)
- [Diagnostic-driver design](docs/kmdf-diagnostic-driver.md)