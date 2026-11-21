# Phase 75 — Inside NVIDIA's network: its activations pass by pass against ours (2026-11-21)

`phase74` compared the composed picture: our A and B against NVIDIA's at 0.989-0.997 correlation
of the change, a residual of 6-13 % of it, its source not established. The owner, 11-21: find it
the exact way, by reading the vendor's buffers between its passes. This note is how, what the
DLL's graph looks like from inside, and the answer for network A. Everything below was measured
on the owner's RTX 5060 Ti (driver 620.73.04, 310.9.16's DLL), unless it says otherwise.

## How

- `nr_vendor.c` (`work/vendor/`) already hooks the DLL's `GetProcAddress`. The DLL takes
  `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` from `vulkan-1.dll` that way and every
  other Vulkan function through those two, so the hook hands it ours, which return the real
  functions but for a few wrapped ones: the CUDA modules and functions (by name), the kernel
  launches, the buffers with their device addresses, the image views with their handles.
- A launch's arguments are CUDA's `extra` buffer (`BUFFER_POINTER`, the bytes, `BUFFER_SIZE`,
  `END`), given with `extraCount` 1. Read as 64-bit words, every pointer into one of the DLL's
  buffers prints as that buffer and an offset, so the trace says what each pass reads and
  writes.
- `NRV_VKTRACE=1` logs it all. `NRV_TAPS=L:B:O:N,...` copies N bytes at offset O of the DLL's
  buffer B right after launch L, inside the DLL's own command buffer (a barrier, a
  `vkCmdCopyBuffer` into a host-visible buffer, a barrier). Every buffer the DLL creates gets
  `TRANSFER_SRC`, so that it can be copied from.
- Checked: without the variables the output is byte for byte what it was; with 18 taps, and with
  another 18, it is byte for byte the untapped output.
- The frame is a 512x320 crop of Mortal Kombat 1's close-up (a face, `work/mk1cap/023` at 40,
  120), so that the whole scratch is 12.4 MB.

## The graph from inside

- **One evaluation of A is 156 launches**, the 156 slots of skchen17's capture
  (`phase3-execution-order.md`) one for one; B's is 80. Nine CUDA modules, 96 functions for A.
  - Launch 0: `cc_cb_clear`, which zeroes the kernels' synchronisation counters.
  - Launch 1: `pre_block`. The input adapter, block 0 at full resolution and its pool: it writes
    the full-resolution skip and level 1's input.
  - Each level: a first kernel (`..._inpview_tilesync`), then `..._chained` ones, then the
    transition (`..._ds_wait`), which writes the level's skip and the next level's input.
  - Launches 24-56: the 512-channel split blocks, four kernels a block (`qkv`, `proj`, `ffwd`,
    `ffwd_proj`), then `proj_pool` and `final_head`.
  - Launches 57-98: the ViT, from a repack into tokens to a repack back.
  - Launches 99-153: the decoder. `dec_input_upsample`, the split blocks, then per level an
    `..._upsample_tilesync` kernel that reads the skip, chained ones and `..._outview_wait`.
  - Launch 154: `post_block`. The head and the composition in one kernel: it reads the decoder's
    output, the full-resolution skip and the colour.
  - Launch 155: `cg2r_copy_kernel`, into the output image.
- Three buffers:
  - `b0`, the weights, 147 719 680 bytes, the arena of `phase3`;
  - `b1`, its copy on the host side;
  - `b2`, the scratch, 12 422 144 bytes at 512x320. Its first 0x18c00 bytes are the counters;
    after them every activation lies as E4M3 bytes, the levels taking turns in the same places.
- Its levels at 512x320:
  - full resolution, the pre-block only;
  - then 160x256 with 32 channels, 80x128 with 64, 40x64 with 128, 20x32 with 256;
  - 12x16 with 512 (10x16 padded);
  - the ViT on 8x8 tokens of 1024.
- The control lanes are scalars among the pre-block's arguments: style, tone, structure and the
  two automatic-mask strengths. These are our feature channels 10-14 to the value: 0, 1, 1, -1,
  -1 by default. With `UseAutoMask=1` the last two are 1 and 1, with its skin strength at 2 they
  are 2 and 1, and at 0 they are 0 and 1. No mask image is bound.

## Its two layouts

- **A level's input and the decoder's outputs: `[c/16][h][w][16]`.** Within each 16 the
  channels come in pairs from its two halves: 0 1 8 9 2 3 10 11 4 5 12 13 6 7 14 15.
- **The skips, the bottleneck's and the ViT's outputs, and every block's output inside a level:
  `mma.m16n8k32`'s A fragment for E4M3.**
  - A tile is 16 pixels (a 4x4 block) by 32 channels, 16 bytes a lane. Byte i of lane l is row
    `(l >> 2) + 8 (i >> 2 & 1)` and column `4 (l & 3) + (i & 3) + 16 (i >> 3)`.
  - The 4x4 blocks go in raster order, a block's channel groups of 32 come one after another,
    and within 32 the pairs are interleaved as above.
- How they were found:
  - Read as E4M3, the bytes had our tensor's mean |x| to four digits: 0.5517 against 0.5518.
  - The stream's autocorrelation was 0.97562 at a lag of 16 bytes, where ours in NHWC is 0.97563
    at 32: 16 bytes to a pixel.
  - Each of the vendor's planes then matched one of our channels at 0.998-1.000.
  - The skips' mean pairs by byte position, and their autocorrelation at 4 and 64, gave the
    fragment.
- `work/vendor/taps_compare.py` decodes both.

## Where the residual comes from (network A)

**Stage by stage, each side running freely** (`taps_compare.py`; our side is the resident
graph's `capture`, and the level skips read from their buffers after the run):

| point | correlation | the same E4M3 value | relative RMS |
| --- | --- | --- | --- |
| full-resolution skip (block 0) | 0.99981 | 89.8 % | 1.9 % |
| level 1's input | 0.99985 | 88.9 % | 1.7 % |
| level 1's skip | 0.99089 | 28.7 % | 13.4 % |
| level 2's input | 0.99777 | 36.9 % | 6.7 % |
| level 2's skip | 0.93981 | 14.0 % | 34.7 % |
| level 3's input | 0.99334 | 20.0 % | 11.5 % |
| level 3's skip | 0.94758 | 9.9 % | 32.4 % |
| level 4's input | 0.98426 | 15.3 % | 17.7 % |
| level 4's skip | 0.97143 | 12.4 % | 23.9 % |
| the bottleneck's input | 0.98702 | 31.0 % | 16.1 % |
| the bottleneck's output | 0.98288 | 17.2 % | 18.5 % |
| the ViT's output | 0.98024 | 17.0 % | 19.8 % |
| decoder, 512 channels | 0.98483 | 16.3 % | 17.4 % |
| decoder, 256 | 0.98458 | 15.5 % | 17.5 % |
| decoder, 128 | 0.98417 | 15.2 % | 17.8 % |
| decoder, 64 | 0.98866 | 18.4 % | 15.0 % |
| decoder, 32 (d1) | 0.99447 | 23.7 % | 10.5 % |

The skips' difference is spread over most channels, even across a window's positions, hardly
higher at the frame's border and not concentrated in large values. Pooling into the next level
averages it down.

**Block by block, each of our blocks on the vendor's own input** (`taps_blocks.py`: the
vendor's input to the block, decoded from the taps, through our CPU reference's block, against
the vendor's output of it):

| encoder level | blocks | correlation | the same E4M3 value | relative RMS |
| --- | --- | --- | --- | --- |
| 1 (32 channels) | 1-4 | 0.99985-0.99995 | 90.6-96.1 % | 1.0-1.7 % |
| 2 (64) | 5-8 | 0.99955-0.99989 | 76.5-88.2 % | 1.5-3.0 % |
| 3 (128) | 9-14 | 0.99945-0.99969 | 63.9-74.3 % | 2.5-3.3 % |
| 4 (256) | 15-22 | 0.99944-0.99981 | 67.4-84.7 % | 2.0-3.4 % |

A transition block counts twice there, for its skip and for the next level's input.

**The head and the composition on the vendor's tensors** (`taps_head.py`). The vendor's d1 and
full-resolution skip went through our head block and `nr_frame.compose` at intensity 1, against
the vendor's output of the frame:

- 92.18 % of the bytes are the same, 99.97 % within one level, the largest difference is 5, and
  the mean |difference| is 0.079 of a level.
- Our whole run against it: 18.15 %, 53.78 %, 24 and 1.629.

**Not FP16 accumulation.** Every GEMM in our reference was redirected (`nr_model.MATMUL`) to
float32 partial sums of 32, or of 16, added to a half accumulator rounded after each: the
vendor's accumulators are half (`phase4`).

- With 32, a level-1 block's share of the same values rises by 0.2-1.3 points, and level 2's
  by 0.2-0.6.
- With 16 it falls, at level 1 by 1-2.6 points.

## Reading

- **The vendor's network is ours, block by block.** On its own input every encoder block lands
  at 0.9994-0.99995 of the vendor's output, the same E4M3 byte for 64-96 % of the values. The
  input assembly and block 0 are the same to 0.9998, 90 % of the values the same byte, so the
  feature assembly, its deterministic noise and the lanes are ruled out. The head and the
  composition are ruled out as well: on the vendor's tensors they leave 0.08 of a level.
- **The residual is those small differences, compounded.** Run freely, they grow level by level
  through the encoder to 17 % relative RMS, stay there through the bottleneck, the ViT and the
  decoder, and fall to 10.5 % at d1. That is the chaos the brief warns of
  (`phase9-numerics.md`), now measured at each stage.
- A block's own difference grows with its width: 1-1.7 % at 32 channels, 1.5-3 % at 64,
  2-3.4 % at 128-256. It is
  arithmetic inside the fused kernels, not the accumulator as emulated here. The candidates are
  the softmax, the cosine normalisation, the gates and the halves kept between their steps. The
  vendor keeps those in registers and shared memory, out of a buffer tap's reach.

## Not established

- Network B: traced (80 launches, the same families), not tapped.
- The bottleneck's, the ViT's and the decoder's blocks one by one: only their stage outputs were
  compared.
- Which arithmetic inside a block differs.

## Where things are

`work/vendor/`:

- `nr_vendor.c` with `NRV_VKTRACE`, `NRV_TAPS`, `NRV_STOP_AFTER` and `NRV_MVEC`;
- `trace_table.py` (a trace as a table of launches, pointers and sizes);
- `taps_ours.py` (our stages on a frame);
- `taps_layout.py`, `taps_compare.py`, `taps_blocks.py` and `taps_head.py`;
- the traces in `trace/`; the taps, our captures (`ours_A.npz`) and `map_A.txt`, which says
  which launch and offset holds which of our capture's points, in `taps/`.
