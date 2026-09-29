#include "build_profile.hpp"

#include <windows.h>

#include "utility/Logging.hpp"

namespace ueht::builds {

namespace {

// Newest first: the top entry is the one an unknown build is compared against
// to word the dormancy line.
const BuildProfile* const kKnownProfiles[] = {
    &kSteamProfile_20201114,
};

}  // namespace

const BuildProfile* MatchRunningBuild() {
    using cameraunlock::memory::FingerprintMismatch;
    cameraunlock::memory::PeFingerprint running{};
    if (!cameraunlock::memory::ReadPeFingerprint(GetModuleHandleW(nullptr), running)) {
        UEHT_LOG(Error, "Could not read the game executable's PE header; head tracking stays off.");
        return nullptr;
    }
    for (const BuildProfile* profile : kKnownProfiles) {
        if (running.Matches(profile->fingerprint)) {
            UEHT_LOG(Info, "Game build: %s", profile->name);
            return profile;
        }
    }

    const BuildProfile& primary = *kKnownProfiles[0];
    const char* why = "";
    switch (cameraunlock::memory::ClassifyMismatch(running, primary.fingerprint)) {
        case FingerprintMismatch::Newer:
            why = "The game is newer than any build this mod knows; check for an updated mod.";
            break;
        case FingerprintMismatch::Older:
            why = "The game is older than the builds this mod knows; let the store finish updating.";
            break;
        case FingerprintMismatch::Differs:
            why = "The executable has been modified; this mod does not engage on a modified binary.";
            break;
    }
    UEHT_LOG(Warn,
             "Unknown game build (TimeDateStamp=0x%08X SizeOfImage=0x%08X CheckSum=0x%08X, newest known %s). "
             "%s Head tracking stays off and the game runs unmodified.",
             running.TimeDateStamp, running.SizeOfImage, running.CheckSum, primary.name, why);
    return nullptr;
}

}  // namespace ueht::builds
