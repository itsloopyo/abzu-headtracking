// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace ueht::builds {
struct ImageView { const std::uint8_t* data; std::size_t size; std::uintptr_t base; };
struct Bootstrap { std::uint32_t names, engineClass; };
struct CameraLayout {
    std::uint32_t location, rotation, secondary, viewTarget, viewLocation, viewRotation;
    std::uint32_t target, slot;
};
bool DiscoverBootstrap(ImageView, Bootstrap&, std::string&);
bool DiscoverCameraTarget(ImageView, CameraLayout&, std::string&);
void StartRuntimeDiscovery(ImageView, const Bootstrap&, bool known);
void ResetRuntimeDiscovery();
bool RuntimeDiscoveryActive();
std::uintptr_t ResolveRuntimeCamera(CameraLayout&);
}
