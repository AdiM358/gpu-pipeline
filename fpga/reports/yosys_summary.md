# Yosys synthesis cross-check (not Vivado)

Tool: Yosys 0.33 (git sha1 2584903a060); sv2v v0.0.12. Command: `python3 fpga/yosys_report.py` in the gpu-synth image (docker/Dockerfile.synth). `synth_xilinx -family xc7 -flatten`. Cell counts only: no timing and no logic depth (see the script's docstring).

| Design | LUT | FF | CARRY4 | DSP48E1 | RAMB36E1 | LUTRAM cells | SRL |
|---|---:|---:|---:|---:|---:|---:|---:|
| gpu_top RAST_SPAN=1 | 10273 | 8903 | 1071 | 37 | 76 | 33 | 299 |
| gpu_top RAST_SPAN=2 | 10751 | 8905 | 1108 | 37 | 76 | 33 | 299 |
| gpu_top RAST_SPAN=4 (default) | 10901 | 8975 | 1169 | 48 | 76 | 33 | 299 |
| gpu_top RAST_SPAN=8 | 11563 | 9183 | 1302 | 81 | 76 | 33 | 299 |
| baseline geom_engine | 939 | 898 | 144 | 48 | 0 | 0 | 0 |
| new geom_engine | 361 | 712 | 56 | 12 | 0 | 0 | 74 |
| baseline persp_viewport | 40296 | 152 | 7810 | 4 | 0 | 0 | 0 |
| new persp_viewport | 3393 | 2362 | 416 | 18 | 0 | 0 | 187 |
| baseline rasterizer | 1977 | 937 | 401 | 24 | 0 | 0 | 0 |
| new tri_setup | 2521 | 2627 | 272 | 6 | 0 | 12 | 6 |
| new rasterizer (SPAN=4) | 1830 | 1423 | 287 | 11 | 0 | 0 | 0 |
| baseline pixel_map | 89 | 213 | 19 | 2 | 0 | 0 | 0 |
| new rop (incl. 2 buffers) | 479 | 235 | 10 | 1 | 76 | 0 | 32 |

Default configuration as a share of the XC7Z020 (capacities from DS190: 53,200 LUTs, 106,400 FFs, 140 RAMB36, 220 DSP48E1). LUT excludes LUTs used as LUTRAM/SRL, so Vivado's 'Slice LUTs' will be higher:

- LUT: 10901 / 53200 = 20.5%
- FF: 8975 / 106400 = 8.4%
- RAMB36E1: 76 / 140 = 54.3%
- DSP48E1: 48 / 220 = 21.8%
