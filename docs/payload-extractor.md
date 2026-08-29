# SW1000XG payload extractor

This read-only tool extracts known DSP/MPR assets from Yamaha `yswds.sys` version 1.01.0014.1. It never loads or executes the driver.

The supported input has SHA-256:

```text
0e475f6af8a4e23af5d5fd3533533fc50433ae10e98aaab9d96368cda35eb4b4
```

## Usage

Python 3.9 or later is sufficient; no third-party packages are required.

```text
python extract_yswds.py extract /path/to/yswds.sys /path/to/extracted
python extract_yswds.py verify  /path/to/yswds.sys /path/to/extracted
```

The output contains individual components, concatenated research images for each recognized variant, and `manifest.json`. The manifest records source identity, original virtual/file addresses, PE section, size, and SHA-256 for every component. The concatenated files are archival research images, not directly uploadable firmware.

## Current scope

The extractor preserves four symbol-defined DSP families:

- 32-bit base
- 32-bit `MEL`
- 16-bit base
- 16-bit `MEL`

It also extracts the SW1000/DS2416 MPR pointer tables and four auxiliary RAM/timer tables.

The `sw1000_startup` group contains the assets actually used by ordinary
non-ASIO SW1000XG reset: five global-register records, eleven baseline MPR
transfers, and two deliberately overlapping 64-word bootstrap buffers. Their
exact transfer order and register protocol are documented in
`loader-protocol.md` in this documentation directory.

These are research assets, not a standalone firmware format. The meaning of
`MEL` and the large effect-program variants still needs hardware validation. The
tool deliberately refuses unknown SYS hashes instead of applying offsets to an
unverified binary.

## Distribution model

Do not bundle Yamaha's driver or extracted payloads with this tool. An owner supplies their own original `yswds.sys` and performs extraction locally. Obtain appropriate legal advice before distributing a product that depends on proprietary firmware or microcode.
