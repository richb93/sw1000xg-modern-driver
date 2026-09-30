# Driver architecture (MIDI first, PCM later)

This note fixes the shape of the full driver before MIDI work starts, so that
PCM can be added later without a rewrite. It is based on
[power-interrupt-uart-findings.md](power-interrupt-uart-findings.md) and
[pcm-path.md](pcm-path.md).

## Constraints from the hardware

1. **One adapter, one interrupt, one register block.** MIDI, PCM and the
   mixer share the BAR, the `TRPIF` interrupt (PCM bits 0–23, UART bits
   24–27) and the `PORT1` shadow.
2. **The mixer is MIDI.** Volume and mute are XG SysEx on the SWXG1 transport
   (UART 1). The topology filter depends on the MIDI output path.
3. **The ISR touches the DSP window.** The original moves each PCM channel's
   interrupt address (`IA`) from the ISR with `SetRAM`. So every DSP-window
   transaction must be serialised with the ISR, not just with other threads.
4. **No interrupt acknowledge.** Causes clear when serviced. UART TX must be
   masked in `TRPIF` whenever its queue is empty.
5. **Boot gate.** No SWXG byte may be sent until 10 s after DSP reset release
   (`SWXG_H8_BOOT_MS`).
6. **Board-wide sample rate**, 32-bit DMA addresses, and contiguous PCM
   buffers.
7. **Full re-initialisation on every D0 entry.** Nothing is retained across
   D3.

## Chosen shape

A single **PortCls adapter driver** exposes three kinds of subdevice. It uses
KMDF in *miniport mode* (`WdfDriverInitNoDispatchOverride`), as Microsoft's
SYSVAD sample does, so PortCls keeps the WDM dispatch table and KMDF is
available for helpers.

```text
                 +------------------------------------------+
                 | Adapter (one per card)                   |
                 |  BAR map, swxg core, PORT1/TRPIF shadows |
                 |  IInterruptSync + ISR, power, H8 gate    |
                 |  UART layer (ch 0 external, ch 1 SWXG)   |
                 |  DSP-window access, serialised with ISR  |
                 +----+--------------+--------------+-------+
                      |              |              |
            MIDI miniports     Topology miniport   WaveRT miniports (later)
            SWXG1, SWXG2,      master/wave vol+mute  6 render, 2 capture
            SWXG3, MIDI OUT,   -> XG SysEx on SWXG1  SA/EA/IA/TP per channel
            MIDI IN
```

The current KMDF diagnostic driver stays as a **bring-up tool**. It is the
safe way to validate startup on new hardware and reuses the same core.

## Layers and files

| Layer | Location | Kernel-free? | Tested on host |
|---|---|---|---|
| Register protocol, startup, DSP window, `SetRAM` | `src/hardware/sw1000xg_hw.*` | yes | yes (fake BAR) |
| MMIO trace | `src/hardware/sw1000xg_trace.*` | yes | yes |
| UART channel: init, TX/RX rings, ISR service, polled TX, `F5` framing, running status | `src/hardware/sw1000xg_uart.*` | yes | yes |
| PCM channel programming: buffer registers, `SetPlayMode` tables | `src/hardware/sw1000xg_pcm.*` (later) | yes | yes |
| Adapter, ISR, power, PortCls glue | `src/portcls/` (new) | no | Windows only |
| Bring-up initializer | `src/kmdf/` (existing) | no | Windows only |

Keeping protocol logic in the kernel-free core means most behaviour
(queueing, framing, masking, DSP transaction order) is covered by `make test`
before anything runs on Windows.

## Interrupt and locking model

- **ISR** (`IInterruptSync`, shared line): read `TRPIF` once and AND it with
  the enable shadow. If the result is zero, return "not mine". Otherwise:
  - UART TX bits 24/26: feed one byte while status `0x01` is set. On an
    empty queue, clear the bit in the shadow and write `TRPIF` directly
    (the ISR-level variant).
  - UART RX bit 25: drain while status `0x02` is set, into a bounded RX
    ring, then notify the MIDI-in service group.
  - PCM bits 0–23: advance `IA` to the other half with `SetRAM`, then notify
    the WaveRT stream (later).
- **Everything else that touches `TRPIF`, `PORT1` or the DSP window** runs
  through `IInterruptSync::CallSynchronizedRoutine`, as the original does.
  One lock domain removes a whole class of races between the ISR's `SetRAM`
  and passive-level DSP writes.
- **Bounded waits at DIRQL.** The core's busy poll limit (`0xFFFFF` reads)
  is acceptable at passive level but too long at interrupt level. Calls made
  from the ISR or a synchronised routine must use a small poll limit, and
  treat a timeout as a reported error, not a spin.
- **UART TX queues** are filled at passive/DPC level under a spin lock and
  drained by the ISR. Only the enable/mask transition goes through the
  interrupt lock.

## MIDI design

- **SWXG1–3 share UART 1.** A small per-port selector tracker emits
  `F5 <port+1>` on a port switch, re-sends running status after a switch,
  and forces a selector refresh every 100 messages, as the original does.
  External MIDI OUT uses UART 0 without `F5`.
- **Message-boundary scheduling.** A client's SysEx must never be split by
  another port's bytes or by a mixer SysEx. The UART 1 writer interleaves
  sources only at MIDI message boundaries.
- **Boot gate.** Opening an SWXG render stream succeeds immediately. Bytes
  written before the gate opens are queued, not dropped, and the gate is
  re-armed on every D0 entry. This is more conservative than the original,
  which waits only once.
- **Transport to Windows.** Each endpoint is a PortCls `IMiniportMidi`
  (KS MIDI 1.0), the path Windows 10 and 11 still support for PCI hardware.
  How it appears through the newer Windows MIDI Services stack needs checking
  on a real system, but it does not affect the driver's design.

## PCM design (later)

- **WaveRT**, one render miniport per `WAVEOUT` and one capture per `WAVEIN`,
  starting with a single stereo 16-bit render.
- Buffers from `AllocateBufferWithNotification`, restricted to below 4 GiB
  and physically contiguous. Two notifications per buffer match the
  half-buffer interrupt. Position comes from `TP(n)` through a synchronised
  DSP-window read.
- **One rate at a time.** Data ranges advertise only the current board rate.
  A rate change is a validated adapter setting, applied only when no stream
  is active. There is no raw `PORT1` access from user mode.

## Topology design

- Nodes: master volume/mute, per-`WAVEOUT` volume/mute, wave-in volume/mute.
  The mapping to XG SysEx is in [pcm-path.md](pcm-path.md#mixer).
- Values are cached per device, saved in the registry, and replayed after
  D0 entry once the boot gate opens.
- Synth volume stays under MIDI control (XG master volume, part volumes), as
  it did with the original driver.

## Power

- **D0 entry:**
  1. startup (core);
  2. UART init for channels 0 and 1;
  3. TX/RX enable;
  4. re-arm the boot gate;
  5. replay the mixer once the gate opens;
  6. restart any stream PortCls asks for.
- **D0 exit:** stop DSP transfers, `TRPIF = 0`, flush the UART queues.
- **Refusing power changes:** report busy while PCM streams run, as the
  original's `QueryPowerChangeState` does. MIDI does not block power changes.

## Security rules

- No user-mode path to MMIO, `PORT1`, DSP RAM or the DSP window. The
  original's private `SWXGControl` and `SWDSControl` properties are not
  reproduced.
- No mapping of kernel buffers into user space. That rules out the original
  ASIO mechanism; ASIO is out of scope.
- SysEx from clients is forwarded as data, and length-bounded by the queues.

## Milestones

| # | Deliverable | Gate to proceed |
|---:|---|---|
| M0 | KMDF bring-up: startup + trace (current) | `recipe_trace.py` reports `OK` on cold, warm and resume starts |
| M1 | PortCls adapter + SWXG1 render, polled TX, then interrupt TX | Middle C sounds and stops; no stuck interrupts |
| M2 | SWXG2/3, external MIDI OUT/IN, `F5` refresh, SysEx | XG reset and SysEx round trips; RX under load |
| M3 | Topology: master volume/mute via XG SysEx | Controls work and survive resume |
| M4 | WaveRT: one stereo 16-bit render at 44.1 kHz | Glitch-free playback; correct position |
| M5 | Capture, multiple streams, rate setting | Driver Verifier clean under load |

Out of scope: ASIO, DS2416, S/PDIF mode selection beyond the startup defaults.
