#include "tinyio/detail/context.hpp"

#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <gtest/gtest.h>

#include "tinyio/detail/sanitizer_hooks.hpp"
#include "tinyio/detail/stack.hpp"

// The context switch on its own, without io_uring: these tests run anywhere
// the CPU does, including an emulated x86_64 container on an arm64 host,
// where io_uring is not available.

namespace {

using namespace tinyio::detail;

// One main context and one child context, switched exactly the way
// IO::ResumeFiber() / IO::SuspendFiber() do it, sanitizer hooks included.
struct Harness {
    context::Context main_ctx{};
    context::Context child_ctx{};
    ToolFiberState main_tools{};
    ToolFiberState child_tools{};
    stack::Stack child_stack{};

    // What the child records, checked from the main context.
    void* received_arg = nullptr;
    int counter = 0;
    std::uintptr_t frame_misalignment = ~std::uintptr_t{0};
    char formatted[64] = {};
    std::uint64_t child_checksum = 0;
    int child_rounding_first = 0;
    int child_rounding_after = 0;

    explicit Harness(const context::EntryFn entry) {
        auto allocated = stack::AllocateStack(64 * 1024, 4096);
        if (!allocated) {
            std::abort();
        }
        child_stack = *allocated;
        ToolsCaptureCurrentThread(main_tools);
        ToolsCreateFiber(child_tools, child_stack);
        context::Make(child_ctx, child_stack.high, entry, this);
    }

    ~Harness() {
        ToolsDestroyFiber(child_tools);
        stack::ReleaseStack(child_stack);
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    TIO_NO_SANITIZE_ADDRESS void Resume() noexcept {
        ToolsBeforeSwitch(main_tools, &child_tools, &child_stack);
        context::Switch(main_ctx, child_ctx);
        ToolsAfterSwitch(main_tools);
    }

    TIO_NO_SANITIZE_ADDRESS void Suspend() noexcept {
        ToolsBeforeSwitch(child_tools, &main_tools, nullptr);
        context::Switch(child_ctx, main_ctx);
        ToolsAfterSwitch(child_tools, &main_tools);
    }

    // First thing every child entry does (see IO::FiberEntry).
    static Harness& Enter(void* arg) noexcept {
        auto& h = *static_cast<Harness*>(arg);
        ToolsAfterSwitch(h.child_tools, &h.main_tools);
        h.received_arg = arg;
        return h;
    }

    // Entry functions must never return: park for good. The harness is
    // destroyed without resuming the child again.
    [[noreturn]] void Park() noexcept {
        for (;;) {
            Suspend();
        }
    }
};

// Opaque to the optimizer, so the values below cannot be folded into
// constants and have to live in registers across the switches.
std::uint64_t Opaque(const std::uint64_t v) {
    volatile std::uint64_t sink = v;
    return sink;
}

}  // namespace

TEST(ContextTest, FirstSwitchCallsEntryWithItsArgument) {
    Harness h([](void* arg) noexcept { Harness::Enter(arg).Park(); });

    h.Resume();

    EXPECT_EQ(h.received_arg, &h);
}

TEST(ContextTest, PingPongAlternates) {
    constexpr int k_rounds = 1000;
    Harness h([](void* arg) noexcept {
        Harness& self = Harness::Enter(arg);
        for (;;) {
            ++self.counter;
            self.Suspend();
        }
    });

    for (int i = 1; i <= k_rounds; ++i) {
        h.Resume();
        ASSERT_EQ(h.counter, i);
    }
}

// Values the compiler keeps in callee-saved registers on either side must
// come back intact. With optimizations on, the loops below hold their
// accumulators in rbx/r12-r15 (x86_64) or x19-x28 (aarch64).
TEST(ContextTest, CalleeSavedValuesSurviveSwitches) {
    Harness h([](void* arg) noexcept {
        Harness& self = Harness::Enter(arg);
        std::uint64_t a = Opaque(3);
        std::uint64_t b = Opaque(5);
        std::uint64_t c = Opaque(7);
        std::uint64_t d = Opaque(11);
        std::uint64_t e = Opaque(13);
        std::uint64_t f = Opaque(17);
        for (int i = 0; i < 100; ++i) {
            a = a * 31 + b;
            b = b * 37 + c;
            c = c * 41 + d;
            d = d * 43 + e;
            e = e * 47 + f;
            f = f * 53 + a;
            self.Suspend();
        }
        self.child_checksum = a ^ b ^ c ^ d ^ e ^ f;
        self.Park();
    });

    std::uint64_t a = Opaque(2);
    std::uint64_t b = Opaque(4);
    std::uint64_t c = Opaque(6);
    std::uint64_t d = Opaque(8);
    std::uint64_t e = Opaque(10);
    std::uint64_t f = Opaque(12);
    for (int i = 0; i < 101; ++i) {
        a = a * 59 + f;
        b = b * 61 + a;
        c = c * 67 + b;
        d = d * 71 + c;
        e = e * 73 + d;
        f = f * 79 + e;
        h.Resume();
    }
    const std::uint64_t main_checksum = a ^ b ^ c ^ d ^ e ^ f;

    // The same computations without any switch in the middle.
    std::uint64_t ca = 3, cb = 5, cc = 7, cd = 11, ce = 13, cf = 17;
    for (int i = 0; i < 100; ++i) {
        ca = ca * 31 + cb;
        cb = cb * 37 + cc;
        cc = cc * 41 + cd;
        cd = cd * 43 + ce;
        ce = ce * 47 + cf;
        cf = cf * 53 + ca;
    }
    std::uint64_t ma = 2, mb = 4, mc = 6, md = 8, me = 10, mf = 12;
    for (int i = 0; i < 101; ++i) {
        ma = ma * 59 + mf;
        mb = mb * 61 + ma;
        mc = mc * 67 + mb;
        md = md * 71 + mc;
        me = me * 73 + md;
        mf = mf * 79 + me;
    }

    EXPECT_EQ(h.child_checksum, ca ^ cb ^ cc ^ cd ^ ce ^ cf);
    EXPECT_EQ(main_checksum, ma ^ mb ^ mc ^ md ^ me ^ mf);
}

// The ABI wants sp 16-byte aligned at every call. A misaligned new stack
// shows up as a misaligned frame, and as a crash in code that uses aligned
// SSE stores for its locals -- printf-style floating-point formatting is
// the classic victim.
TEST(ContextTest, NewStackIsAligned) {
    Harness h([](void* arg) noexcept {
        Harness& self = Harness::Enter(arg);
        self.frame_misalignment = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)) % 16;
        std::snprintf(self.formatted, sizeof(self.formatted), "%.3f %Lf", 1.25, 2.5L);
        self.Park();
    });

    h.Resume();

    EXPECT_EQ(h.frame_misalignment, 0u);
    EXPECT_STREQ(h.formatted, "1.250 2.500000");
}

// MXCSR and the x87 control word (x86_64), FPCR (aarch64): the FP control
// registers are callee-saved, so a new context inherits its creator's
// rounding mode, and each context keeps its own across switches.
#if defined(__x86_64__) || defined(__aarch64__)
TEST(ContextTest, FloatingPointControlIsPerContext) {
    ASSERT_EQ(std::fesetround(FE_UPWARD), 0);
    Harness h([](void* arg) noexcept {
        Harness& self = Harness::Enter(arg);
        self.child_rounding_first = std::fegetround();
        std::fesetround(FE_TOWARDZERO);
        self.Suspend();
        self.child_rounding_after = std::fegetround();
        self.Park();
    });

    h.Resume();
    const int main_rounding = std::fegetround();
    h.Resume();
    std::fesetround(FE_TONEAREST);

    EXPECT_EQ(h.child_rounding_first, FE_UPWARD);
    EXPECT_EQ(main_rounding, FE_UPWARD);
    EXPECT_EQ(h.child_rounding_after, FE_TOWARDZERO);
}
#endif
