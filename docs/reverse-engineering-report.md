# Yamaha SW1000XG XP driver: static reverse-engineering report

Date: 28 August 2026  
Package examined: `Yamaha_SW10000XG_Win2000-XP`  
Method: static inspection only. No driver, installer, DLL, or utility was executed.

## Executive assessment

A clean Windows 10/11 x64 driver is technically feasible, but full feature parity is a medium-to-large reverse-engineering project rather than a simple port. The binary is a favorable target: an x86 C++ PortCls/WDM audio driver with extensive diagnostic strings, class/method names, and separate code paths for SW1000XG and DS2416. PCI discovery, PnP, MMIO mapping, interrupt synchronization, cyclic PCM streaming, mixer topology, MIDI UART, power management, and ASIO integration are all identifiable.

The important correction to the initial premise is that the card is not operated solely through fixed logic. `yswds.sys` contains compiled-in DSP/MPR program and data tables (`dsp0`–`dsp4`, `dsp30`, 16/32-bit and SW1000/DS2416 variants) and routines named `SendDSP`, `SendMpr`, `initMprDsp`, `InitDSPAsio`, and `dspInitAsio`. No separate firmware file is loaded, but the kernel binary appears to upload programs/data into on-card processing memory. Basic PCM/MIDI bring-up may be possible without reproducing every program; complete routing, mixing, and ASIO behavior probably is not.

## Package and provenance

| File | Static identification | SHA-256 |
|---|---|---|
| `yswds.sys` | PE32, Intel 80386, NT native kernel driver | `0e475f6af8a4e23af5d5fd3533533fc50433ae10e98aaab9d96368cda35eb4b4` |
| `sw1000xg.inf` | SW1000XG installation manifest | `d402b5acccbc4be9bafa0e1a664963462d2c7d58bf2f634da8308c227e28bb06` |
| `ds2416.inf` | DS2416 variant of the same driver manifest | `1fbee348e558ebf49d8cc434123937cc8d3cf2c01ff20950a1b5b40383596ab5` |
| `SETUP.EXE` | 32-bit InstallShield self-extracting installer, described by the readme as the ASIO installer | `afaec115fda58d25614b0d1665847a6d08b73ec8ef073a34066c2ee542ace5b6` |
| `UPDATE.EXE` | 32-bit InstallShield self-extracting installer for WDM update | `4b4c2b33b9f7ff8cbf42fc21d8dc31ccc8d2f62aad27cfe026292f4b0d49e19b` |
| `readme.txt` | Driver/readme dated 28 August 2002 | `97239b1255521bf953a1c8ebdecba883e4e3179d6fd8980cb16a88797e646534` |

The SYS version resource says `1.01.0014.1`, Yamaha copyright 1999–2002, and “DS2416, SW1000XG Adapter Driver.” Its PE timestamp is 30 January 2002; the readme calls the release 1.01.0014.1 and says it was created 6 August 2002. It is unsigned and has no PE security directory.

The two InstallShield executables were inspected only as outer PE files and string containers. Their embedded payloads were not available as separate files and were not executed. Consequently, this package does not yet provide the user-mode ASIO DLL for detailed analysis.

## Architecture and driver model

Confirmed:

- 32-bit x86, linked with Microsoft Visual C++ 6-era tools (`MajorLinkerVersion 6.0`) and Windows 2000 DDK libraries.
- PortCls/WDM adapter/miniport design, initialized with `PcInitializeAdapterDriver` and `PcAddAdapterDevice`.
- PortCls subdevices are constructed with `PcNewPort`, `PcNewMiniport`, `PcRegisterSubdevice`, and `PcRegisterPhysicalConnection`.
- Wave audio uses the legacy `IMiniportWaveCyclic` / `IMiniportWaveCyclicStream` model, not WaveRT.
- Separate topology and MIDI miniports are present. MIDI uses `IMiniportMidi`, `IMiniportMidiStream`, `IPortMidi`, and a class named `CMiniportMidiUartSWXG`.
- An adapter-level `IAdapterPowerManagement` implementation handles D-states.
- The same binary supports SW1000XG and DS2416, selected by PCI identity and separate `StartDevice`, adapter, topology, and resource-assignment paths.

The PE entry point is RVA `0x26d40` (virtual address `0x36d40` at the preferred base). It is a small `DriverEntry` thunk into PortCls initialization. Diagnostic strings identify `DriverEntry`, `AddDevice`, `StartDevice`, `SW1000_StartDevice`, `DS2416_StartDevice`, `InstallSubdevice`, and `SW1000_AssignResources`/`DS2416_AssignResources`.

## PCI identity and exposed endpoints

- SW1000XG: `PCI\VEN_1073&DEV_1000`
- DS2416: `PCI\VEN_1073&DEV_2000`
- No subsystem-ID restriction appears in the INF.
- Service: `yswds`, kernel driver, system-start in the old INF.

The SW1000XG INF registers:

- Two capture endpoints: `WAVEIN1` and `WAVEIN2`.
- Six PCM render endpoints: `WAVEOUT1` through `WAVEOUT6`.
- Three synthesizer render endpoints: `SWXG1` through `SWXG3`.
- One external MIDI output and one MIDI input.
- One topology/mixer endpoint.

The old INF is not suitable for modern Windows as-is: it has `$CHICAGO$` syntax, Windows 9x sections, legacy `wdmaud`/`sbemul` registrations, no architecture decorations, no catalog, and a system-start service.

## Hardware access, resources, interrupts, and DMA

Confirmed from imports and named routines:

- The adapter consumes a translated memory resource, logs its 64-bit start and length, and maps it with `MmMapIoSpace`; teardown uses `MmUnmapIoSpace`.
- Hardware accesses are byte-oriented MMIO through `READ_REGISTER_UCHAR` and `WRITE_REGISTER_UCHAR`. No imported port-I/O helpers were found.
- Interrupt handling uses `PcNewInterruptSync`/`IInterruptSync`, an adapter ISR, synchronized callback helpers, spin locks, DPCs, and service groups. Diagnostic strings show a shared interrupt status named `TRPIF`, per-wave-in/out flags, MIDI ISR processing, and a separate ASIO DPC.
- PCM streams receive an `IDmaChannel` from PortCls. Named methods include `WaveDmaRegIniz`, `WaveDmaRegDeiniz`, `WaveDmaRegReset`, `WaveDmaRegSetFrameSize`, `startTX`, `startREC`, position getters, and synchronized start/standby/stop functions.
- `SetWaveCommonBuffer` and `GetWaveCommonBuffer` retain a physical address, virtual address, and length. This strongly indicates the device is programmed with a common/cyclic buffer address rather than a modern scatter/gather descriptor list.
- ASIO code maps locked pages and MDLs into user space and exposes buffer-address operations. This legacy mechanism should not be copied into a new kernel interface without a fresh security design.

Likely, but not yet proven:

- The PCI device probably exposes one principal MMIO BAR plus one interrupt; the exact resource-list counts and BAR layout need deeper function-level decompilation or observation on hardware.
- The DMA engine is likely limited to 32-bit physical addresses. The old x86 design and absence of 64-bit DMA handling make this the safe initial assumption, but a real card test or chipset documentation must confirm it.
- `TRW*`, `TP`, `IA`, `EA`, `APP/APF`, and `ARP/ARF` are DMA ring/transfer address and pointer registers. Their exact semantics and offsets remain unresolved.

No reliable register-offset table should yet be published. The binary clearly contains the necessary access logic, but the current pass has not assigned each byte offset and bit mask with sufficient confidence. That is the main next reverse-engineering deliverable.

## MIDI, synthesizer, mixer, and digital paths

MIDI:

- The external MIDI path is a PCI-attached UART abstraction (`CPciUart`) with command/status/data operations, TX/RX enable, queueing, timeout handling, and interrupt-driven input/output.
- It exposes at most one render and one capture stream at a time and resets MPU hardware when both counts return to zero.
- Three separately named SWXG render filters exist in the INF, but Yamaha's own user documentation assigns musical parts only to ports 1 and 2. Port 3 is associated with external/remote control in historical Yamaha material and should not be assumed to be a third 16-part synthesizer.
- The implementation resembles an MPU/UART service model, but the actual register offsets and command bytes must be recovered before calling it MPU-401 compatible.

Mixer/DSP:

- The topology implements master, six wave-output, two wave-input/monitor volume and mute controls, plus Yamaha-specific property handlers.
- Named controls include `PORT0`, `PORT1`, `MKS`, `FSLClr`, peak metering, DSP read/write/transmit/init, and SWXG/YSWDS private control payloads.
- The driver contains embedded DSP/MPR tables and initialization programs for normal and ASIO modes. These are data assets as well as algorithms; a clean-room implementation should document the command protocol independently and obtain legal advice before redistributing byte-identical Yamaha payloads.

Digital audio:

- The inspected strings expose port-routing and sampling-interface helpers (`GetSIFormat2ch`, `GetSIFormat4ch`) but do not explicitly name S/PDIF, AES/EBU, or word-clock controls.
- The readme states that the ASIO implementation lacks direct monitoring, sample-accurate positioning, audio transfer, and continuous re-sync, indicating hardware/firmware timing limitations.
- Exact digital connector modes, clock-source selection, channel-status bits, and sample-rate rules remain unresolved and require deeper topology-table/data analysis plus hardware tests.

## Supported formats and feature constraints

The driver validates sample rate, channel count, and bit depth and has distinct 16-bit and 32-bit MPR/DSP paths. The readme describes the ASIO driver as “32bit mode,” meaning a 32-bit sample container suitable for 24-bit recording, not merely a 32-bit executable. The exact WDM format matrix is encoded in filter/data-range tables but was not confidently decoded in this pass. It should be extracted before implementation rather than guessed.

The original release claims multiprocessor-safe interrupt handling, low-latency KS streaming, two SW1000 inputs, six SW1000 outputs, three synth ports, MIDI I/O, and simultaneous SW1000/DS2416 ASIO operation. Ordinary WDM enumeration was said to have a ten-port limit at the time.

## Recommended clean implementation

1. **Evidence notebook and register map.** Load the SYS only into a modern static decompiler, apply the recovered class/method names, and annotate `CAdapterSW1000::Init`, resource assignment, ISR, every `READ/WRITE_REGISTER_UCHAR` caller, DMA helpers, UART helpers, and `SendDSP`/`SendMpr`. Record offset, width, direction, bit meanings, call sites, ordering, delays, and confidence.
2. **Hardware observation harness.** On a sacrificial Windows XP/32-bit machine with the card, use PCI configuration/resource inspection and a kernel debugger or bus analyzer to record BAR sizes, reset state, initialization writes, DMA addresses, interrupt status/acknowledge behavior, and power transitions. Do not experiment first on a production Windows 11 host.
3. **Minimal x64 PortCls driver.** Use the current WDK, modern INF, PnP/power callbacks, one topology filter, and a WaveRT render miniport. Constrain DMA to addresses below 4 GiB until proven otherwise. Initially expose one conservative stereo PCM render endpoint and no private IOCTLs.
4. **Bring-up sequence.** Implement PCI match, resource mapping, hardware reset, interrupt mask/acknowledge, common-buffer allocation, silent-buffer playback, period interrupts, position reporting, and clean stop/remove. Validate with Driver Verifier and HVCI/Memory Integrity enabled.
5. **Expand PCM.** Recover and add the exact format matrix, capture, all six render/two capture logical paths, synchronized start, mixer volume/mute, suspend/resume, surprise removal, and PnP rebalance.
6. **MIDI.** Add the physical UART first, then the three synth input ports. Modern MIDI exposure can remain PortCls-compatible initially; MIDI 2.0 is not necessary for this MIDI 1.0-era device.
7. **DSP/digital features.** Recreate or lawfully source the DSP/MPR initialization data, then add routing, digital I/O, clocking, peak meters, and private mixer controls. Keep the hardware protocol in a separate documented module.
8. **Low-latency user mode.** Prefer native WaveRT/WASAPI exclusive-mode support. Treat a new ASIO user-mode component as optional and build it over a deliberately narrow, validated interface; do not clone the old MDL/user-mapping ABI.
9. **Release engineering.** Test-sign during development. For distribution, use a Microsoft Hardware Dev Center signing route; HLK/WHCP is the recommended production path, while attestation signing is aimed at testing and has distribution limitations.

Microsoft continues to recommend PortCls for typical PCI/DMA audio hardware, with WaveRT handling most generic DMA streaming work while the miniport supplies device-specific details. That makes a modern PortCls/WaveRT rewrite lower risk than preserving the old WaveCyclic design.

## Feasibility verdict

- **Basic stereo playback:** good prospect after register/DMA recovery.
- **Playback plus capture and external MIDI:** feasible with moderate reverse engineering and access to real hardware.
- **All six outputs, two inputs, three synth ports, mixer and digital routing:** feasible but dependent on reconstructing Yamaha's DSP/MPR protocol and tables.
- **Drop-in replacement for all legacy WDM and ASIO behavior:** possible in principle, but disproportionately expensive and not the recommended first target.
- **Main risks:** undocumented embedded programs, 32-bit DMA limitations, interrupt timing, exact format/clock constraints, absence of the extracted ASIO DLL, and legal clearance for redistributing Yamaha-derived data.

Overall recommendation: proceed with a proof-of-concept driver, but define success initially as stable x64 playback, capture, and MIDI—not full 2002 feature parity.

## Synth-focused addendum

The primary target is now the onboard XG synthesizer. Static evidence supports a smaller first driver than the full plan above.

### Confirmed MIDI architecture

- `SWXG1`, `SWXG2`, and `SWXG3` are three distinct kernel-streaming MIDI **render** subdevices. They are hardware-facing MIDI inputs to the board, not Windows' software synthesizer; the readme explicitly warns that the Windows “SW Synth” is unrelated.
- Yamaha's owner documentation confirms that SWXG1 addresses XG parts 1–16 and SWXG2 addresses parts 17–32. It does not present SWXG3 as another musical port. Other period Yamaha material describes port 3 as an external/remote-control mode, making control SysEx the leading interpretation of the third endpoint.
- `MIDIOUT` and `MIDIIN` are the physical MIDI connector paths. They are separate from the three internal synth ports.
- Yamaha uses a common `CMiniportMidiUartSWXG`/`CMiniportMidiStreamUartSWXG` implementation, parameterized by a port number or GUID. Named helpers convert subdevice GUIDs to numeric channels and obtain subdevice names.
- The lower layer is `CPciUart`, with a per-channel `CUartChannel` queue. Its operations include initialization/reset, command/status/data access, TX/RX enable, open/close, transmit-empty waiting, interrupt-time reads/writes, and queue management.
- MIDI input and output logic (`CMidiIn`, `CMidiOut`) sits above that UART layer. Output contains byte-wise send and an `F5`-related compatibility path; input has a bounded interrupt-fed queue and a PortCls service group.
- One render and one capture stream are allowed per MIDI miniport instance. Stream state follows the normal STOP/ACQUIRE/PAUSE/RUN model.
- The shared adapter ISR dispatches both audio and MIDI causes; MIDI therefore still needs the adapter's interrupt synchronization and acknowledge/mask logic even if PCM support is omitted.

### Short-message and SysEx expectations

The stream write interface takes arbitrary byte buffers rather than fixed three-byte MIDI packets, and the output layer sends bytes through a queue. This is compatible with ordinary MIDI 1.0 channel messages, system-common/realtime messages, and SysEx streams. It does not prove that arbitrarily long SysEx transfers work without chunking; queue depth, timeout behavior, running status, inter-byte pacing, and the special handling associated with status `0xF5` must be reconstructed and tested.

### Dependency on DSP/MPR initialization

The synth MIDI classes do not themselves advertise firmware upload. Their visible responsibility is UART transport. The adapter, however, performs board-wide reset/initialization before registering subdevices and contains a specific `SWXGInputInit` operation. Therefore:

- It is plausible that basic XG synthesis needs only board reset, UART setup, `SWXGInputInit`, and routing/volume defaults.
- It is not yet safe to claim that the large embedded DSP/MPR programs can all be omitted. They may configure digital mixing and route the tone generator to the analogue/digital outputs even though they do not generate XG voices themselves.
- The practical way to separate these dependencies is to annotate the complete call tree from `SW1000_StartDevice` and `CAdapterSW1000::Init`, then compare traces with and without Wave/topology subdevice creation on real hardware.

### Revised minimum viable driver

1. Match `PCI\VEN_1073&DEV_1000`, map the translated MMIO resource, and initialize synchronized interrupts.
2. Reproduce only the proven-safe adapter reset and SW1000 board-start sequence.
3. Implement the PCI-UART register layer and interrupt acknowledgement.
4. Expose **SWXG1 only** as a MIDI 1.0 render endpoint and verify Note On/Off, controllers, program changes, pitch bend, realtime bytes, and GM/XG reset SysEx through the card's physical analogue output.
5. Add SysEx queueing/back-pressure tests and timestamp/jitter measurements.
6. Add SWXG2 for XG parts 17–32. Add SWXG3 separately only after its remote-control protocol and intended clients are understood.
7. Add the physical MIDI output and input paths.
8. Restore only the mixer/routing initialization required to make the synth audible. Leave PCM DMA, capture, ASIO, and unrelated topology controls out of the first release.

### Synth-specific unknowns to resolve next

- Exact UART base offsets and per-channel stride/address table.
- Mapping of logical channel numbers to SWXG1/2/3 and physical MIDI ports.
- UART status, data, command, interrupt-enable, and interrupt-cause bits.
- Reset and `SWXGInputInit` byte sequences.
- Exact SWXG3 behavior. Current evidence points to remote/external control, not another 16 musical parts.
- Queue size, overflow behavior, TX-ready polling/interrupt behavior, and SysEx pacing.
- Minimum DSP/mixer program needed to route synth audio to line/digital outputs.
- Whether the card preserves patches/settings across driver reset and which XG reset message the original driver sends, if any.

### Revised feasibility verdict

A modern synth-only driver has a **good** feasibility outlook and is materially smaller than a 1:1 audio driver. The favorable evidence is the clean separation between PortCls MIDI streams and the common PCI-UART layer, plus the absence of any separate tone-generator firmware file. The remaining hard part is the board-wide initialization/routing boundary: enough of Yamaha's adapter and DSP startup must be understood to make MIDI audible, even if no PCM endpoints are exposed.

## Recovered PCI-UART protocol

This section comes from function-level disassembly using the public-symbol records retained inside `yswds.sys`. Addresses below are offsets within the mapped SW1000XG MMIO BAR, not CPU virtual addresses.

### UART window table

The binary contains the following constant table, named `CPciUart::m_UartAdrs`:

| UART index | MMIO base offset | Confidence |
|---:|---:|---|
| 0 | `0x3E000` | Confirmed |
| 1 | `0x3E002` | Confirmed |
| 2 | `0x3F000` | Confirmed |
| 3 | `0x3F100` | Confirmed |
| 4 | `0x3F200` | Confirmed |
| 5 | `0x3F300` | Confirmed |
| 6 | `0x3F400` | Confirmed |

The SW1000XG constructor creates four MIDI-output objects and one MIDI-input object. Its connection table proves that the SW1000XG uses UART indices 0 and 1; indices 2–6 remain available to other configurations or functions in the shared SW1000XG/DS2416 binary.

| UART index | SW1000XG assignment |
|---:|---|
| 0 (`0x3E000`) | Physical mini-DIN MIDI OUT and MIDI IN |
| 1 (`0x3E002`) | Shared internal SWXG1/SWXG2/SWXG3 transport |
| 2–6 | Not used by the recovered SW1000XG MIDI connection table; purpose unresolved |

### Register layout

For a selected UART window:

| Offset | Read | Write | Confidence |
|---:|---|---|---|
| `+0` | Receive data | Transmit data | Confirmed from `GetDataUARTISR` / `SetDataUARTISR` |
| `+1` | Status | Command/control | Confirmed from `_GetStatUART` / `_SetCmndUART` |

All four operations use `READ_REGISTER_UCHAR` or `WRITE_REGISTER_UCHAR` and therefore are byte MMIO accesses.

### Initialization and control

Each initialized UART receives this command sequence at `base+1`:

```text
00 00 00 50 4E 10
```

The repeated zero writes may reset or drain internal state; `0x50`, `0x4E`, and `0x10` then configure the UART. These command meanings are not yet assigned because the binary provides behavior but no symbolic bit names.

The driver keeps a shadow command/control byte for each channel:

- Control bit `0x01`: transmit interrupt/operation enable (`EnableTX` sets it; `DisableTX` clears it).
- Control bit `0x04`: receive interrupt/operation enable (`EnableRX` sets it; `DisableRX` clears it).
- Status bit `0x01`: receive-side condition is the leading interpretation from ISR paths, but final assignment needs the complete caller trace.
- Status bit `0x02`: transmit-ready/empty condition. The UART ISR dequeues one outgoing byte and writes it to `base+0` when this bit is set.
- Status mask `0x38`: error/exception conditions. Reading such a status makes the driver set control bit `0x10`, apparently to acknowledge or clear the condition.

### Logical-port framing

The old driver multiplexes logical MIDI destinations over a UART transport. `CMidiOut::PutData` calls a per-channel `IsNeedF5` tracker. When a destination switch is needed, it transmits:

```text
F5 <logical-port-number + 1>
```

before the client's MIDI bytes. For the three SWXG endpoints this produces selectors `F5 01`, `F5 02`, and `F5 03`.

This `F5` is an internal Yamaha transport selector, not a MIDI message intended for the tone generator. After inserting the selector, the driver may retransmit the remembered MIDI running-status byte so that a data-byte-only continuation remains valid at the newly selected destination. The running-status cache is updated for status bytes `0x80`–`0xEF` and cleared for most system status bytes.

The port-selection tracker suppresses redundant prefixes for up to 100 operations while the same logical destination remains selected, then forces a refresh. Queue reset sets its remembered destination to `0xFF` and the refresh counter to 100.

### Buffering and interrupt behavior

- Each UART channel object is large because it embeds two `0x2000`-byte circular queues.
- Ring indices are masked with `0x1FFF`, confirming an 8192-byte queue capacity per direction with one slot reserved to distinguish full from empty.
- The enqueue path waits for space up to a performance-counter-based timeout, then marks a timeout flag rather than overrunning the queue.
- The interrupt handler sends one queued byte per TX-ready interrupt.
- `WaitTX` polls the status through a synchronized interrupt callback and uses a bounded timeout; the old driver therefore supports both polling during control/setup operations and interrupt-driven queued streaming.
- Per-channel locking uses spin locks at interrupt/DPC level.

### Clean-driver implications

The first synth proof of concept no longer needs to guess the basic register shape, endpoint mapping, or logical framing. It can implement:

1. Map a sufficiently large BAR to include offsets through at least `0x3F401`.
2. Initialize UART index 1 at `BAR + 0x3E002`, the proven SWXG transport, with `00 00 00 50 4E 10` written to `BAR + 0x3E003`.
3. Select SWXG1 with `F5 01`, then transmit a complete status-bearing MIDI message such as Note On.
4. Enable TX with control bit `0x01`, feed bytes when status bit `0x02` is asserted, and acknowledge `0x38` conditions with control bit `0x10` as the original does.
5. Add SWXG2 with `F5 02`; reserve `F5 03` for the control endpoint.
6. Implement running-status restoration and the 100-operation selector refresh only after straightforward status-complete messages work.

### Proven endpoint and interrupt mapping

`CAdapterSW1000::Init` connects its MIDI objects as follows:

| Windows endpoint | UART index | MMIO base | Yamaha logical selector | Interrupt-status bit |
|---|---:|---:|---:|---:|
| SWXG1 render | 1 | `0x3E002` | `F5 01` | TX bit 26 |
| SWXG2 render | 1 | `0x3E002` | `F5 02` | TX bit 26 |
| SWXG3/control render | 1 | `0x3E002` | `F5 03` | TX bit 26 |
| Physical MIDI OUT | 0 | `0x3E000` | none/internal logical index 3 | TX bit 24 |
| Physical MIDI IN | 0 | `0x3E000` | not applicable | RX bit 25 |

The adapter derives interrupt bits algorithmically:

```text
TX bit = 24 + 2 * uart_index
RX bit = 25 + 2 * uart_index
```

Thus UART 1 also has a receive bit at 27, although no internal synth-capture endpoint is registered. The shared adapter ISR reads a 32-bit interrupt status at `BAR + 0x3FF04`, masks it with the driver's enabled-interrupt mask, and dispatches UART indices 0 and 1 separately. This makes the minimum synth interrupt path specific: test/acknowledge bit 26 and service the UART at `0x3E002`.

The adapter initializes both UART indices during normal startup. A synth-only driver can initially omit UART 0 if physical MIDI I/O is out of scope, but should preserve the original reset ordering until hardware tests establish that the two blocks are independent.

### Corrected minimum synth transaction

After board reset and interrupt setup, a conservative first transaction is:

```text
MMIO command address: BAR + 0x3E003
MMIO data address:    BAR + 0x3E002

command writes: 00 00 00 50 4E 10
data writes:    F5 01 90 3C 40
```

The data sequence selects SWXG1 and sends MIDI channel 1 Note On, middle C, velocity 64. A subsequent `80 3C 00` or `90 3C 00` should silence it. This is a protocol reconstruction, not a recommendation to write the registers from user mode; it belongs in a controlled test driver with bounded waits and interrupt masking.

## Recovered synth startup ordering

The original driver's startup call graph establishes the following order:

```text
CAdapterSW1000::Init
  -> CAdapterCommon::Init
       -> map translated memory resource with MmMapIoSpace
       -> ResetController
            -> InitDSP
            -> IrqDisableAll
       -> create and initialize PcNewInterruptSync/IInterruptSync
  -> connect SWXG1, SWXG2, SWXG3, physical MIDI OUT, physical MIDI IN
  -> InitializeHardware
       -> run SWXGInit through IInterruptSync
            -> CAdapterSW1000::InitCrit
                 -> open SWXG1 output
                 -> open SWXG2 output
                 -> open SWXG3/control output
                 -> open physical MIDI input
       -> enable TX for UART 0
       -> enable TX for UART 1
       -> enable RX for UART 0
```

`OpenCrit` initializes a UART on the first open, resets its queue state, and increments its open count. Because SWXG1–3 share UART 1, the first synth endpoint performs the hardware initialization and subsequent endpoints reuse it. Physical MIDI OUT is constructed but is not opened by `InitCrit`; its UART is already initialized through the physical MIDI input open.

### Board-global registers confirmed

| MMIO offset | Width | Role | Confidence |
|---:|---:|---|---|
| `0x3FF00` | 32-bit | `PORT0`, a global control/status register | Confirmed |
| `0x3FF04` | 32-bit | shared interrupt pending/enable/acknowledge register (`TRPIF`) | Confirmed |
| `0x3FF10` | 32-bit | `PORT1`, a second global control register | Confirmed |
| `0x3E004` | 32-bit | auxiliary SW1000 register accessed under its own lock; semantic name unresolved | Confirmed access, unresolved purpose |

The driver maintains software shadows of `PORT0`, `PORT1`, and the interrupt mask. Changes are serialized and then written as complete 32-bit values.

`PORT1` bit 31 is used by the driver's global interrupt mask/unmask operation. Bits 16–17 select a clock/sample-rate mode; the corresponding cached rates are 48,000 Hz, 44,100 Hz, or 32,000 Hz. A synth-only implementation should initially preserve the exact original `PORT1` programming instead of choosing a rate independently.

### Interrupt programming

The driver treats `0x3FF04` as both the interrupt-status source and the programmed interrupt mask/acknowledge target:

- `SetTRPIF(mask)` updates the software shadow and writes the full 32-bit value to `BAR + 0x3FF04` through `IInterruptSync` when available.
- `IrqEnable(bit)` sets `1 << bit`; `IrqDisable(bit)` clears it.
- `IrqDisableAll` writes zero.
- The SW1000 ISR reads `BAR + 0x3FF04`, intersects it with the enabled mask, then dispatches only recognized sources.
- MIDI TX/RX bit formulas and endpoint assignments are listed in the previous section.

### DSP dependency verdict

The original driver unconditionally invokes `InitDSP` from its reset path before interrupt and MIDI initialization. This changes the synth-only risk assessment:

- The large embedded DSP/MPR data is not merely loaded on first PCM or ASIO use; at least the common initialization program is part of normal adapter bring-up.
- It may initialize audio routing, effects, clocks, or shared communication machinery required for the XG engine to be audible.
- A first faithful prototype should reproduce the original DSP initialization rather than omit it.
- A later controlled experiment may determine that a smaller subset is sufficient, but static evidence does not currently justify that assumption.

The next reverse-engineering unit is therefore `CAdapterCommon::InitDSP` and its `initMprDsp`/`SendDSP`/`SendMpr` helpers: identify which embedded tables are sent during ordinary SW1000 startup, their destination addresses, ordering, completion/status polling, and whether any branch is specific to PCM/ASIO rather than baseline synthesis.

### Findings from independent documentation

Yamaha's SW1000XG owner manual independently confirms the endpoint model inferred from the INF and binary:

- SW1000 Synthesizer #1 is the MIDI output device for tone-generator parts 1–16.
- SW1000 Synthesizer #2 is the MIDI output device for parts 17–32.
- The external mini-DIN MIDI connector uses the separately named SW1000 MIDI IN and MIDI OUT devices.
- The board contains a 32-part AWM2 XG tone generator and accepts ordinary MIDI and XG SysEx control. Mixer settings are also transmitted as MIDI data to a selected SW1000 synthesizer port.
- The optional PLG100-series board extends the tone generator; it does not change the conclusion that the base engine's musical parts are split across ports 1 and 2.
- Yamaha notes that a mode-change message can require approximately half a second before subsequent musical data. A new driver should not hide this hardware settling requirement; the MIDI client or a documented initialization helper should delay appropriately after GM/XG mode reset.

No open-source Linux/BSD driver or public register specification was found. Contemporary Linux discussions specifically report that Yamaha kept the interface closed. The unrelated Linux `snd-ymfpci` driver supports YMF724/740/744/754 devices, not PCI ID `1073:1000`, and should not be used as a register-map template.

## Sources

- Local evidence: `yswds.sys`, `sw1000xg.inf`, `ds2416.inf`, and `readme.txt` from the supplied package; SHA-256 hashes above.
- Microsoft, “Introduction to Port Class”: https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/introduction-to-port-class
- Microsoft, “Miniport Driver Types by Operating System”: https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/miniport-driver-types-by-operating-system
- Microsoft, “Implement PnP Rebalance for PortCls Audio Drivers”: https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implement-pnp-rebalance-for-portcls-audio-drivers
- Microsoft, “Driver signing options and best practices”: https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings
