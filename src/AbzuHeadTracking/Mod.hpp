#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ueht {

class Framework;

/// Base class for all UEHT mods. Mods are instantiated once and live until the
/// process exits; they are never torn down. Lifecycle:
///
///   1. `OnInitialize()` - once, on the init thread, after Framework is up.
///      Return a non-empty string to abort load (and log the reason).
///   2. `OnFrame()` - every Present from the render thread.
class Mod {
public:
    virtual ~Mod() = default;

    virtual std::string_view Name() const = 0;

    /// @return error message on failure, std::nullopt on success.
    virtual std::optional<std::string> OnInitialize() { return std::nullopt; }
    virtual void OnFrame() {}
};

}  // namespace ueht
