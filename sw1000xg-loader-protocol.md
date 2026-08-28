# SW1000XG ordinary DSP/MPR loader protocol

Static reconstruction from Yamaha `yswds.sys` 1.01.0014.1, SHA-256
`0e475f6af8a4e23af5d5fd3533533fc50433ae10e98aaab9d96368cda35eb4b4`.
Nothing from the package was executed. Names below are descriptive names for a
future clean driver unless explicitly identified as Yamaha symbols.

## Result

Synth-first startup does **not** upload the large `dsp000`/`deq000` effect
images. Yamaha's ordinary reset path loads a smaller baseline: eleven MPR
arrays, one global-register record, and two 64-word bootstrap buffers. The large
images belong to later DSP/ASIO configuration.

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

The recovered `dspInitMprOnly(0)` path is:

1. Acquire the adapter lock and assert master mute.
2. Assert DSP reset (`NResDSP0 = 0`).
3. Stall for 44 ms.
4. Release DSP reset (`NResDSP0 = 1`).
5. Send MPR slots 0 through 10, stopping on the first failure.
6. Clear the top two cached global-control bits and program the result.
7. Apply `global_register_record.bin` through the global-register helper.
8. Upload `bootstrap_zero_a.bin`: window 0, destination `0x700`, 64 words.
9. Perform the SW1000-specific `SendCESCR` step.
10. Upload `bootstrap_zero_b.bin`: window 0, destination `0x700`, 64 words.
11. Perform the SW1000 post-bootstrap hook.
12. Apply reset/run hooks, clear `TRWFO`, clear global-control bit 25, and set
    DSP run state.
13. Release the lock.

The bootstrap buffers overlap by 63 words in the original image. The second
begins one dword earlier. Preserve that distinction instead of deduplicating
them.

## Global-register record

The helper consumes signed 16-bit fields at offsets `+8`, `+10`, `+14`, and
`+16` of `global_register_record.bin`, combining them into three 32-bit writes at
upload-window-relative offsets `+0x88`, `+0x8E`, and `+0x90`. The `+0x8E`
address is rounded down by the old driver's 32-bit MMIO calculation. A clean
implementation should reproduce that aligned access deliberately and leave the
fields unnamed until hardware validation.

## Remaining boundary

The bulk transfer and baseline payload list are now exact enough to implement
behind a test-only kernel abstraction. Before powering hardware, the short
SW1000-specific `SendCESCR`, post-bootstrap, reset/run, and initial global-control
hooks still need their individual register writes annotated. Do not test a
partial sequence by writing PCI BARs from user mode.
