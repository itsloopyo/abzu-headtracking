#include "Scan.hpp"

#include "Logging.hpp"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <cstring>

namespace ueht::scan {

namespace {

bool IcaseContains(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (std::towlower(hay[i + j]) != std::towlower(needle[j])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

ModuleRange RangeFromHandle(HMODULE h) {
    if (!h) return {};
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) return {};
    ModuleRange r;
    r.handle = h;
    r.base   = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    r.size   = mi.SizeOfImage;
    return r;
}

/// Returns the bounds of a module's `.text` (executable code) section, falling
/// back to the whole module on failure.
std::pair<uintptr_t, size_t> CodeSectionOf(const ModuleRange& m) {
    if (!m.valid()) return {0, 0};
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {m.base, m.size};
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {m.base, m.size};
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
            std::memcmp(sec[i].Name, ".text", 5) == 0) {
            return {m.base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize};
        }
    }
    return {m.base, m.size};
}

/// Returns ranges of all readable, non-executable data sections (.rdata, .data).
std::vector<std::pair<uintptr_t, size_t>> DataSectionsOf(const ModuleRange& m) {
    std::vector<std::pair<uintptr_t, size_t>> out;
    if (!m.valid()) return out;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m.base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { out.push_back({m.base, m.size}); return out; }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { out.push_back({m.base, m.size}); return out; }
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_READ) &&
           !(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
            out.push_back({m.base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize});
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Module enumeration
// ---------------------------------------------------------------------------

ModuleRange GetModule(std::wstring_view module_name) {
    HMODULE h;
    if (module_name.empty()) {
        h = GetModuleHandleW(nullptr);
    } else {
        std::wstring nul(module_name);  // null-terminate
        h = GetModuleHandleW(nul.c_str());
    }
    return RangeFromHandle(h);
}

std::vector<ModuleRange> FindModulesContaining(std::wstring_view substr) {
    std::vector<ModuleRange> out;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return out;
    const size_t count = needed / sizeof(HMODULE);
    wchar_t name[MAX_PATH];
    for (size_t i = 0; i < count; ++i) {
        if (GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH) == 0) continue;
        if (IcaseContains(name, substr)) {
            auto r = RangeFromHandle(mods[i]);
            if (r.valid()) out.push_back(r);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// String-literal scanning
// ---------------------------------------------------------------------------

template <typename Char>
static std::vector<uintptr_t> FindLiteralImpl(const ModuleRange& mod,
                                              const Char* needle, size_t n_chars) {
    std::vector<uintptr_t> hits;
    if (!mod.valid() || n_chars == 0) return hits;
    const size_t n_bytes = n_chars * sizeof(Char);

    for (auto [start, size] : DataSectionsOf(mod)) {
        if (size < n_bytes) continue;
        const auto* p   = reinterpret_cast<const uint8_t*>(start);
        const auto* end = p + size - n_bytes;
        // Use first char as a fast prefilter.
        const auto first = *reinterpret_cast<const uint8_t*>(needle);
        for (const uint8_t* cur = p; cur <= end; ++cur) {
            if (*cur != first) continue;
            if (std::memcmp(cur, needle, n_bytes) == 0) {
                // Require a null terminator just past the literal so we don't
                // match substrings (e.g. "Hello" inside "HelloWorld").
                if (cur + n_bytes + sizeof(Char) <= reinterpret_cast<const uint8_t*>(start + size)) {
                    Char term;
                    std::memcpy(&term, cur + n_bytes, sizeof(Char));
                    if (term == Char{}) hits.push_back(reinterpret_cast<uintptr_t>(cur));
                }
            }
        }
    }
    return hits;
}

std::vector<uintptr_t> FindWideStringLiterals(const ModuleRange& mod, std::wstring_view literal) {
    return FindLiteralImpl(mod, literal.data(), literal.size());
}

std::vector<uintptr_t> FindAsciiStringLiterals(const ModuleRange& mod, std::string_view literal) {
    return FindLiteralImpl(mod, literal.data(), literal.size());
}

// ---------------------------------------------------------------------------
// RIP-relative reference scan
// ---------------------------------------------------------------------------
//
// We look for any 7-byte LEA encoding `48 8D ?? disp32` whose displacement
// resolves to `string_address`. We don't restrict the ModR/M byte: shipping
// builds compile this LEA against rax/rcx/rdx/r8/r9 etc. depending on the
// calling convention slot the literal is filling. The ModR/M's mod field
// must be 00 and r/m must be 101 (RIP-relative), which corresponds to
// bytes 0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D.

std::vector<uintptr_t> FindRipReferencesTo(const ModuleRange& mod, uintptr_t string_address) {
    std::vector<uintptr_t> hits;
    auto [text, text_size] = CodeSectionOf(mod);
    if (text == 0 || text_size < 7) return hits;

    const uint8_t* p   = reinterpret_cast<const uint8_t*>(text);
    const uint8_t* end = p + text_size - 7;

    auto is_rip_modrm = [](uint8_t b) {
        // mod == 00, r/m == 101, any reg field (0..7)
        return (b & 0xC7) == 0x05;
    };

    for (const uint8_t* cur = p; cur <= end; ++cur) {
        if (cur[0] != 0x48 || cur[1] != 0x8D) continue;     // REX.W + LEA
        if (!is_rip_modrm(cur[2])) continue;
        const auto disp_at         = reinterpret_cast<uintptr_t>(cur + 3);
        const auto next_instruction = reinterpret_cast<uintptr_t>(cur + 7);
        if (ResolveRip32(disp_at, next_instruction) == string_address) {
            hits.push_back(reinterpret_cast<uintptr_t>(cur));
        }
    }
    return hits;
}

// ---------------------------------------------------------------------------
// Function-start walker
// ---------------------------------------------------------------------------
//
// We walk backward looking for the most common x64 function-prologue marker:
// the byte immediately before a function is either a `ret` (0xC3 / 0xC2 imm16)
// or `INT3` padding (0xCC). Once we land on such a boundary, the *next* byte
// is the first instruction of the enclosing function.

uintptr_t WalkToFunctionStart(uintptr_t address, size_t max_back) {
    if (address == 0) return 0;
    const auto* p = reinterpret_cast<const uint8_t*>(address);
    for (size_t i = 0; i < max_back; ++i) {
        const uint8_t b = p[-static_cast<ptrdiff_t>(i)];
        if (b == 0xCC) {
            // INT3 alignment padding — function starts right after.
            return reinterpret_cast<uintptr_t>(&p[-static_cast<ptrdiff_t>(i) + 1]);
        }
        if (b == 0xC3) {
            // Possible `ret`. Heuristic: previous byte should not be part of a
            // larger instruction. Accept and treat next byte as function start.
            return reinterpret_cast<uintptr_t>(&p[-static_cast<ptrdiff_t>(i) + 1]);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// High-level helpers
// ---------------------------------------------------------------------------

template <typename Char>
static uintptr_t FindFuncByStringRefImpl(const ModuleRange& mod, const Char* literal, size_t n) {
    auto strings = FindLiteralImpl(mod, literal, n);
    for (uintptr_t s : strings) {
        auto refs = FindRipReferencesTo(mod, s);
        for (uintptr_t r : refs) {
            if (auto fn = WalkToFunctionStart(r); fn != 0) return fn;
        }
    }
    return 0;
}

uintptr_t FindFunctionByWideStringRef(const ModuleRange& mod, std::wstring_view literal) {
    return FindFuncByStringRefImpl(mod, literal.data(), literal.size());
}

uintptr_t FindFunctionByAsciiStringRef(const ModuleRange& mod, std::string_view literal) {
    return FindFuncByStringRefImpl(mod, literal.data(), literal.size());
}

}  // namespace ueht::scan
