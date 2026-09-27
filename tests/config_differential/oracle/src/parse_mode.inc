UnrealCamera::Mode ParseMode(const std::string& s) {
    if (s == "updatecamera" || s == "UpdateCamera") return UnrealCamera::Mode::UpdateCamera;
    return UnrealCamera::Mode::ControlRotation;
}
