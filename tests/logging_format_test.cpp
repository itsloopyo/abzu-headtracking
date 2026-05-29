// Regression tests for ueht::log::Format.
//
// Format snprintf's into a fixed 1024-byte stack buffer. snprintf returns the
// length it WOULD have written had the buffer been large enough, so a line
// longer than the buffer used to be turned into std::string(buf, n) with an
// n past the buffer end - an out-of-bounds read. These tests pin the clamp.
//
// Standalone and portable (Format is header-only, standard C++ only). Build:
//   g++ -std=c++17 -I../src/AbzuHeadTracking/utility logging_format_test.cpp
//       -o logging_format_test  (then run ./logging_format_test)
// Add -fsanitize=address where the runtime is available to also catch the
// over-read directly. The non-template Info/Warn/Error in the header are only
// declared, so no link step is required.

#include "Logging.hpp"

#include <cstdio>
#include <string>

namespace {
int g_failures = 0;

void Check(bool cond, const char* name) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++g_failures;
}
}  // namespace

int main() {
    using ueht::log::Format;

    std::printf("Logging::Format tests\n");

    // Ordinary line round-trips intact.
    {
        const std::string s = Format("yaw=%.2f pitch=%d", 12.5, 7);
        Check(s == "yaw=12.50 pitch=7", "short format is exact");
    }

    // Empty / no-arg format.
    {
        const std::string s = Format("plain");
        Check(s == "plain", "no-arg format is exact");
    }

    // A line that fits exactly in the buffer minus the null terminator (1023
    // chars) must come back whole.
    {
        const std::string filler(1023, 'x');
        const std::string s = Format("%s", filler.c_str());
        Check(s.size() == 1023, "1023-char line preserved");
        Check(s == filler, "1023-char content preserved");
    }

    // The regression: a line far longer than the buffer must be clamped to
    // buffer-1 bytes, never reported at its untruncated length (which would
    // over-read the stack buffer). ASan turns the old behaviour into a hard
    // failure; the size assertion catches it everywhere else.
    {
        const std::string huge(5000, 'A');
        const std::string s = Format("%s", huge.c_str());
        Check(s.size() == 1023, "over-long line clamped to buffer-1");
        Check(s == std::string(1023, 'A'), "clamped content is the buffer prefix");
    }

    if (g_failures == 0) {
        std::printf("All Logging::Format tests passed.\n");
        return 0;
    }
    std::printf("%d Logging::Format test(s) failed.\n", g_failures);
    return 1;
}
