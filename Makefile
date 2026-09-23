# GPU pipeline build and regression.
#
#   make all            build + run every test; non-zero exit if any check fails
#   make test-<name>    build + run one test (e.g. make test-rasterizer)
#   make TRACE=1 ...    build with VCD support (run a test binary with +trace)
#   make SEED=7 ...     stimulus seed passed to every test as +seed=N
#   make coverage       all tests with line coverage, merged, per-module report
#   make clean
#
# Each test is tb/<name>_tb.cpp driving top module <name> unless overridden
# with top_<name>. Extra Verilator flags per test go in vflags_<name>, extra
# runtime plusargs in args_<name>.

VERILATOR ?= verilator
BUILD_DIR ?= build
SEED      ?= 1
JOBS      ?= $(shell nproc 2>/dev/null || echo 4)
TRACE     ?= 0
COVERAGE  ?= 0

RTL       := $(abspath $(wildcard rtl/*.sv))
MODEL_SRC := $(abspath model/gpu_model.cpp)
TB_COMMON := $(wildcard tb/common/*.h tb/common/*.cpp sw/*.h sw/*.hpp model/*.h) $(MODEL_SRC)

TESTS := axil_regs vertex_fetch geom_engine recip_pipe persp_viewport \
         prim_assembly tri_setup rasterizer rasterizer_s1 rasterizer_s8 \
         rop gpu_top

VFLAGS := -Wall --x-assign unique --x-initial unique -O3 \
          --build -j $(JOBS) \
          -CFLAGS "-std=c++20 -O2 -I$(CURDIR)/tb/common -I$(CURDIR)/sw -I$(CURDIR)/model"
ifeq ($(TRACE),1)
  VFLAGS += --trace
endif
ifeq ($(COVERAGE),1)
  VFLAGS += --coverage-line
endif

RUN_ARGS := +seed=$(SEED) +verilator+seed+$(SEED)

# Per-test overrides
vflags_recip_pipe := -GPAY_W=32
# The rasterizer is verified at several span widths (default SPAN=4).
top_rasterizer_s1   := rasterizer
src_rasterizer_s1   := tb/rasterizer_tb.cpp
vflags_rasterizer_s1 := -GSPAN=1
top_rasterizer_s8   := rasterizer
src_rasterizer_s8   := tb/rasterizer_tb.cpp
vflags_rasterizer_s8 := -GSPAN=8

.PHONY: all clean lint coverage seeds $(addprefix test-,$(TESTS))

all: $(addprefix test-,$(TESTS))
	@echo "All $(words $(TESTS)) tests passed."

# $(1) = test name
define TEST_RULES
top_$(1)   ?= $(1)
src_$(1)   ?= tb/$(1)_tb.cpp
build_$(1) := $(BUILD_DIR)/$(1)

$$(build_$(1))/V$$(top_$(1)): $(RTL) $$(src_$(1)) $(TB_COMMON) Makefile
	@mkdir -p $$(build_$(1))
	$(VERILATOR) $(VFLAGS) --cc --exe --top-module $$(top_$(1)) \
		-Mdir $$(build_$(1)) $$(vflags_$(1)) $(RTL) $$(abspath $$(src_$(1))) $(MODEL_SRC) > $$(build_$(1))/build.log 2>&1 \
		|| { cat $$(build_$(1))/build.log; exit 1; }

test-$(1): $$(build_$(1))/V$$(top_$(1))
	@cd $$(build_$(1)) && ./V$$(top_$(1)) $(RUN_ARGS) +cov=$$(abspath $$(build_$(1)))/coverage.dat $$(args_$(1))
endef

$(foreach t,$(TESTS),$(eval $(call TEST_RULES,$(t))))

# Line coverage over the whole regression. Each test writes coverage.dat;
# they are merged and summarised per RTL file (one module per file).
COV_DIR := $(BUILD_DIR)/cov
coverage:
	$(MAKE) BUILD_DIR=$(COV_DIR) COVERAGE=1 all
	verilator_coverage --write $(COV_DIR)/merged.dat $(COV_DIR)/*/coverage.dat
	rm -rf $(COV_DIR)/annotated
	verilator_coverage --annotate $(COV_DIR)/annotated --annotate-min 1 $(COV_DIR)/merged.dat
	python3 scripts/coverage_report.py $(COV_DIR)/merged.dat | tee $(COV_DIR)/report.md

# Re-run the (already built) regression with several stimulus seeds.
SEEDS ?= 2 3 4 5 6
seeds: all
	@for s in $(SEEDS); do $(MAKE) --no-print-directory all SEED=$$s || exit 1; done

lint:
	$(VERILATOR) --lint-only -Wall --top-module gpu_top $(RTL)

clean:
	rm -rf $(BUILD_DIR) *.vcd
