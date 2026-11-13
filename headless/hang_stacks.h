// SPDX-License-Identifier: GPL-3.0-or-later
// EDEN_HANG_STACKS=1 (diagnostics): where are the threads when the game hangs? Every thread that
// names itself (the SetCurrentThreadName hook) is noted with its stack. A watchdog follows a
// heartbeat (Eden::Performance::RefreshFreeMemory, which the GPU thread's work calls several times
// a second); when it has not moved for 8 s, each noted thread is sent SIGUSR1, whose handler notes
// the thread's rip and rsp, and the stack from rsp up (64 KiB, or the top 64 KiB of the stack when
// the handler did not run) is scanned for values in the app's code. stderr.log gets one line per
// thread, EDEN_HANG_STACK <name> rip=<offset> <offsets>, offsets from the code's start as in a
// crash report, nearest call first; tools/symbolize-hang.py names them. Reported once, then again
// after 60 s if the hang lasts.
#pragma once
#if defined(PS5_NATIVE) || defined(__linux__)
#include <pthread.h>
#if defined(PS5_NATIVE)
#include <pthread_np.h>
#endif
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

// The app's code, from the console link (tools/unwind.ld); weak: host tests link without it.
extern const char __eden_text_start[] __attribute__((weak));
extern const char __eden_text_end[] __attribute__((weak));

namespace Eden::HangStacks {
struct Slot {
    pthread_t thread{};
    char name[32]{};
    std::uintptr_t low{}, high{};
    std::atomic<std::uintptr_t> rip{0}, rsp{0};
    std::atomic<bool> taken{false};
};
inline Slot slots[256];
inline std::atomic<unsigned> slot_count{0};
inline std::atomic<unsigned long long> heartbeat{0};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_HANG_STACKS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

inline void Beat() { heartbeat.fetch_add(1, std::memory_order_relaxed); }

inline void OnSignal(int, siginfo_t*, void* context) {
    const pthread_t self = pthread_self();
    const unsigned count = slot_count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; ++i) {
        if (!pthread_equal(slots[i].thread, self)) continue;
        const auto* bytes = static_cast<const unsigned char*>(context);
        std::uintptr_t rip = 0, rsp = 0;
#if defined(PS5_NATIVE)
        // The console's context: rip at 0xe0, rsp at 0xf8 (headless/crash_report.cpp).
        std::memcpy(&rip, bytes + 0xe0, sizeof(rip));
        std::memcpy(&rsp, bytes + 0xf8, sizeof(rsp));
#else
        (void)bytes;
        rsp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
        slots[i].rip.store(rip, std::memory_order_relaxed);
        slots[i].rsp.store(rsp, std::memory_order_relaxed);
        slots[i].taken.store(true, std::memory_order_release);
        return;
    }
}

inline void Report(double seconds) {
    const auto code = reinterpret_cast<std::uintptr_t>(__eden_text_start);
    const auto code_end = __eden_text_end ? reinterpret_cast<std::uintptr_t>(__eden_text_end) : ~std::uintptr_t{0};
    const unsigned count = slot_count.load(std::memory_order_acquire);
    std::fprintf(stderr, "EDEN_HANG no heartbeat for %.0f s; stacks of %u named threads follow\n", seconds, count);
    for (unsigned i = 0; i < count; ++i) {
        Slot& slot = slots[i];
        slot.taken.store(false, std::memory_order_relaxed);
        const bool signalled = pthread_kill(slot.thread, SIGUSR1) == 0;
        for (int wait = 0; signalled && wait < 50 && !slot.taken.load(std::memory_order_acquire); ++wait)
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        const bool taken = slot.taken.load(std::memory_order_acquire);
        std::uintptr_t from = taken ? slot.rsp.load(std::memory_order_relaxed) : 0;
        if (from < slot.low || from >= slot.high) from = slot.high > (64u << 10) ? slot.high - (64u << 10) : slot.low;
        from &= ~std::uintptr_t{7};
        const std::uintptr_t to = from + (64u << 10) < slot.high ? from + (64u << 10) : slot.high;
        const std::uintptr_t rip = slot.rip.load(std::memory_order_relaxed);
        char line[1024];
        int used = std::snprintf(line, sizeof(line), "EDEN_HANG_STACK %s %s rip=%lx:", slot.name,
                                 taken ? "regs" : "top", static_cast<unsigned long>(rip >= code && rip < code_end ? rip - code : rip));
        unsigned found = 0;
        for (std::uintptr_t at = from; at + 8 <= to && found < 96 && used < int(sizeof(line)) - 20; at += 8) {
            std::uintptr_t value;
            std::memcpy(&value, reinterpret_cast<const void*>(at), sizeof(value));
            if (value <= code || value >= code_end) continue;
            used += std::snprintf(line + used, sizeof(line) - used, " %lx", static_cast<unsigned long>(value - code));
            ++found;
        }
        std::fprintf(stderr, "%s\n", line);
    }
    std::fflush(stderr);
}

inline void Watch() {
    unsigned long long last = heartbeat.load(std::memory_order_relaxed);
    auto changed = std::chrono::steady_clock::now();
    double reported_at = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const unsigned long long now_beat = heartbeat.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (now_beat != last || now_beat == 0) {
            last = now_beat;
            changed = now;
            reported_at = 0;
            continue;
        }
        const double seconds = std::chrono::duration<double>(now - changed).count();
        if (seconds >= 8 && (reported_at == 0 || seconds - reported_at >= 60)) {
            Report(seconds);
            reported_at = seconds;
        }
    }
}

// Every thread that names itself.
inline void Note(const char* name) {
    if (!Enabled()) return;
    static std::once_flag started;
    std::call_once(started, [] {
        struct sigaction action {};
        action.sa_sigaction = OnSignal;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(SIGUSR1, &action, nullptr);
        std::thread(Watch).detach();
    });
    const unsigned index = slot_count.load(std::memory_order_relaxed);
    if (index >= sizeof(slots) / sizeof(slots[0])) return;
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    const unsigned at = slot_count.load(std::memory_order_relaxed);
    if (at >= sizeof(slots) / sizeof(slots[0])) return;
    Slot& slot = slots[at];
    slot.thread = pthread_self();
    std::snprintf(slot.name, sizeof(slot.name), "%s", name);
    for (char& c : slot.name) if (c == ' ') c = '_';
    pthread_attr_t attr;
    void* stack = nullptr;
    size_t size = 0;
#if defined(PS5_NATIVE)
    if (pthread_attr_init(&attr) == 0) {
        if (pthread_attr_get_np(pthread_self(), &attr) == 0 && pthread_attr_getstack(&attr, &stack, &size) == 0) {
            slot.low = reinterpret_cast<std::uintptr_t>(stack);
            slot.high = slot.low + size;
        }
        pthread_attr_destroy(&attr);
    }
#else
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        if (pthread_attr_getstack(&attr, &stack, &size) == 0) {
            slot.low = reinterpret_cast<std::uintptr_t>(stack);
            slot.high = slot.low + size;
        }
        pthread_attr_destroy(&attr);
    }
#endif
    slot_count.store(at + 1, std::memory_order_release);
}
}
#endif
