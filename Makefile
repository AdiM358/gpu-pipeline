# Absolute path to project root
PROJ_ROOT := $(shell pwd)

# Directory Definitions
RTL_DIR   = $(PROJ_ROOT)/rtl
TB_DIR    = $(PROJ_ROOT)/tb
BUILD_DIR = build

.PHONY: all run_axi run_fetch clean wave

all: run_axi run_fetch

# Build & Run AXI-Lite Testbench
run_axi:
	verilator -Wall --cc --trace --exe \
		-I$(RTL_DIR) \
		$(RTL_DIR)/axi_lite_s_intf.sv $(TB_DIR)/axi_lite_s_intf_tb.cpp \
		--top-module axi_lite_s_intf -Mdir $(BUILD_DIR)/axi
	make -C $(BUILD_DIR)/axi -f Vaxi_lite_s_intf.mk Vaxi_lite_s_intf
	./$(BUILD_DIR)/axi/Vaxi_lite_s_intf

# Build & Run Vertex Fetch Testbench
run_fetch:
	verilator -Wall --cc --trace --exe \
		-I$(RTL_DIR) \
		$(RTL_DIR)/vertex_fetch.sv $(TB_DIR)/vertex_fetch_tb.cpp \
		--top-module vertex_fetch -Mdir $(BUILD_DIR)/fetch
	make -C $(BUILD_DIR)/fetch -f Vvertex_fetch.mk Vvertex_fetch
	./$(BUILD_DIR)/fetch/Vvertex_fetch

wave:
	gtkwave waveform.vcd &

clean:
	rm -rf $(BUILD_DIR) obj_dir* *.vcd