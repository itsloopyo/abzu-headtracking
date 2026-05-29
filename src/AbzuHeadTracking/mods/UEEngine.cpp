#include "UEEngine.hpp"

#include "utility/Logging.hpp"
#include "utility/SafeMemory.hpp"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace ueht::ue {

std::string EngineVersion::ToString() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", major, minor, patch);
    return buf;
}

EngineVersion DetectEngineVersion() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    const wchar_t* leaf = wcsrchr(path, L'\\');
    leaf = leaf ? leaf + 1 : path;
    if (_wcsicmp(leaf, L"AbzuGame-Win64-Shipping.exe") == 0) {
        EngineVersion v;
        v.major = 4;
        v.minor = 12;
        v.patch = 0;
        return v;
    }

    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (size == 0) return {};

    std::unique_ptr<uint8_t[]> buf(new uint8_t[size]);
    if (!GetFileVersionInfoW(path, handle, size, buf.get())) return {};

    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.get(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) || !ffi) return {};

    EngineVersion v;
    v.major = HIWORD(ffi->dwProductVersionMS);
    v.minor = LOWORD(ffi->dwProductVersionMS);
    v.patch = HIWORD(ffi->dwProductVersionLS);
    if (v.major < 4 || v.major > 5) return {};
    return v;
}

namespace {

// UE 4.12 (ABZU) - offsets confirmed via Ghidra against
// AbzuGame-Win64-Shipping.exe property registration immediates.
// See .lab/NOTES.md for the decompilation evidence.
constexpr EngineOffsets kUE4_12_Abzu = {
    /*engine_to_game_instance              */ 0x5E8, // UEngine::GameViewport (UGameViewportClient*)
    /*game_instance_to_local_players       */ 0x38,  // UGameInstance::LocalPlayers (TArray)
    /*local_player_to_player_controller    */ 0x30,  // UPlayer::PlayerController (ULocalPlayer inherits UPlayer)
    /*controller_to_control_rotation       */ 0x3B0, // AController::ControlRotation (FRotator) - source UE reads each tick
    /*player_controller_to_camera_manager  */ 0x418, // APlayerController::PlayerCameraManager (confirmed in-game, see NOTES.md)
};

}  // namespace

std::optional<EngineOffsets> OffsetsFor(EngineVersion v) {
    if (!v.valid()) return std::nullopt;
    if (v.major == 4 && v.minor == 12) return kUE4_12_Abzu;
    return std::nullopt;
}

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
// For ABZU specifically, the UEngine UClass pointer is the DAT_142adde30
// global - that's the `Z_Registration_Info_UClass_UEngine.OuterSingleton`
// in UE source. RVA = 0x02adde30 from imageBase.

namespace {

constexpr uintptr_t kAbzuUEngineClassRVA = 0x02adde30;

std::atomic<uintptr_t> g_cached_gengine{0};
std::atomic<bool>      g_resolve_attempted{false};

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
            char nm[9] = {};
            std::memcpy(nm, sec[i].Name, 8);
            UEHT_LOG(Info, "LocateGEngine: scanning writable section '%s' at 0x%llX size 0x%llX",
                     nm,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(base + sec[i].VirtualAddress)),
                     static_cast<unsigned long long>(sec[i].Misc.VirtualSize));
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

uintptr_t LocateGEngine() {
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
    if (!SafeRead(module_base + kAbzuUEngineClassRVA, uengine_class) || uengine_class == 0) {
        UEHT_LOG(Warn, "LocateGEngine: UEngine UClass slot at +0x%llX is empty - engine not initialized yet",
                 static_cast<unsigned long long>(kAbzuUEngineClassRVA));
        g_resolve_attempted.store(false, std::memory_order_release);
        return 0;
    }

    auto sections = FindWritableSections(host);
    if (sections.empty()) {
        UEHT_LOG(Warn, "LocateGEngine: no writable sections in host module");
        return 0;
    }
    UEHT_LOG(Info, "LocateGEngine: UEngine UClass = 0x%llX",
             static_cast<unsigned long long>(uengine_class));

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

    // Dump UEngine UClass header so we can identify SuperStruct offset empirically.
    // Whichever QWORD points back into the same heap region as uengine_class is the
    // SuperStruct pointer (UEngine's parent UClass).
    for (size_t off = 0x10; off <= 0x80; off += 8) {
        uintptr_t v = 0;
        if (SafeRead(uengine_class + off, v)) {
            UEHT_LOG(Info, "LocateGEngine: UEngineUClass[+0x%02llX] = 0x%llX%s",
                     static_cast<unsigned long long>(off),
                     static_cast<unsigned long long>(v),
                     IsClassLike(v) ? "  <- looks like UClass*" : "");
        }
    }

    const auto step = sizeof(uintptr_t);
    size_t candidate_count = 0;
    size_t class_like_total = 0;
    size_t class_like_logged = 0;

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

            if (class_like_logged < 8) {
                UEHT_LOG(Info, "LocateGEngine: candidate slot=0x%llX obj=0x%llX cls=0x%llX",
                         static_cast<unsigned long long>(p),
                         static_cast<unsigned long long>(candidate),
                         static_cast<unsigned long long>(cls));
                ++class_like_logged;
            }

            // Walk SuperStruct chain looking for UEngine.
            uintptr_t walk = cls;
            for (int depth = 0; depth < kMaxChainDepth; ++depth) {
                if (walk == uengine_class) {
                    UEHT_LOG(Info, "LocateGEngine: found UEngine-derived instance at 0x%llX via slot 0x%llX (cls=0x%llX, depth=%d)",
                             static_cast<unsigned long long>(candidate),
                             static_cast<unsigned long long>(p),
                             static_cast<unsigned long long>(cls),
                             depth);
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

    UEHT_LOG(Warn, "LocateGEngine: scanned %llu pointer candidates, %llu UClass-shaped (logged %llu), no UEngine in chain (SuperStructOffset=0x%llX)",
             static_cast<unsigned long long>(candidate_count),
             static_cast<unsigned long long>(class_like_total),
             static_cast<unsigned long long>(class_like_logged),
             static_cast<unsigned long long>(kSuperStructOffset));
    g_resolve_attempted.store(false, std::memory_order_release);
    return 0;
}

}  // namespace ueht::ue
