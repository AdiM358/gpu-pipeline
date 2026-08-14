PROJ_ROOT := $(shell pwd)

RTL_DIR   = $(PROJ_ROOT)/rtl
TB_DIR    = $(PROJ_ROOT)/tb
BUILD_DIR = build

.PHONY: all run_axi run_fetch run_geom run_top run_pixel clean wave

all: run_axi run_fetch run_geom run_top run_pixel

run_axi:
	mkdir -p $(BUILD_DIR)/axi
	verilator -Wall --cc --trace --exe \
		-I$(RTL_DIR) \
		$(RTL_DIR)/axi_lite_s_intf.sv $(TB_DIR)/axi_lite_s_intf_tb.cpp \
		--top-module axi_lite_s_intf -Mdir $(BUILD_DIR)/axi
	make -C $(BUILD_DIR)/axi -f Vaxi_lite_s_intf.mk Vaxi_lite_s_intf
	./$(BUILD_DIR)/axi/Vaxi_lite_s_intf

run_fetch:
	mkdir -p $(BUILD_DIR)/fetch
	verilator -Wall --cc --trace --exe \
		-I$(RTL_DIR) \
		$(RTL_DIR)/vertex_fetch.sv $(TB_DIR)/vertex_fetch_tb.cpp \
		--top-module vertex_fetch -Mdir $(BUILD_DIR)/fetch
	make -C $(BUILD_DIR)/fetch -f Vvertex_fetch.mk Vvertex_fetch
	./$(BUILD_DIR)/fetch/Vvertex_fetch

run_geom:
	mkdir -p $(BUILD_DIR)/geom
	verilator -Wall --cc --trace --exe \
		-I$(RTL_DIR) \
		$(RTL_DIR)/geom_engine.sv $(TB_DIR)/geom_engine_tb.cpp \
		--top-module geom_engine -Mdir $(BUILD_DIR)/geom
	make -C $(BUILD_DIR)/geom -f Vgeom_engine.mk Vgeom_engine
	./$(BUILD_DIR)/geom/Vgeom_engine

run_persp:
	mkdir -p $(BUILD_DIR)/persp
	verilator -Wall --cc --trace --exe \
		-I$(RTL_DIR) \
		$(RTL_DIR)/persp_viewport.sv $(TB_DIR)/persp_viewport_tb.cpp \
		--top-module persp_viewport -Mdir $(BUILD_DIR)/persp
	make -C $(BUILD_DIR)/persp -f Vpersp_viewport.mk Vpersp_viewport
	./$(BUILD_DIR)/persp/Vpersp_viewport

run_prim:
	mkdir -p $(BUILD_DIR)/prim
	verilator -Wall --cc --trace --exe \
		-I$(RTL_DIR) \
		$(RTL_DIR)/prim_assembly.sv $(TB_DIR)/prim_assembly_tb.cpp \
		--top-module prim_assembly -Mdir $(BUILD_DIR)/prim
	make -C $(BUILD_DIR)/prim -f Vprim_assembly.mk Vprim_assembly
	./$(BUILD_DIR)/prim/Vprim_assembly

run_rast:
	mkdir -p $(BUILD_DIR)/rast
	verilator -Wall --cc --trace --exe \
		-I$(RTL_DIR) \
		$(RTL_DIR)/rasterizer.sv $(TB_DIR)/rasterizer_tb.cpp \
		--top-module rasterizer -Mdir $(BUILD_DIR)/rast
	make -C $(BUILD_DIR)/rast -f Vrasterizer.mk Vrasterizer
	./$(BUILD_DIR)/rast/Vrasterizer

run_pixel:
	mkdir -p $(BUILD_DIR)/pixel
	verilator -Wall --cc --trace --exe \
		-I$(RTL_DIR) \
		$(RTL_DIR)/pixel_map.sv $(TB_DIR)/pixel_map_tb.cpp \
		--top-module pixel_map -Mdir $(BUILD_DIR)/pixel
	make -C $(BUILD_DIR)/pixel -f Vpixel_map.mk Vpixel_map
	./$(BUILD_DIR)/pixel/Vpixel_map

run_top:
	mkdir -p $(BUILD_DIR)/top
	verilator -Wall --cc --trace --exe --public-flat-rw \
		-I$(RTL_DIR) \
		$(RTL_DIR)/gpu_top.sv $(TB_DIR)/gpu_top_tb.cpp \
		--top-module gpu_top -Mdir $(BUILD_DIR)/top
	make -C $(BUILD_DIR)/top -f Vgpu_top.mk Vgpu_top
	./$(BUILD_DIR)/top/Vgpu_top

wave:
	gtkwave waveform.vcd &

clean:
	rm -rf $(BUILD_DIR) obj_dir* *.vcd