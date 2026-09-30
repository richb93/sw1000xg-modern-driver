# Power, interrupt, UART and startup findings (second static pass)

Static analysis of Yamaha `yswds.sys` 1.01.0014.1, SHA-256
`0e475f6af8a4e23af5d5fd3533533fc50433ae10e98aaab9d96368cda35eb4b4`. The driver
was disassembled only; it was never loaded or executed. Addresses are virtual
addresses at the preferred image base `0x10000`.

## Method

The binary's debug directory carries an embedded CodeView `NB11` block (file
offset `0x2AAA0`, 0x11C78 bytes) with 867 `S_PUB32` public-symbol records.
These give the mangled C++ name of almost every function, vtable and global,
so virtual calls can be resolved through the named vtables
(`??_7CAdapterSW1000@@6BIAdapterCommon@@@` and so on) instead of being guessed.
Most diagnostic strings in the image are unreferenced: the release build
compiled the debug prints out.

`CAdapterSW1000` object layout used below: `+0x0` `IMidiDevice`, `+0x4`
`CPciUart`, `+0x40C8` `CAdapterCommon` (its `IAdapterCommon` vtable),
`+0x40CC` `IAdapterPowerManagement`.

## Startup recipe corrections

The earlier recipe misread three DSP-window calls. `SetDSP` (vtable `+0x1C`)
and `SendDSP` (vtable `+0x14`) take `(window, target_selector, destination, …)`,
and the core commits `window+0x84 = (destination << 16) + target_selector`
(`SetDSP` at `0x1FBB8`, `SendDSPCore` at `0x1FCE8`).

| Step | Earlier recipe | Actual (`dspInitMprOnly`, `0x34008`) |
|---|---|---|
| DSP reset hold | 44 ms | `KeStallExecutionProcessor(0x2C)`: 44 µs |
| Before MPRs | 64 words from `0x138C0` to destination `0x700`, selector 0 | One word: `SetDSP(0, 0x700, 0x40, n1mod0KeyOn)`, value `0x0000FFFF` |
| MPR slot *i* | selector 0, destination 0 | `SendMpr` (`0x2039C`): selector `i << 8`, destination 0 |
| After MPRs | 64 words from `0x138BC` to destination `0x700`, selector 0 | One word: `SetDSP(0, 0x700, 0x40, n1mod0KeyOnOff)`, value `0` |
| CESCR | destination `0x800`, selector 0 | `SendCESCR` (`0x318BA`): selector `0x800`, destination 0 |
| DSP run | two writes, including `0x147F0020` at `0x700/0x0F` | One write, selector `0x100`, destination `0xE0`, value `0x40000000` |

The "two overlapping 64-word bootstrap buffers" were the adjacent 32-bit
globals `n1mod0KeyOnOff` (`0x138BC` = 0) and `n1mod0KeyOn` (`0x138C0` =
`0xFFFF`), read as arrays. `dspInitMprOnly` forces the 32-bit MPR flag on
(`this+0x1C = 1` when `Is32bitMPR()` is false), so `dspSetRun(1)` always takes
the single-write 32-bit path. The `0x147F0020` write belongs to `SetRun16`.

The selector reading is consistent with the pointer-table symbols
(`TblMprPtr_SW1000`, bank 0):

| Slot / selector | Yamaha symbol | Words |
|---:|---|---:|
| 0 / `0x000` | `bcr1MEL` | `0x140` |
| 1 / `0x100` | `deqc1` | `0x140` |
| 2 / `0x200` | `deqm1` | `0x140` |
| 3 / `0x300` | `dspc1` | `0x140` |
| 4 / `0x400` | `dspa1` | `0x0A0` |
| 5 / `0x500` | `dspm1` | `0x140` |
| 6 / `0x600` | (second half of `dspm1`) | `0x140` |
| 7 / `0x700` | `n1mod0` | `0x040` |
| 8 / `0x800` | `n1creg0CMEL` | `0x006` |
| 9 / `0x900` | `n1fgtim0` | `0x020` |
| 10 / `0xA00` | `n1fguram0` | `0x040` |

The key-on/off writes target selector `0x700` (`n1mod0`, "module 0"), and CESCR
(`n1creg0MEL`, 6 words) targets selector `0x800`, the same class and size as
slot 8 (`n1creg0CMEL`).

Everything else in the recipe was confirmed: the prelude, `SendGlobal`
(`0x20334`, offsets `+0x88`, aligned `+0x8C`, `+0x90`), `TRWF`/`TRWFO` through
`SetRAM` at `0xC100`/`0xC101`, the unmute before the run write, `PORT1 =
0xD1A18000`, the three `set_dit` transactions, and `IrqDisableAll` from
`ResetController` (`0x3151C`).

## Power management

`CAdapterSW1000::PowerChangeState` (`0x32120`):

- **D1, D2, D3:** records the new state only. No register is written and the
  interrupt mask is not changed.
- **Return to D0** from another state:
  1. `InitDSP()`: the full ordinary DSP startup above, *without* the
     `IrqDisableAll` that `ResetController` adds on first start;
  2. `CPciUart::InitUART(base)`: both UARTs get `00 00 00 50 4E 10`;
  3. `InitializeHardware(IInterruptSync)`: `SWXGInit` under the interrupt
     lock (reopens SWXG1–3 and MIDI IN), then `EnableTX(0)`, `EnableTX(1)`
     and `EnableRX(0)`;
  4. `MixerRegWrite(i, saved[i])` for every cached mixer register.

`QueryPowerChangeState` (`0x1F4FA`) refuses a change while any of the 16
wave-out or 8 wave-in channels is active; MIDI activity does not block it.

For the new driver, running startup in `EvtDeviceD0Entry` matches the
original. Writing `TRPIF=0` in `EvtDeviceD0Exit` goes beyond what the
original does, and is harmless.

## Interrupt model

- **`TRPIF` (`BAR+0x3FF04`) read** returns pending causes. The ISRs
  (`0x1E29C`, `0x1E778`) read it once, return `STATUS_UNSUCCESSFUL` when
  `(status & enabled_mask) == 0` (shared line), and otherwise dispatch.
- **`TRPIF` write** is the enable mask. Every write is the complete shadow
  (`SetTRPIF`, `0x1E226`): `IrqEnable(bit)` ORs `1 << bit` in,
  `IrqDisable(bit)` clears it, and `IrqDisableAll` writes zero. The
  synchronized variants go through `IInterruptSync::CallSynchronizedRoutine`;
  the `…ISR` variants write directly.
- **No acknowledge write.** No ISR path writes `TRPIF` to clear a cause.
  Causes are cleared by servicing the source, or by masking it.
- **UART TX is level-like.** For UART index *n* (0, 1) the ISR tests bit
  `24 + 2n`, calls `CPciUart::InterruptServiceRoutineUART(n)`, and if that
  returns false (queue drained) calls `IrqDisableISR(24 + 2n)`. A clean driver
  must mask TX when its queue is empty, or the interrupt will keep firing.
- **UART 0 RX** (bit 25) is handled by `CMidiIn::InterruptServiceRoutine`
  (`0x20CAE`), which drains data while status bit `0x02` is set.
- **`PORT1` bit 31 is the global interrupt unmask.** `IrqUnMask` (`0x1E012`)
  sets it; `IrqMask` (`0x1DFDE`) clears it. The startup value `0xD1A18000`
  leaves it set, so after startup `TRPIF` alone decides which sources can
  interrupt.

## PCI-UART details

These corrections and additions apply to the UART section of
`reverse-engineering-report.md`.

| Status bit (`base+1` read) | Meaning | Evidence |
|---:|---|---|
| `0x01` | Transmit ready: write the next byte | `InterruptServiceRoutineUART` (`0x21CE2`) dequeues and writes one byte when it is set |
| `0x02` | Receive data available | `CMidiIn::InterruptServiceRoutine` loops on it, reading `base+0` |
| `0x04` | Transmitter empty (fully drained) | `WaitTX` (`0x21AB2`) polls it, with a bounded timeout that sets an error flag |
| `0x38` | Error conditions | `_GetStatUART` (`0x219C4`) then writes `command_shadow \| 0x10` |

- The earlier report had `0x01` and `0x02` the wrong way round.
- `_SetCmndUART` (`0x21968`) stores the byte as the channel's command shadow
  and then waits `WaitMicroSeconds(0x24)`, **36 µs after every command
  write**, including each of the six init bytes.
- After init the shadow is `0x10`. `EnableTX` writes `shadow | 0x01` and
  `EnableRX` writes `shadow | 0x04`.
- The UART object maps the **same first memory resource** again
  (`FindTranslatedEntry(CmResourceTypeMemory, 0)` in `CAdapterSW1000::Init`)
  and indexes `m_UartAdrs` into it. There is no second BAR.

## XG tone-generator boot wait (H8)

`CAdapterCommon::ClearInitialTimeStamp` records `PcGetTimeInterval(0)` right
after DSP reset is released, inside `dspInitMprOnly`.
`CAdapterSW1000::WaitH8` (`0x31F42`) busy-waits until
`PcGetTimeInterval(timestamp) >= 0x5F5E100` (100,000,000 × 100 ns = **10 s**).
`CMiniportMidiUartSWXG::NewStream` calls `WaitH8` before creating a render
stream, and `IMidiDevice::SendData` calls it too. A flag makes it wait only
once per adapter object, so it is not repeated after resume.

"H8" matches the Hitachi H8 microcontroller family, which fits an XG
tone-generator controller booting after reset. **The first SWXG MIDI byte
should not be sent until 10 s after DSP reset release.** A MIDI test that
starts sooner may be ignored by the board even when the UART path is correct.
The core exposes this as `SWXG_H8_BOOT_MS`.

## Answers to the open questions

1. **Resume:** full re-initialisation on D0 entry, nothing on D1–D3. This
   confirms the `EvtDeviceD0Entry` design.
2. **`PORT1` bit 31 / `TRPIF`:** bit 31 set = global unmask. `TRPIF` reads
   as status, is written as the enable mask, and is never written as an
   acknowledge.
3. **UART:** TX-ready = `0x01`, RX-available = `0x02`, TX-empty = `0x04`,
   error = `0x38`. There is a 36 µs wait after each command byte. The
   command bytes' individual meanings are still unnamed.
4. **Audible synth:** the original's ordinary path is exactly the corrected
   recipe, plus UART init, TX/RX enable and the mixer-register restore, plus
   the 10 s H8 wait. `SWXGInputInit` is only an unreferenced string. The
   mixer defaults (`MixerRegWrite`) are the remaining candidate for
   "initialised but silent" and are the next static target.
5. **Recipe check:** corrected as described above.
