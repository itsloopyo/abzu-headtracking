#include "UEEngine.hpp"

#include "utility/Logging.hpp"
#include "utility/SafeMemory.hpp"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <utility>
#include <vector>

namespace ueht::ue {

// ---------------------------------------------------------------------------
// GEngine discovery
// ---------------------------------------------------------------------------
//
// Strategy: scan the .data section of the host module for pointers whose
// target looks like a live UEngine instance. A live UObject has a vtable
// as its first qword pointing into .text. We confirm by checking that the
// pointed-to UObject's UClass (at offset 0x10 in UE4) matches the known
// UEngine UClass global.
//
// The UEngine UClass slot is build-specific and comes from the build profile.

namespace {

std::atomic<uintptr_t> g_cached_gengine{0};
std::atomic<bool>      g_resolve_attempted{false};

// LocateGEngine is retried every ~120 frames until the engine is constructed,
// which on a cold start is the whole splash-and-menu stretch. Each failure
// diagnostic is worth exactly one line per session; without these latches the
// same handful of lines is what fills the log a user is asked to send us.
bool g_warned_class_slot = false;
bool g_warned_no_sections = false;
bool g_warned_no_engine   = false;

template <typename... Args>
void LogOnce(bool& latch, const char* fmt, Args&&... args) {
    if (latch) return;
    latch = true;
    UEHT_LOG(Warn, fmt, std::forward<Args>(args)...);
}

struct SectionRange {
    uintptr_t base = 0;
    size_t    size = 0;
    bool valid() const { return base != 0 && size != 0; }
};

std::vector<SectionRange> FindWritableSections(HMODULE mod) {
    std::vector<SectionRange> out;
    if (!mod) return out;
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;
    auto nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) {
            out.push_back(SectionRange{
                reinterpret_cast<uintptr_t>(base + sec[i].VirtualAddress),
                static_cast<size_t>(sec[i].Misc.VirtualSize),
            });
        }
    }
    return out;
}

bool LooksLikePointer(uintptr_t base, size_t size, uintptr_t p) {
    if (p < 0x10000) return false;
    if ((p & 7) != 0) return false;
    return p < base || p >= base + size;
}

}  // namespace

uintptr_t LocateGEngine(uintptr_t uengine_class_rva) {
    if (auto cached = g_cached_gengine.load(std::memory_order_acquire); cached != 0) {
        return cached;
    }
    if (g_resolve_attempted.exchange(true, std::memory_order_acq_rel)) {
        return 0;
    }

    HMODULE host = GetModuleHandleW(nullptr);
    if (!host) return 0;
    const auto module_base = reinterpret_cast<uintptr_t>(host);

    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), host, &mi, sizeof(mi))) return 0;
    const size_t module_size = mi.SizeOfImage;

    // Read UEngine UClass pointer from the well-known static slot.
    uintptr_t uengine_class = 0;
    if (!SafeRead(module_base + uengine_class_rva, uengine_class) || uengine_class == 0) {
        LogOnce(g_warned_class_slot,
                "LocateGEngine: UEngine UClass slot at +0x%llX is empty - engine not initialized yet; retrying",
                static_cast<unsigned long long>(uengine_class_rva));
        g_resolve_attempted.store(false, std::memory_order_release);
        return 0;
    }

    auto sections = FindWritableSections(host);
    if (sections.empty()) {
        LogOnce(g_warned_no_sections, "LocateGEngine: no writable sections in host module");
        return 0;
    }

    // UObject layout in UE 4.12 (UObjectBase):
    //   +0x00  vtable
    //   +0x08  ObjectFlags (int32)
    //   +0x0C  InternalIndex (int32)
    //   +0x10  ClassPrivate (UClass*)
    //   +0x18  NamePrivate (FName, 8 bytes)
    //   +0x20  OuterPrivate (UObject*)
    // UStruct (parent of UClass) in UE 4.12 has SuperStruct at +0x30.
    // (UObjectBase 0x28 + UField::Next 0x08 = 0x30. Verified against UEngine UClass
    // header dump where +0x30 points to UObject UClass.)
    constexpr size_t kClassPrivateOffset = 0x10;
    constexpr size_t kSuperStructOffset  = 0x30;
    constexpr int    kMaxChainDepth      = 8;

    // Treat as "UClass-shaped" if it lives in the same heap region as UEngine's UClass.
    // Heaps tend to occupy multi-GB aligned ranges; reject if the high 24 bits differ.
    const uintptr_t kClassHeapMask = ~static_cast<uintptr_t>((1ULL << 40) - 1);
    const uintptr_t uengine_class_region = uengine_class & kClassHeapMask;

    auto IsClassLike = [&](uintptr_t v) {
        if (v < 0x10000 || (v & 7) != 0) return false;
        return (v & kClassHeapMask) == uengine_class_region;
    };

    const auto step = sizeof(uintptr_t);
    size_t candidate_count = 0;
    size_t class_like_total = 0;

    for (const auto& sect : sections) {
        for (uintptr_t p = sect.base; p + step <= sect.base + sect.size; p += step) {
            // The host module's own writable sections are committed for the life
            // of the process, so the slot read cannot fault - read it directly
            // (same guarantee cameraunlock-core's pattern_scanner relies on). The
            // SEH guard is only needed for the candidate dereference below, which
            // chases an arbitrary heap pointer that may be stale.
            const uintptr_t candidate = *reinterpret_cast<const uintptr_t*>(p);
            if (!LooksLikePointer(module_base, module_size, candidate)) continue;
            ++candidate_count;

            uintptr_t cls = 0;
            if (!SafeRead(candidate + kClassPrivateOffset, cls)) continue;
            if (!IsClassLike(cls)) continue;
            ++class_like_total;

            // Walk SuperStruct chain looking for UEngine.
            uintptr_t walk = cls;
            for (int depth = 0; depth < kMaxChainDepth; ++depth) {
                if (walk == uengine_class) {
                    UEHT_LOG(Info, "LocateGEngine: GEngine @ 0x%llX",
                             static_cast<unsigned long long>(candidate));
                    g_cached_gengine.store(candidate, std::memory_order_release);
                    return candidate;
                }
                uintptr_t super = 0;
                if (!SafeRead(walk + kSuperStructOffset, super)) break;
                if (!IsClassLike(super) || super == walk) break;
                walk = super;
            }
        }
    }

    LogOnce(g_warned_no_engine,
            "LocateGEngine: scanned %llu pointer candidates, %llu UClass-shaped, no UEngine in chain; retrying",
            static_cast<unsigned long long>(candidate_count),
            static_cast<unsigned long long>(class_like_total));
    g_resolve_attempted.store(false, std::memory_order_release);
    return 0;
}

}  // namespace ueht::ue
