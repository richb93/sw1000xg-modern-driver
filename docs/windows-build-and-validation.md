# Windows build and validation

## 1. Prepare an isolated build machine

Install Visual Studio 2022 with Desktop C++ tools and the current Windows Driver
Kit. Clone the repository to a short local path. Do not begin on the machine that
will host the card.

Keep the original Yamaha package outside the repository. Extract and generate a
private asset unit locally:

```powershell
py tools\extract_yswds.py extract C:\private\yswds.sys work\extracted
py tools\extract_yswds.py verify  C:\private\yswds.sys work\extracted
py tools\generate_assets.py work\extracted src\kmdf\sw1000xg_assets.generated.c
```

The extractor accepts only the analysed SHA-256. Confirm that Git still reports
the generated file as ignored.

## 2. Build

Open `sw1000xg-modern-driver.sln`, select `Debug | x64`, and build. If the
installed WDK requires a different KMDF target version, change
`KMDF_VERSION_MAJOR`/`KMDF_VERSION_MINOR` in the project; StampInf substitutes it
for `$KMDFVERSION$` in the INF. The project targets KMDF 1.31, which is in-box on
Windows 10 version 2004 and later and on Windows 11.

The project uses `sw1000xg_assets.generated.c` when present. Without it, the
placeholder builds but hardware preparation deliberately fails before startup.

Run from a Developer Command Prompt using paths from the installed WDK:

```text
InfVerif.exe /w src\kmdf\sw1000xg_diag.inf
```

Run Visual Studio Code Analysis for Drivers. Resolve warnings; do not blanket
disable rules. Inspect the resulting SYS with `dumpbin /headers` and
`dumpbin /imports` and confirm it is AMD64 and has only expected kernel/KMDF
dependencies.

## 3. Test signing

Generate a development certificate and catalog using Microsoft's current driver
signing guidance. Keep private keys outside the repository. Sign both catalog
and driver with SHA-256 and verify the signatures before moving the package.

Do not publish private keys, generated certificates, SYS/PDB/CAT files, or the
generated asset source.

## 4. Test-host preparation

Use a sacrificial Windows 10/11 x64 PC with:

- a restorable system image and physical access;
- kernel debugging configured to a second machine;
- the SW1000XG installed without the legacy x86 driver;
- test signing enabled only as required for the isolated test;
- automatic reboot after a crash disabled;
- the driver package available locally for recovery/removal.

Record the PCI configuration and assigned resources before installation. The
driver requires a translated memory resource at least `0x3FF14` bytes long.

## 5. First installation

The diagnostic build has no public device interface and no MIDI/audio endpoint.
Install it through Device Manager or `pnputil` only after the INF and signatures
verify. Keep the kernel debugger attached.

Expected success criteria:

1. `EvtDevicePrepareHardware` locates exactly one suitable memory resource.
2. The BAR maps successfully as non-cached memory.
3. `EvtDeviceD0Entry` writes `TRPIF=0` before startup begins.
4. Both the 10 ms and 1 ms waits occur at PASSIVE_LEVEL.
5. Every DSP busy poll completes before its bound.
6. Startup returns success and the machine remains responsive.
7. Disable/uninstall invokes `EvtDeviceD0Exit`, which writes `TRPIF=0`, and
   then `EvtDeviceReleaseHardware`, which unmaps the BAR.

Do not continue after a timeout, unexpected resource length, machine-check,
display corruption, spontaneous audio, or any write trace that differs from
`startup-recipe.json`.

## 6. Lifecycle matrix

After one successful start/stop, test each case separately:

- enable, disable, and re-enable;
- warm reboot and cold boot;
- repeated driver update/removal;
- sleep/resume and hibernate/resume (startup must run again on each D0 entry);
- surprise removal only if the PCI test platform safely supports it;
- Driver Verifier with relevant KMDF, pool, IRQL, I/O, and deadlock checks.

Start with a small verifier rule set and keep recovery instructions ready. A
Verifier-clean result does not prove hardware correctness.

## 7. Trace comparison

The `Debug` build records every MMIO write, poll read and delay made by
startup and prints it to the kernel debugger as `SWXG ...` lines. Nothing is
exposed to user mode. Before installing, enable the output in the debugger:

```text
ed nt!Kd_IHVDRIVER_Mask 0xF
.logopen C:\private\swxg-startup.log
```

After the device starts, close the log (`.logclose`) and compare it with an
independent expansion of the recipe:

```text
py tools\recipe_trace.py compare --extraction work\extracted C:\private\swxg-startup.log
```

`--extraction` checks every payload word exactly; without it, payload values
are wildcards. Debugger prefixes on each line are ignored. The tool reports
the first differing operation, or `OK` with a count of reads (busy polls),
which are hardware-dependent and not compared. Keep the log private, because
it contains Yamaha-derived payload values. Do not continue to MIDI work until
this reports `OK` on repeated cold and warm starts, and after resume (each D0
entry prints a new trace).

`make test` runs the same comparison on the host against the fake BAR, so the
C core and the JSON recipe are checked against each other on every change.

## 8. MIDI milestone

Only after repeatable initialization:

1. Add UART1 at `BAR+0x3E002` and its command byte at `+1`.
2. Program command sequence `00 00 00 50 4E 10`, waiting 36 µs after each
   byte, then enable TX with `0x11`.
3. Implement interrupt bit 26 and a bounded 8192-byte transmit queue. Status
   bit `0x01` means transmit-ready; mask bit 26 in `TRPIF` whenever the queue
   is empty, because the original never acknowledges it any other way.
4. Wait until at least 10 s after DSP reset release (`SWXG_H8_BOOT_MS`)
   before the first SWXG byte. Yamaha's driver blocks stream creation for this
   long while the XG section boots.
5. Add Yamaha logical selector `F5 01` for SWXG1.
6. Replace or extend the diagnostic wrapper with a PortCls MIDI render miniport.
7. Test complete status-bearing Note On/Off messages before running status or
   long SysEx.

First intended transaction:

```text
F5 01 90 3C 40
F5 01 80 3C 00
```

This selects SWXG1, requests middle C, then releases it. It is a controlled
kernel-driver test, never a user-mode BAR write.
