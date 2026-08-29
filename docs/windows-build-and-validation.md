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
installed WDK requires a different KMDF target version, update the project and
INF together rather than suppressing the error.

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
3. Both 10 ms and 44 ms waits occur at PASSIVE_LEVEL.
4. Every DSP busy poll completes before its bound.
5. Startup returns success and the machine remains responsive.
6. Disable/uninstall invokes release, writes `TRPIF=0`, and unmaps the BAR.

Do not continue after a timeout, unexpected resource length, machine-check,
display corruption, spontaneous audio, or any write trace that differs from
`startup-recipe.json`.

## 6. Lifecycle matrix

After one successful start/stop, test each case separately:

- enable, disable, and re-enable;
- warm reboot and cold boot;
- repeated driver update/removal;
- sleep/resume and hibernate/resume;
- surprise removal only if the PCI test platform safely supports it;
- Driver Verifier with relevant KMDF, pool, IRQL, I/O, and deadlock checks.

Start with a small verifier rule set and keep recovery instructions ready. A
Verifier-clean result does not prove hardware correctness.

## 7. Trace comparison

Before adding MIDI, add an internal debug trace containing operation index,
offset, value, and timeout result. Do not expose arbitrary register access to
user mode. Compare the captured sequence mechanically with
`docs/startup-recipe.json`; document any hardware-dependent reads separately.

## 8. MIDI milestone

Only after repeatable initialization:

1. Add UART1 at `BAR+0x3E002` and its command byte at `+1`.
2. Program command sequence `00 00 00 50 4E 10`.
3. Implement interrupt bit 26 and a bounded 8192-byte transmit queue.
4. Add Yamaha logical selector `F5 01` for SWXG1.
5. Replace or extend the diagnostic wrapper with a PortCls MIDI render miniport.
6. Test complete status-bearing Note On/Off messages before running status or
   long SysEx.

First intended transaction:

```text
F5 01 90 3C 40
F5 01 80 3C 00
```

This selects SWXG1, requests middle C, then releases it. It is a controlled
kernel-driver test, never a user-mode BAR write.
