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

# ---------------------------------------------------------------- demo
# Renders FRAMES frames of the animated scene (each checked against the
# model), writes out/demo/frame_NNN.ppm and docs/img/demo.gif.
FRAMES ?= 48
top_demo  := gpu_top
args_demo  = +frames=$(FRAMES) +out=$(abspath out/demo)
$(eval $(call TEST_RULES,demo))

.PHONY: demo perf
demo:
	@mkdir -p out/demo docs/img
	$(MAKE) --no-print-directory test-demo
	python3 scripts/make_gif.py out/demo docs/img/demo.gif

# ---------------------------------------------------------------- perf
# The same benchmark scenes at each rasterizer span width -> out/perf.md
PERF_SPANS := 1 2 4 8
define PERF_RULES
top_perf_s$(1)    := gpu_top
src_perf_s$(1)    := tb/perf_tb.cpp
vflags_perf_s$(1) := -GRAST_SPAN=$(1)
args_perf_s$(1)   := +span=$(1)
$$(eval $$(call TEST_RULES,perf_s$(1)))
endef
$(foreach s,$(PERF_SPANS),$(eval $(call PERF_RULES,$(s))))

perf: $(addprefix $(BUILD_DIR)/perf_s,$(addsuffix /Vgpu_top,$(PERF_SPANS)))
	@mkdir -p out
	@for s in $(PERF_SPANS); do \
	   $(MAKE) --no-print-directory test-perf_s$$s > out/perf_s$$s.log 2>&1 || { cat out/perf_s$$s.log; exit 1; }; \
	 done
	@{ echo "| RAST_SPAN | scene | triangles | rasterized | fragments | draw cycles | fragments/cycle | rasterizer busy cycles |"; \
	   echo "|---:|---|---:|---:|---:|---:|---:|---:|"; \
	   grep -h '^| ' $(foreach s,$(PERF_SPANS),out/perf_s$(s).log); } | tee out/perf.md

# Line coverage over the whole regression. Each test writes coverage.dat;
# they are merged and summarised per RTL file (one module per file).
COV_DIR := $(BUILD_DIR)/cov
coverage:
	$(MAKE) BUILD_DIR=$(COV_DIR) COVERAGE=1 all
	verilator_coverage --write $(COV_DIR)/merged.dat $(COV_DIR)/*/coverage.dat
	rm -rf $(COV_DIR)/annotated
	verilator_coverage --annotate $(COV_DIR)/annotated --annotate-min 1 $(COV_DIR)/merged.dat
	python3 scripts/coverage_report.py $(COV_DIR)/merged.dat | tee $(COV_DIR)/report.md

# ---------------------------------------------------------------- baseline
# Before/after: builds the ORIGINAL vertex_fetch and pixel_map straight from
# the `baseline` git tag and measures them on the same workloads as the new
# units' tests. -> out/baseline_bench.txt
BASE_DIR   := $(BUILD_DIR)/baseline
BASE_FLAGS := $(filter-out -Wall,$(VFLAGS)) -Wno-fatal
.PHONY: baseline-bench
baseline-bench:
	@mkdir -p $(BASE_DIR)/rtl out
	git -c safe.directory='*' show baseline:rtl/vertex_fetch.sv > $(BASE_DIR)/rtl/vertex_fetch.sv
	git -c safe.directory='*' show baseline:rtl/pixel_map.sv > $(BASE_DIR)/rtl/pixel_map.sv
	$(VERILATOR) $(BASE_FLAGS) --cc --exe --top-module vertex_fetch -Mdir $(BASE_DIR)/fetch \
		$(abspath $(BASE_DIR)/rtl/vertex_fetch.sv) $(abspath bench/baseline_fetch_bench.cpp) > $(BASE_DIR)/fetch.log 2>&1
	$(VERILATOR) $(BASE_FLAGS) --cc --exe --top-module pixel_map -GSCREEN_W=320 -GSCREEN_H=240 \
		-Mdir $(BASE_DIR)/pixel $(abspath $(BASE_DIR)/rtl/pixel_map.sv) $(abspath bench/baseline_pixel_bench.cpp) \
		> $(BASE_DIR)/pixel.log 2>&1
	@{ $(BASE_DIR)/fetch/Vvertex_fetch $(RUN_ARGS) && $(BASE_DIR)/pixel/Vpixel_map $(RUN_ARGS); } | tee out/baseline_bench.txt

# Re-run the (already built) regression with several stimulus seeds.
SEEDS ?= 2 3 4 5 6
seeds: all
	@for s in $(SEEDS); do $(MAKE) --no-print-directory all SEED=$$s || exit 1; done

lint:
	$(VERILATOR) --lint-only -Wall --top-module gpu_top $(RTL)

clean:
	rm -rf $(BUILD_DIR) *.vcd
