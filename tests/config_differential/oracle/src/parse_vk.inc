/// Map a friendly key name from ueht.ini to a Win32 VK_ code.
/// Handles "F1".."F24", single A-Z/0-9, and a few common names.
int ParseVk(const std::string& name) {
    using namespace cameraunlock::input;
    if (name.empty()) return 0;

    std::string n;
    n.reserve(name.size());
    for (char c : name) n.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

    // Numeric VK literal, e.g. "0x22" or "34" (base 0 honours the 0x prefix).
    if (std::isdigit(static_cast<unsigned char>(n[0]))) {
        int v = static_cast<int>(std::strtol(n.c_str(), nullptr, 0));
        if (v > 0 && v <= 0xFF) return v;
    }
    if (n.size() >= 2 && n[0] == 'F' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        int idx = std::atoi(n.c_str() + 1);
        if (idx >= 1 && idx <= 24) return 0x6F + idx;  // VK_F1 = 0x70
    }
    if (n.size() == 1) {
        char c = n[0];
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
    }
    if (n == "HOME")     return VK::Home;
    if (n == "END")      return VK::End;
    if (n == "INSERT")   return VK::Insert;
    if (n == "DELETE")   return VK::Delete;
    if (n == "SPACE")    return VK::Space;
    if (n == "PAGEUP")   return 0x21;  // VK_PRIOR
    if (n == "PAGEDOWN") return 0x22;  // VK_NEXT
    if (n == "ESCAPE" || n == "ESC") return VK::Escape;
    return 0;
}
