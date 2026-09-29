#include "Mods.hpp"

#include "mods/HeadTracking.hpp"
#include "mods/UnrealCamera.hpp"
#include "utility/Logging.hpp"

namespace ueht {

Mods::Mods(const builds::BuildProfile& build) {
    auto tracking = std::make_unique<HeadTracking>();
    auto camera   = std::make_unique<UnrealCamera>(*tracking, build.offsets);

    m_tracking = tracking.get();
    m_camera   = camera.get();

    m_mods.push_back(std::move(tracking));
    m_mods.push_back(std::move(camera));
}

Mods::~Mods() = default;

std::optional<std::string> Mods::Initialize() {
    for (auto& m : m_mods) {
        UEHT_LOG(Info, "Initializing mod: %.*s",
                 static_cast<int>(m->Name().size()), m->Name().data());
        if (auto err = m->OnInitialize(); err.has_value()) {
            return std::string(m->Name()) + ": " + *err;
        }
    }
    return std::nullopt;
}

void Mods::OnFrame() {
    for (auto& m : m_mods) m->OnFrame();
}

}  // namespace ueht
