#include "build_profile.hpp"

namespace ueht::builds {

// Steam, x64, TimeDateStamp 2020-11-14.
extern const BuildProfile kSteamProfile_20201114 = {
    "steam-win64-20201114",
    {0x5FB03EF4, 0x02D5E000, 0x02B67AD5},
    {
        0x02ADDE30,  // uengine_class_rva
        0x5E8,       // engine_to_game_viewport
        0x88,        // game_viewport_to_game_instance
        0x38,        // game_instance_to_local_players
        0x30,        // local_player_to_player_controller
        0x418,       // player_controller_to_camera_manager
    },
};

}  // namespace ueht::builds
