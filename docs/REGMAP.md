# Register map

AXI4-Lite slave, 32-bit data, 8-bit byte address (256 B window). Registers
are word-aligned; address bits [1:0] are ignored. The C definitions are in
[`sw/gpu_regs.h`](../sw/gpu_regs.h) and the driver in
[`sw/gpu_driver.hpp`](../sw/gpu_driver.hpp). `tb/axil_regs_tb.cpp` checks every
offset, access type, and reset value listed here.

**Bus behaviour**

- AW and W may arrive in either order or in the same cycle. The write is
  performed once both have been accepted, and then B is returned. One write
  and one read can be outstanding at a time.
- `WSTRB` is honoured byte by byte. For `CTRL` and `STATUS`, only byte 0 has
  an effect.
- Unmapped offsets respond `SLVERR`; reads of them return 0 and writes are
  dropped. Writes to read-only registers respond `OKAY` and have no effect.

## Summary

| Offset | Name | Access | Reset | Description |
|-------:|------|:------:|------:|-------------|
| 0x00 | `CTRL` | W | 0 | Command register. Reads as 0. |
| 0x04 | `STATUS` | R / W1C | 0 | Busy, done, error. |
| 0x08 | `IRQ_EN` | RW | 0 | Interrupt enables. |
| 0x0C | `ID` | R | `0x47505531` | "GPU1". |
| 0x10 | `VBUF_BASE` | RW | 0 | Vertex buffer byte address. Bits [3:0] read as 0 (16 B aligned). |
| 0x14 | `VERTEX_COUNT` | RW | 0 | Number of vertices (3 per triangle). |
| 0x18 | `RASTER_CFG` | RW | 0 | Face culling. |
| 0x1C | `CLEAR_COLOR` | RW | 0 | [15:0] RGB565 value written by `CLEAR`. |
| 0x20 | `FB_INFO` | R | `H<<16 \| W` | Framebuffer size (320x240 by default). |
| 0x40 | `PERF_CYCLES` | R | 0 | Cycles from command accept to DONE. |
| 0x44 | `PERF_VERTS` | R | 0 | Vertices delivered by vertex fetch. |
| 0x48 | `PERF_TRI_IN` | R | 0 | Triangles assembled. |
| 0x4C | `PERF_TRI_CULLED` | R | 0 | Triangles removed by face culling or zero area. |
| 0x50 | `PERF_TRI_CLIPPED` | R | 0 | Triangles removed by frustum reject, near plane, or guard band. |
| 0x54 | `PERF_FRAG_GEN` | R | 0 | Fragments produced by the rasterizer. |
| 0x58 | `PERF_FRAG_PASS` | R | 0 | Fragments that passed the depth test and were written. |
| 0x5C | `PERF_RAST_BUSY` | R | 0 | Cycles the rasterizer held a triangle. |
| 0x80–0xBC | `MVP[r][c]` | RW | 0 | 4x4 model-view-projection matrix, Q16.16, row-major: offset `0x80 + 4*(4r + c)`. |

All performance counters are 32 bits, cleared when a command is accepted, and
count until DONE, so they describe the most recent command.

## Fields

### `CTRL` (0x00)

| Bit | Name | Description |
|----:|------|-------------|
| 0 | `DRAW` | Fetch `VERTEX_COUNT` vertices from `VBUF_BASE` and render them. |
| 1 | `CLEAR` | Fill the colour buffer with `CLEAR_COLOR` and the depth buffer with `0xFFFF`. If `DRAW` is also set, the clear runs first. |

A command written while `STATUS.BUSY` is set is ignored. Accepting a command
clears `STATUS.DONE` and all performance counters.

### `STATUS` (0x04)

| Bit | Name | Access | Description |
|----:|------|:------:|-------------|
| 0 | `BUSY` | R | A command is executing. |
| 1 | `DONE` | R/W1C | Sticky. Set when a command has completely finished, meaning every fragment has been written, not just the last vertex fetched. |
| 2 | `FETCH_ERR` | R/W1C | Sticky. The vertex fetch saw a non-OKAY `RRESP` or a misplaced `RLAST`. The draw is abandoned and still ends with `DONE`. |

If a completion event arrives in the same cycle as a write-1-to-clear, the
event wins, so a completion is never lost.

### `IRQ_EN` (0x08)

`irq = (STATUS.DONE & IRQ_EN[0]) | (STATUS.FETCH_ERR & IRQ_EN[1])`, level-sensitive.

### `RASTER_CFG` (0x18)

| Bit | Name | Description |
|----:|------|-------------|
| 0 | `CULL_BACK` | Discard back-facing triangles. |
| 1 | `FRONT_CW` | 0: counter-clockwise triangles are front-facing (OpenGL default). 1: clockwise. |

Winding is judged in normalised device coordinates (y up), after the
perspective divide. Zero-area triangles are always discarded and counted in
`PERF_TRI_CULLED`.

## Programming sequence

```c
write(MVP[0..15], matrix_q16);          // row-major
write(VBUF_BASE, buffer_addr);          // 16-byte aligned
write(VERTEX_COUNT, 3 * triangles);
write(RASTER_CFG, GPU_RASTER_CULL_BACK);
write(CLEAR_COLOR, rgb565);
write(CTRL, GPU_CTRL_CLEAR | GPU_CTRL_DRAW);
while (!(read(STATUS) & GPU_STATUS_DONE)) ;   // or wait for irq
write(STATUS, GPU_STATUS_DONE);         // acknowledge
```

`gpu::Driver` in `sw/gpu_driver.hpp` implements this sequence. The
testbenches run the same driver code over the AXI-Lite BFM.
