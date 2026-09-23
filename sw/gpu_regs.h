/*
 * GPU register map (AXI4-Lite, 32-bit registers, byte offsets).
 * Authoritative description: docs/REGMAP.md. Kept in sync with rtl/axil_regs.sv
 * by tb/axil_regs_tb.cpp, which checks every offset and reset value below.
 */
#ifndef GPU_REGS_H
#define GPU_REGS_H

#include <stdint.h>

#define GPU_REG_CTRL          0x00u /* W:   command (reads 0)                   */
#define GPU_REG_STATUS        0x04u /* R/W1C                                     */
#define GPU_REG_IRQ_EN        0x08u /* RW:  interrupt enables                    */
#define GPU_REG_ID            0x0Cu /* R:   GPU_ID_VALUE                         */
#define GPU_REG_VBUF_BASE     0x10u /* RW:  vertex buffer address, 16 B aligned  */
#define GPU_REG_VERTEX_COUNT  0x14u /* RW:  vertices to draw (3 per triangle)    */
#define GPU_REG_RASTER_CFG    0x18u /* RW:  culling configuration                */
#define GPU_REG_CLEAR_COLOR   0x1Cu /* RW:  RGB565 clear colour                  */
#define GPU_REG_FB_INFO       0x20u /* R:   [15:0] width, [31:16] height         */

/* Performance counters (R). Reset when a command is accepted. */
#define GPU_REG_PERF_CYCLES       0x40u /* cycles from command accept to done     */
#define GPU_REG_PERF_VERTS        0x44u /* vertices delivered by vertex fetch     */
#define GPU_REG_PERF_TRI_IN       0x48u /* triangles assembled                    */
#define GPU_REG_PERF_TRI_CULLED   0x4Cu /* back-facing or zero-area               */
#define GPU_REG_PERF_TRI_CLIPPED  0x50u /* outside frustum, near plane, guard band*/
#define GPU_REG_PERF_FRAG_GEN     0x54u /* fragments emitted by the rasterizer    */
#define GPU_REG_PERF_FRAG_PASS    0x58u /* fragments that passed the depth test   */
#define GPU_REG_PERF_RAST_BUSY    0x5Cu /* cycles the rasterizer held a triangle  */
#define GPU_NUM_PERF              8u

/* MVP matrix, Q16.16, row-major: element (r,c) at GPU_REG_MVP(r,c). */
#define GPU_REG_MVP0          0x80u
#define GPU_REG_MVP(r, c)     (GPU_REG_MVP0 + 4u * (4u * (uint32_t)(r) + (uint32_t)(c)))

/* CTRL bits */
#define GPU_CTRL_DRAW         (1u << 0)
#define GPU_CTRL_CLEAR        (1u << 1) /* runs before DRAW when both are set */

/* STATUS bits */
#define GPU_STATUS_BUSY       (1u << 0) /* read-only                           */
#define GPU_STATUS_DONE       (1u << 1) /* sticky, write 1 to clear            */
#define GPU_STATUS_FETCH_ERR  (1u << 2) /* sticky, write 1 to clear            */

/* IRQ_EN bits: irq = (DONE & IRQ_EN[0]) | (FETCH_ERR & IRQ_EN[1]) */
#define GPU_IRQ_DONE          (1u << 0)
#define GPU_IRQ_FETCH_ERR     (1u << 1)

/* RASTER_CFG bits */
#define GPU_RASTER_CULL_BACK  (1u << 0)
#define GPU_RASTER_FRONT_CW   (1u << 1) /* 0: counter-clockwise is front (GL) */

#define GPU_ID_VALUE          0x47505531u /* "GPU1" */

/* Fixed-point helpers */
#define GPU_Q16_ONE           0x00010000
static inline uint16_t gpu_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

#endif /* GPU_REGS_H */
