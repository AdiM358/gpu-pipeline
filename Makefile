# GPU pipeline build and regression.
#
#   make all            build + run every test; non-zero exit if any check fails
#   make test-<name>    build + run one test (e.g. make test-rasterizer)
#   make TRACE=1 ...    build with VCD support (run a test binary with +trace)
#   make SEED=7 ...     stimulus seed passed to every test as +seed=N
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
TB_COMMON := $(wildcard tb/common/*.h tb/common/*.cpp sw/*.h sw/*.hpp)

TESTS := axil_regs vertex_fetch geom_engine persp_viewport \
         prim_assembly rasterizer pixel_map gpu_top

VFLAGS := -Wall --x-assign unique --x-initial unique -O3 \
          --build -j $(JOBS) \
          -CFLAGS "-std=c++17 -O2 -I$(CURDIR)/tb/common -I$(CURDIR)/sw"
ifeq ($(TRACE),1)
  VFLAGS += --trace
endif
ifeq ($(COVERAGE),1)
  VFLAGS += --coverage-line
endif

RUN_ARGS := +seed=$(SEED) +verilator+seed+$(SEED)

.PHONY: all clean lint $(addprefix test-,$(TESTS))

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
		-Mdir $$(build_$(1)) $$(vflags_$(1)) $(RTL) $$(abspath $$(src_$(1))) > $$(build_$(1))/build.log 2>&1 \
		|| { cat $$(build_$(1))/build.log; exit 1; }

test-$(1): $$(build_$(1))/V$$(top_$(1))
	@cd $$(build_$(1)) && ./V$$(top_$(1)) $(RUN_ARGS) +cov=$(CURDIR)/$$(build_$(1))/coverage.dat $$(args_$(1))
endef

$(foreach t,$(TESTS),$(eval $(call TEST_RULES,$(t))))

lint:
	$(VERILATOR) --lint-only -Wall --top-module gpu_top $(RTL)

clean:
	rm -rf $(BUILD_DIR) *.vcd
