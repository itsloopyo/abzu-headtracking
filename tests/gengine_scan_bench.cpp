// Microbenchmark for the LocateGEngine .data scan inner loop.
//
// LocateGEngine walks every 8-byte slot of the host module's writable sections
// (~tens of MB for a UE shipping EXE) looking for a pointer to a live UEngine.
// The original code read each slot through SafeRead -- an SEH __try/__except per
// qword. A loaded module's own sections are guaranteed mapped (this is exactly
// why cameraunlock-core's pattern_scanner reads module bytes directly), so the
// per-slot SEH is pure overhead; only the heap-candidate dereference can fault
// and needs the guard.
//
// This pins the win as a measured number: SEH-per-slot vs direct read over a
// synthetic 45 MB buffer (matching ABZU's ~0x2adde30 .data span). Both variants
// must find the SAME candidate slots (correctness), then we time them.
//
// MSVC only (SEH). Build under a VS dev prompt:
//   cl /O2 /EHsc /std:c++17 gengine_scan_bench.cpp

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <windows.h>

template <typename T>
static bool SafeRead(uintptr_t addr, T& out) {
    __try { out = *reinterpret_cast<const T*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static inline bool LooksLikePointer(uintptr_t base, size_t size, uintptr_t p) {
    if (p < 0x10000) return false;
    if ((p & 7) != 0) return false;
    return p < base || p >= base + size;
}

// Original: SafeRead per slot.
static size_t ScanSeh(uintptr_t base, size_t size, uintptr_t modBase, size_t modSize) {
    size_t hits = 0;
    const auto step = sizeof(uintptr_t);
    for (uintptr_t p = base; p + step <= base + size; p += step) {
        uintptr_t candidate = 0;
        if (!SafeRead(p, candidate)) continue;
        if (!LooksLikePointer(modBase, modSize, candidate)) continue;
        ++hits;
    }
    return hits;
}

// Optimised: direct read of the module's own (guaranteed-mapped) slots.
static size_t ScanDirect(uintptr_t base, size_t size, uintptr_t modBase, size_t modSize) {
    size_t hits = 0;
    const uintptr_t end = base + (size & ~(sizeof(uintptr_t) - 1));
    for (uintptr_t p = base; p + sizeof(uintptr_t) <= end + sizeof(uintptr_t) && p < end; p += sizeof(uintptr_t)) {
        const uintptr_t candidate = *reinterpret_cast<const uintptr_t*>(p);
        if (!LooksLikePointer(modBase, modSize, candidate)) continue;
        ++hits;
    }
    return hits;
}

int main() {
    const size_t kBytes = 45u * 1024 * 1024;        // ~ABZU .data span
    std::vector<uint8_t> buf(kBytes, 0);
    auto* words = reinterpret_cast<uintptr_t*>(buf.data());
    const size_t nwords = kBytes / sizeof(uintptr_t);

    const uintptr_t modBase = 0x140000000ull;
    const size_t    modSize = 0x4000000ull;          // 64 MB image

    // Seed a realistic mix: ~5% look like live heap pointers, rest junk/null.
    uintptr_t rng = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < nwords; ++i) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        if ((rng & 0xF) == 0) words[i] = 0x200000000ull | (rng & ~uintptr_t(7));  // heap-ish, aligned
        else if ((rng & 0xF) == 1) words[i] = modBase + (rng % modSize);          // into-module (rejected)
        else words[i] = (rng & 1) ? 0 : (rng & 0xFFFF);                           // null / tiny
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(buf.data());

    size_t hSeh = ScanSeh(base, kBytes, modBase, modSize);
    size_t hDir = ScanDirect(base, kBytes, modBase, modSize);
    std::printf("candidates: seh=%zu direct=%zu  %s\n", hSeh, hDir,
                hSeh == hDir ? "(identical)" : "(MISMATCH!)");
    if (hSeh != hDir) { std::printf("FAIL: scans disagree\n"); return 1; }

    auto bench = [&](const char* tag, auto fn) {
        using clock = std::chrono::steady_clock;
        const int reps = 20;
        volatile size_t sink = 0;
        auto t0 = clock::now();
        for (int r = 0; r < reps; ++r) sink += fn(base, kBytes, modBase, modSize);
        auto t1 = clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
        std::printf("  %-14s %7.3f ms/scan  (sink=%zu)\n", tag, ms, (size_t)sink);
        return ms;
    };

    double sehms = bench("seh-per-slot", ScanSeh);
    double dirms = bench("direct", ScanDirect);
    std::printf("  speedup: %.2fx\n", sehms / dirms);
    return 0;
}
