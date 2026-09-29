#pragma once

#include <cstdint>

namespace ueht::ue {

/// Finds the GEngine global in the host process: the UObject whose class chain
/// reaches the UEngine UClass held at `uengine_class_rva`. Returns 0 until the
/// engine has constructed it; the result is cached once found.
uintptr_t LocateGEngine(uintptr_t uengine_class_rva);

}  // namespace ueht::ue
