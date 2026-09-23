// Shared Verilator testbench harness.
//
// - CHECK / CHECK_EQ record failures instead of aborting (and are not
//   compiled out by NDEBUG the way assert() is). tb::finish() turns the
//   tally into the process exit code, so `make` fails on any failed check.
// - Plusargs: +seed=N (stimulus RNG), +trace (VCD dump, needs TRACE=1 build),
//   +cov=FILE (coverage output, needs COVERAGE=1 build).
// - Cycle model: the testbench drives inputs, calls tick() (one posedge),
//   then samples outputs. Call settle() after changing inputs if you need
//   combinational outputs (e.g. a ready that depends on valid) before tick().
#pragma once

#include <verilated.h>
#if VM_TRACE
#include <verilated_vcd_c.h>
#endif

#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>

namespace tb {

inline uint64_t g_checks = 0;
inline uint64_t g_fails = 0;
constexpr uint64_t kMaxReported = 25;

inline void report_fail(const char* file, int line, const std::string& msg) {
    ++g_fails;
    if (g_fails <= kMaxReported) {
        std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
    } else if (g_fails == kMaxReported + 1) {
        std::fprintf(stderr, "  ... further failures suppressed\n");
    }
}

// Prints the PASS/FAIL line and returns the process exit code.
inline int summarize(const std::string& name, uint64_t cycles, uint64_t seed) {
    if (g_fails == 0 && g_checks > 0) {
        std::printf("[PASS] %s: %" PRIu64 " checks, %" PRIu64 " cycles, seed %" PRIu64 "\n",
                    name.c_str(), g_checks, cycles, seed);
        return 0;
    }
    if (g_checks == 0)
        std::printf("[FAIL] %s: no checks were executed\n", name.c_str());
    else
        std::printf("[FAIL] %s: %" PRIu64 " of %" PRIu64 " checks failed (seed %" PRIu64 ")\n",
                    name.c_str(), g_fails, g_checks, seed);
    return 1;
}

inline std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

}  // namespace tb

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++tb::g_checks;                                                      \
        if (!(cond)) tb::report_fail(__FILE__, __LINE__, #cond);             \
    } while (0)

#define CHECK_MSG(cond, ...)                                                 \
    do {                                                                     \
        ++tb::g_checks;                                                      \
        if (!(cond)) tb::report_fail(__FILE__, __LINE__, tb::fmt(__VA_ARGS__)); \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        ++tb::g_checks;                                                      \
        const auto tb_a_ = (actual);                                         \
        const auto tb_e_ = (expected);                                       \
        if (!(tb_a_ == tb_e_))                                               \
            tb::report_fail(__FILE__, __LINE__,                              \
                tb::fmt("%s == %s (got 0x%llx, expected 0x%llx)", #actual,   \
                        #expected, (unsigned long long)(tb_a_),              \
                        (unsigned long long)(tb_e_)));                       \
    } while (0)

// Unrecoverable testbench condition (e.g. timeout): report and exit now.
#define FATAL(...)                                                           \
    do {                                                                     \
        std::fprintf(stderr, "  FATAL %s:%d: %s\n", __FILE__, __LINE__,      \
                     tb::fmt(__VA_ARGS__).c_str());                          \
        std::exit(1);                                                        \
    } while (0)

namespace tb {

// Owns the Verilator context, the DUT, optional tracing and coverage.
// DUT must have `clk` and `rst_n` ports.
template <class DUT>
class Sim {
public:
    Sim(int argc, char** argv, const char* name) : name_(name) {
        ctx_ = std::make_unique<VerilatedContext>();
        ctx_->commandArgs(argc, argv);
        ctx_->randReset(2);  // randomize uninitialized state (catches missing resets)
        dut = std::make_unique<DUT>(ctx_.get());

        const char* s = ctx_->commandArgsPlusMatch("seed=");
        seed = (s && *s) ? std::strtoull(s + 6, nullptr, 0) : 1;
        rng.seed(seed);
#if VM_TRACE
        if (*ctx_->commandArgsPlusMatch("trace")) {
            ctx_->traceEverOn(true);
            trace_ = std::make_unique<VerilatedVcdC>();
            dut->trace(trace_.get(), 99);
            trace_->open((std::string(name) + ".vcd").c_str());
        }
#endif
        dut->clk = 0;
        dut->rst_n = 0;
        dut->eval();
    }

    ~Sim() {
#if VM_TRACE
        if (trace_) trace_->close();
#endif
    }

    // One full clock cycle: rising edge then falling edge.
    void tick() {
        dut->clk = 1;
        dut->eval();
        dump();
        ++cycle;
        dut->clk = 0;
        dut->eval();
        dump();
    }

    void settle() { dut->eval(); }

    void reset(int cycles = 5) {
        dut->rst_n = 0;
        for (int i = 0; i < cycles; ++i) tick();
        dut->rst_n = 1;
        tick();
    }

    // Uniform helpers on the seeded RNG.
    uint32_t rand_u32() { return static_cast<uint32_t>(rng()); }
    int rand_range(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
    bool chance(int percent) { return rand_range(0, 99) < percent; }

    // Prints the summary, writes coverage, returns the process exit code.
    int finish() {
        dut->final();
#if VM_COVERAGE
        const char* c = ctx_->commandArgsPlusMatch("cov=");
        std::string path = (c && *c) ? std::string(c + 5) : name_ + ".cov.dat";
        ctx_->coveragep()->write(path.c_str());
#endif
        return summarize(name_, cycle, seed);
    }

    VerilatedContext* ctx() { return ctx_.get(); }

    std::unique_ptr<DUT> dut;
    uint64_t cycle = 0;
    uint64_t seed = 1;
    std::mt19937_64 rng;

private:
    void dump() {
#if VM_TRACE
        if (trace_) trace_->dump(ctx_->time());
        ctx_->timeInc(1);
#endif
    }

    std::string name_;
    std::unique_ptr<VerilatedContext> ctx_;
#if VM_TRACE
    std::unique_ptr<VerilatedVcdC> trace_;
#endif
};

}  // namespace tb
