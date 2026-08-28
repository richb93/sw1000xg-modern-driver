# SW1000XG ordinary DSP/MPR loader protocol

Static reconstruction from Yamaha `yswds.sys` 1.01.0014.1, SHA-256
`0e475f6af8a4e23af5d5fd3533533fc50433ae10e98aaab9d96368cda35eb4b4`.
Nothing from the package was executed. Names below are descriptive names for a
future clean driver unless explicitly identified as Yamaha symbols.

## Result

Synth-first startup does **not** upload the large `dsp000`/`deq000` effect
images. Yamaha's ordinary reset path loads a smaller baseline: eleven MPR
arrays, five global-register records, two 64-word bootstrap buffers, and a six-word
SW1000-specific CESCR block. The large images belong to later DSP/ASIO
configuration.

Payload words are copied as native little-endian 32-bit values. There is no byte
swap, decompression, payload relocation, checksum, or executable mapping on the
PC.

## Hardware upload engine

Five upload windows exist at offsets `0x3F000`, `0x3F100`, `0x3F200`,
`0x3F300`, and `0x3F400`. Ordinary SW1000 startup uses window 0.

| Offset from window | Width | Reconstructed role |
|---:|---:|---|
| `+0x00` through `+0x7C` | 32-bit | 32-word staging FIFO |
| `+0x80` | 32-bit | busy/length/address-high command |
| `+0x84` | 32-bit | destination-address-low/commit command |

Before each transaction, poll bit 31 of `window+0x80` until clear, with the
original driver's limit of `0xFFFFF` polls. A timeout is failure.

For `(dsp_index, destination, count, words)`:

1. Set `chunk = min(count, 32)`.
2. Copy `chunk` 32-bit words to `window+0x00`, `+0x04`, and so on.
3. Write `((chunk << 16) | (destination >> 16))` to `window+0x80`.
4. Write `((destination << 16) | dsp_index)` to `window+0x84` to commit.
5. Advance destination and source by `chunk`, and repeat.

The ordinary baseline uses `dsp_index = 0`. A clean driver must retain a bounded
poll and return a real error rather than spinning forever in kernel mode.

## Baseline MPR sequence

Normal startup selects bank 0 of the SW1000-specific pointer table. `SendMpr`
submits these slots through window 0 with destination zero:

| Order | File | Words | Bytes |
|---:|---|---:|---:|
| 0 | `mpr_00.bin` | `0x140` | `0x500` |
| 1 | `mpr_01.bin` | `0x140` | `0x500` |
| 2 | `mpr_02.bin` | `0x140` | `0x500` |
| 3 | `mpr_03.bin` | `0x140` | `0x500` |
| 4 | `mpr_04.bin` | `0x0A0` | `0x280` |
| 5 | `mpr_05.bin` | `0x140` | `0x500` |
| 6 | `mpr_06.bin` | `0x140` | `0x500` |
| 7 | `mpr_07.bin` | `0x040` | `0x100` |
| 8 | `mpr_08.bin` | `0x006` | `0x018` |
| 9 | `mpr_09.bin` | `0x020` | `0x080` |
| 10 | `mpr_10.bin` | `0x040` | `0x100` |

Repeated destination zero is intentional. These appear to be distinct,
sequence-sensitive MPR classes interpreted by the board, not pieces of one
linear firmware image.

## Ordinary non-ASIO ordering

The ordinary reset controller calls `InitDSP`, which performs this outer prelude:

1. Write `0x00000080` to `PORT0` at `BAR+0x3FF00`.
2. Under the `PORT1` lock, write `0x11A18000` to `BAR+0x3FF10`.
3. Stall for 10 ms.
4. Call `dspInitMprOnly(1)`.
5. Under the same lock, write `0xD1A18000` to `BAR+0x3FF10`.
6. Apply `set_sub_code(0, 0, 2, 0)`, then release the lock. Despite accepting
   four parameters, this build emits the fixed triplet `set_dit(0, 0x0204)`,
   `set_dit(1, 0x0008)`, and `set_dit(2, 0x0000)`; the caller's four values are
   not read by the compiled routine.
7. Disable all interrupt sources by writing zero to `TRPIF` at `BAR+0x3FF04`.

The recovered `dspInitMprOnly(1)` path is:

1. Acquire the adapter lock and assert master mute.
2. Assert DSP reset (`NResDSP0 = 0`).
3. Stall for 44 ms.
4. Release DSP reset (`NResDSP0 = 1`).
5. Clear the initial performance-counter timestamp.
6. Clear the top two cached `PORT1` bits and write the result to `BAR+0x3FF10`.
7. Apply `global_register_00.bin` through `global_register_04.bin`, in order,
   through the global-register helper.
8. Upload `bootstrap_zero_a.bin`: window 0, destination `0x700`, 64 words.
9. Send MPR slots 0 through 10, stopping on the first failure.
10. Upload `bootstrap_zero_b.bin`: window 0, destination `0x700`, 64 words.
11. Upload `cescr.bin`: window 0, destination `0x800`, 6 words.
12. Set `TRWF` to zero, then clear `TRWFO`.
13. Clear `PORT1` bit 25 and set DSP run state.
14. Release the lock.

The bootstrap buffers overlap by 63 words in the original image. The second
begins one dword earlier. Preserve that distinction instead of deduplicating
them.

## Global-register record

The helper consumes signed 16-bit fields at offsets `+8`, `+10`, `+14`, and
`+16` of each global record, combining them into three 32-bit writes at
upload-window-relative offsets `+0x88`, `+0x8E`, and `+0x90`. The `+0x8E`
address is rounded down by the old driver's 32-bit MMIO calculation. A clean
implementation should reproduce that aligned access deliberately and leave the
fields unnamed until hardware validation. Record 0 produces first packed value
`0x0117011E`; records 1 through 4 each produce `0x00FF011F`. Their other two
packed values are zero.

## Fixed digital-interface serial protocol

The three `set_dit` lines are bits in the cached `PORT1` shadow:

| Helper | `PORT1` bit | Mask |
|---|---:|---:|
| `W_CCK` | 29 | `0x20000000` |
| `W_CLDW` | 28 | `0x10000000` |
| `W_CDO0` | 27 | `0x08000000` |

Each helper preserves bit 31 from the previous shadow, changes its own bit, and
rewrites the complete shadow to `BAR+0x3FF10`. `dif_clk` produces one high-to-low
clock pulse. `set_dit` first drives `W_CLDW` low, sets each data bit before its
clock pulse, and finally drives `W_CLDW` high. There are no explicit delays
between edges.

| Mode | Data order | Trailer | Total clocks |
|---:|---|---|---:|
| 0 | 32 bits, MSB first | `0, 0` | 34 |
| 1 | low nibble, bit 3 first | `0, 1` | 6 |
| 2 | 32 bits, MSB first | `1, 0` | 34 |

Ordinary startup therefore serialises `(mode 0, 0x00000204)`, then `(mode 1,
0x8)`, then `(mode 2, 0)`, leaving clock low and latch high.

## Remaining boundary

The SW1000 `SendCESCR` hook is no longer opaque: it is exactly one six-word
transfer from `cescr.bin` to destination `0x800` through DSP window 0. There is
no separate post-bootstrap callback; the previously unidentified virtual slot is
`SendCESCR` itself.

`NResDSP0` is `PORT1` bit 23. `TRWF` and `TRWFO` are DSP RAM locations `0xC100`
and `0xC101`; their startup values are both zero and are written through the
same synchronized, busy-polled RAM accessor.

`dspSetRun(1)` uses two single-value writes through DSP window 0. Yamaha's API
keeps the low 16-bit target selector separate from the destination address: the
first writes `0x40000000` with target `0x100`, destination `0xE0`; the second
writes `0x147F0020` with target `0x700`, destination `0x0F`. The off-state
values are zero and `0x14000020`. `PORT1` is initialized to zero by
`CAdapterCommon::Init`; later changes update its cached full 32-bit value and
rewrite `BAR+0x3FF10`.

The baseline sequence is now sufficiently bounded to implement behind a
test-only kernel abstraction. The reset prelude and complete global-record set
are now enumerated. `set_sub_code` is also reduced to its three fixed `set_dit`
transactions; those transactions bit-serialise their values using the driver's
`W_CCK`, `W_CDO0`, and `dif_clk` helpers. Remaining static work is to decode the
semantic purpose of the packed global fields and the individual serial clock/data
bits, and to trace which portions are strictly required for audible synth output.
`SCR` belongs to the separate ASIO reconfiguration path, not this ordinary
baseline. Do not test a partial sequence by writing PCI BARs from user mode.
