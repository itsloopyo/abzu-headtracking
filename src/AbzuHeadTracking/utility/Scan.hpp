#pragma once

// Clean-room reverse-engineering primitives for locating Unreal Engine
// structures inside a shipping game binary.
//
// None of this code is derived from UEVR or any other source — these are
// textbook techniques (string-ref scanning, RIP-relative displacement
// resolution, function-prologue walking) reimplemented from first
// principles against the public x86-64 SysV/MS ABIs and Epic's public
// Unreal Engine source.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace ueht::scan {

struct ModuleRange {
    uintptr_t base = 0;
    size_t    size = 0;
    HMODULE   handle = nullptr;
    bool valid() const { return base != 0 && size != 0; }
};

/// Returns the loaded image range for `module_name` (e.g. L"Engine.dll" or
/// the host exe name). If `module_name` is empty, returns the main exe.
ModuleRange GetModule(std::wstring_view module_name);

/// Returns the range of every loaded module whose name contains `substr`
/// (case-insensitive). Useful for "find the UE-* DLL" without knowing the
/// exact name, since shipping builds rename these.
std::vector<ModuleRange> FindModulesContaining(std::wstring_view substr);

/// Resolve a RIP-relative 32-bit displacement at `displacement_at` into the
/// absolute address it refers to. `next_instruction` is the address of the
/// byte immediately following the 4-byte displacement.
inline uintptr_t ResolveRip32(uintptr_t displacement_at, uintptr_t next_instruction) {
    return next_instruction + *reinterpret_cast<const int32_t*>(displacement_at);
}

/// Find every occurrence of a UTF-16 string literal inside the read-only
/// data section of a module. Returns absolute addresses of the first byte
/// of each match. Empty result means the literal isn't present.
std::vector<uintptr_t> FindWideStringLiterals(const ModuleRange& mod, std::wstring_view literal);

/// Find every occurrence of an ASCII string literal.
std::vector<uintptr_t> FindAsciiStringLiterals(const ModuleRange& mod, std::string_view literal);

/// Find every instruction inside `mod`'s code that loads `string_address`
/// via a RIP-relative `lea r/m64, [rip+disp32]` (encoded 48 8D ?? ?? ?? ?? ??).
/// Returned addresses point at the `lea` opcode itself.
std::vector<uintptr_t> FindRipReferencesTo(const ModuleRange& mod, uintptr_t string_address);

/// Walk backward from `address` to the start of the enclosing function.
/// Heuristic: look for a standard x64 prologue (`push rbp` / sub rsp,imm /
/// mov [rsp+x],rcx etc.) preceded by INT3 padding or a `ret` from the prior
/// function. Returns 0 on failure. Range is capped at 4 KiB by default.
uintptr_t WalkToFunctionStart(uintptr_t address, size_t max_back = 0x1000);

/// Convenience: find a function in `mod` whose body references the given
/// UTF-16 string literal. Returns 0 if no candidate is found. If multiple
/// references exist, returns the first one whose enclosing function we can
/// resolve.
uintptr_t FindFunctionByWideStringRef(const ModuleRange& mod, std::wstring_view literal);

/// Same, but for ASCII strings.
uintptr_t FindFunctionByAsciiStringRef(const ModuleRange& mod, std::string_view literal);

}  // namespace ueht::scan
