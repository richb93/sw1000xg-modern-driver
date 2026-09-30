# PCM audio path and mixer

Static analysis of Yamaha `yswds.sys` 1.01.0014.1 (SHA-256
`0e475f6a…eb4b4`), using the embedded public symbols described in
[power-interrupt-uart-findings.md](power-interrupt-uart-findings.md). The driver
was never executed. Addresses are virtual addresses at image base `0x10000`.
ASIO (`CAsioBase`, `CAsioSWDS`, `CAsioChInfo*`) and DS2416-only code are out of
scope here.

## Summary

- PCM is **not a separate register block**. Every audio stream is described
  to the DSP through the same two primitives the startup already uses:
  single-value DSP RAM writes (`SetRAM`) and DSP-window writes
  (`SetDSP`/`SendDSP`).
- The DSP **bus-masters one contiguous common buffer per channel** using
  32-bit physical byte addresses, as a ping-pong (half-buffer) ring.
- The sample rate is **board-wide**, selected by `PORT1` bits 16–17. The
  default after startup is **44.1 kHz**.
- Supported stream formats: PCM, **16- or 32-bit**, **mono or stereo**, at
  the current board rate only.
- **Mixer controls are XG SysEx**, sent over the SWXG1 MIDI port, not register
  writes. Wave-out/in volume, pan and mute are XG "A/D part" parameters, so
  PCM mixing depends on the MIDI path (and its 10 s H8 boot wait).
- Synth audibility does not depend on any of this. The mixer only affects
  the PCM parts and the XG master volume.

## Channels and endpoints

- The hardware schedules **16 playback DSP channels (8 pairs)** and **8 record
  channels (4 pairs)**. The ISR (`0x1E778`) walks 8 playback pairs, then the
  record pairs. Each channel record (`_WaveInfo`) is 0x44 bytes, and a pair
  (left/right) is 0x88.
- A stereo stream uses both channels of a pair. A mono stream uses one.
- The SW1000XG INF exposes 6 stereo playback endpoints (`WAVEOUT1–6`) and 2
  stereo capture endpoints (`WAVEIN1–2`).
- Each channel record holds its `TRPIF` bit mask. The per-channel interrupt
  bits are the causes below bit 24 (UARTs use 24–27). *The exact
  channel-to-bit numbering is not yet tabulated.*

## DSP RAM map (per channel *n*)

All accessed with `SetRAM`/`GetRAM` (`SyncSetRAM` `0x1F71A`, `SyncGetRAM`
`0x1F54E`), i.e. through DSP window 0 at `0x3F000`.

| Address | Name | Meaning (from use) | Evidence |
|---:|---|---|---|
| `0xC000+n` | `SA` | Buffer start: physical address of byte 0 | `WaveDmaRegResetCore` `0x1FF0C` |
| `0xC020+n` | `EA` | Buffer end: physical start + buffer size | same |
| `0xC040+n` | `IA` | Next interrupt address. Initially start + size/2; the ISR moves it between halves | same; ISR `SetRAM(0xC040+n, …)` |
| `0xC060+n` | `TP` | Transfer pointer (hardware position). Initially start; read by `GetTP` for position | `GetTP`, `WaveGetPositionCore` |
| `0xC080+n` | `TWRP` | Unresolved write pointer | `SetTWRP` |
| `0xC0A0+n` | `APP` | Playback parameter, cleared to 0 before a synchronised start | `SetAPP`, `WaveOutSyncStart` |
| `0xC0B0+n` | `ARP` | Record counterpart of `APP` | `SetARP` |
| `0xC100` | `TRWF` | Transfer-enable **bitmask**, bit *n* per channel (shadow at adapter `+0x168`) | `TrwEnable`/`TrwDisable` |
| `0xC101` | `TRWFO` | Transfer-enable acknowledge/"off" state; `waitTRWFO` polls it before starting | `WaveOutSyncStandby` |
| `0xC102` | `SUSF` | Unresolved | `SetSUSF` |

DSP-window (selector `0x100`) values used by the PCM path:

| Destination | Name | Use |
|---:|---|---|
| `0x06+n` | `APF` | Per-channel playback flag |
| `0x26+n` | `ARF` | Per-channel record flag |
| `0xE1` | `SCR` | Sample counter / clock control (ASIO reconfiguration) |
| table `0x23530[n]` | `PSCPEC` | Two words: sample-count start/end of a scheduled playback start |
| table `0x235B0[n]` | `RSCREC` | Two words: same for record |

`GetSCR` reads a free-running sample counter directly at **MMIO
`BAR+0x3F09C`**. Starts are scheduled a fixed number of samples ahead of it
(`WaveOutSyncStandby` returns 300 × channels-in-standby), so every stream in a
group starts on the same sample.

## Buffer programming

`WaveDmaRegIniz(capture, channel, IDmaChannel)` (`0x203C8`) stores the channel's
`IDmaChannel`, takes the **low 32 bits** of `PhysicalAddress()`, and calls
`WaveDmaRegResetCore`:

```text
size = BufferSize()
SA(n) = phys
TP(n) = phys
IA(n) = phys + size/2
EA(n) = phys + size
```

`WaveDmaRegSetFrameSize(n, frame)` clamps `2 × frame` to the allocated size,
sets the buffer size to `2 × frame`, then rewrites `IA = phys + frame` and
`EA = phys + 2 × frame`. The hardware therefore interrupts at the half and end
of the ring. The ISR then advances `IA` to the other half and notifies the
stream's service group.

DMA channels come from `IPortWaveCyclic::NewMasterDmaChannel` in
`CMiniportWaveYSWDS::ProcessResources` (`0x360BE`) with
`Dma32BitAddresses = TRUE`, `Dma64BitAddresses = FALSE`, `Width32Bits`,
maximum lengths `0x800`/`0x400`. The adapter also keeps a `0x8000`-byte wave
common buffer per stream (`Set/GetWaveCommonBuffer`). **All PCM buffers must
be physically contiguous and below 4 GiB.**

## Format and routing

`CMiniportWaveYSWDS::ValidateFormat` (`0x361FE`) accepts
`KSDATAFORMAT_TYPE_AUDIO` / `KSDATAFORMAT_SUBTYPE_PCM` /
`KSDATAFORMAT_SPECIFIER_WAVEFORMATEX` with `wFormatTag = 1`,
`wBitsPerSample` 16 or 32, `nChannels` 1 or 2, and `nSamplesPerSec` equal to
`GetCurrentFS()`. The stream's `SetFormat` then calls
`SetPlayMode(pair_base_channel, nChannels, bits == 32)` or `SetRecMode(…)`.

`SetPlayMode` (`0x33BDC`) programs the pair's DSP routing with:

1. `SendDSP(0, sel 0x000, dest 0x31 + 2p, 2 words)`, where the values are
   `GetPlayFactor() × p + 0x21041` and `… + 0x21040`, with `+0x100`
   adjustments for mono or 16-bit;
2. `SetDSP(0, sel 0x500, dest 0x12 + 4p, 0x50000201 + (p << 24) [+0xFE80 if 32-bit])`;
3. `SendDSP(0, sel 0x300, dest 0x11 + 4p, 4 words)` from table `0x234D0`, or
   `0x234E0` for 32-bit stereo;
4. three single `SetDSP` writes (selectors `0x300`, `0x300`, `0x400`) whose
   destinations come from byte tables at `0x23508`, `0x23510` and `0x23518`.

Here *p* = pair index. `SetRecMode` (`0x33D6A`) is the same shape with
selectors `0x000`/`0x500`/`0x300`/`0x400` and tables `0x234F0`/`0x234FC`,
`0x23520`–`0x2352C`. These are DSP-program routing words. They must be
reproduced exactly, but they need no further interpretation to implement.

## Sample rate

`SetPORT1` (`0x1DF94`) caches the board rate from `PORT1` bits 16–17:

| Bits 17:16 | Rate |
|---:|---:|
| `00` | 48,000 Hz |
| `01` | 44,100 Hz |
| `10` | 32,000 Hz |
| `11` | 44,100 Hz |

Startup's `0xD1A18000` has `01`: 44.1 kHz. The original lets its control panel
change the rate through a private topology property (`ControlSetPort1` →
`IWritePORT1A`) that writes an arbitrary 32-bit `PORT1` value **from user
mode**. A new driver must not reproduce that. Rate selection should be a
validated setting applied only while no stream is running.

## Start, stop and position

- `WaveStart` → `playStart`/`recStart`. When the adapter is in synchronised
  mode, pending channels are put in standby (`startTX` sets the `TRWF` bit,
  and `waitTRWFO` waits for the DSP's acknowledge). All pending pairs are then
  given a common `PSCPEC` start count, `SCR + standby offset`, with `APP`
  cleared for the second channel of a stereo pair.
- `WaveStop` → `playStop`/`recStop`, then `WaveDmaRegResetCore` (pointers back
  to the buffer start).
- Position: `WaveOutGetPosition` → `WaveGetPositionCore` → `TP(n)`, read
  through `GetRAM` (a DSP-window transaction, not a plain MMIO read).
- `QueryPowerChangeState` refuses D-state changes while any channel is active.

## Mixer

`MixerRegWrite(reg, value)` caches the value and, while the device is powered,
calls `CAdapterSW1000::MixerRegWriteCore` (`0x32020`):

| Register | Control | Implementation |
|---:|---|---|
| 0–1 | Master volume | `SetMasterVolume` |
| 2–13 | Wave-out 1–6 volume (L/R pairs) | `SetWodVolume(pair)` |
| 14–15 | Wave-in volume (L/R) | `SetWidVolume` |
| 20 | Master mute | `SetMasterMute` |
| 21–26 | Wave-out 1–6 mute | `SetWodMute` |
| 27 | Wave-in mute | `SetWidMute` |

Each setter fills a 9-byte Yamaha XG parameter-change template and sends it
with `IMidiDevice::SendData(port 0 = SWXG1, bytes, 9)`, which first performs
the 10 s `WaitH8`:

| Template (`0x10A30`…) | XG address | Parameter | Data |
|---|---|---|---|
| `F0 43 10 4C 00 00 04 vv F7` | System | Master Volume | `volume >> 9`, or 0 when muted |
| `F0 43 10 4C 00 00 05 00 F7` | System | Master Attenuator | 0 |
| `F0 43 10 4C 10 mm 0B vv F7` | A/D part *mm* | Volume | `volume >> 9` |
| `F0 43 10 4C 10 mm 0E pp F7` | A/D part *mm* | Pan | `01` left / `7F` right |
| `F0 43 10 4C 10 mm 35 dd F7` | A/D part *mm* | On/off, used as mute | 1 on, 0 off |

Wave-out pair *k* (0–5) is XG A/D parts `2+2k` (panned hard left) and `3+2k`
(hard right). Wave-in is parts 0 and 1. PCM playback is therefore mixed
**inside the XG tone generator**, as extra A/D parts, alongside the synth.

Values are cached in the adapter, saved under the device's `Settings`
registry key (`Save/RestoreMixerSettingsFromRegistry`), and replayed on
resume. The topology miniport maps volumes through a 128-entry `volTable`
(`0x2E418`) to dB for KS properties (`InitControlValueCache`, `0x35018`).

## Consequences for the new driver

1. **Architecture.** MIDI, PCM and topology share one adapter, one
   interrupt and the SWXG1 port (for mixer SysEx). The modern shape is a
   PortCls adapter with a MIDI miniport, a WaveRT render/capture miniport and
   a topology miniport, all using one `IInterruptSync`. PortCls can coexist
   with KMDF (as in Microsoft's SYSVAD sample, via
   `WdfDriverInitNoDispatchOverride`), so the existing KMDF code need not be
   discarded.
2. **WaveRT fit.** A contiguous buffer below 4 GiB, a hardware position
   (`TP`) and two interrupts per ring map naturally onto WaveRT with
   `AllocateBufferWithNotification` (two notifications per buffer) and a
   `GetPosition` that reads `TP` through the DSP window.
3. **Fixed rate.** Expose only the current board rate. Changing it needs
   every stream stopped, and a controlled, validated control.
4. **Mixer through MIDI.** Topology volume/mute must queue XG SysEx on SWXG1
   after the H8 wait, and must not interleave inside a client's SysEx on the
   same port (the original serialises through `CMidiOut`).
5. **Order of work.** MIDI stays first. PCM needs, in order: per-channel
   interrupt bits, `SetPlayMode` tables captured as data, a single stereo
   16-bit 44.1 kHz render stream, then capture and multiple streams.

## Open items

- Exact `TRPIF` bit for each DSP channel. Where `_WaveInfo +0x10` is
  initialised has not been traced yet.
- `TWRP`, `SUSF`, `APF`/`ARF` semantics, and `GetPlayFactor` for the SW1000.
- Content of the `SetPlayMode`/`SetRecMode` tables (`0x234D0`–`0x2352C`,
  small Yamaha constants; extract with the other assets).
- Digital I/O (S/PDIF) connector modes and clock-source selection beyond the
  fixed `set_dit` startup values.
