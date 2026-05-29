// Differential + benchmark test for the log timestamp/line formatting fast path.
//
// ueht::log built each line's "HH:MM:SS.mmm" stamp with std::ostringstream +
// std::put_time + stream manipulators, then concatenated the final line with a
// chain of std::string operator+ (several heap allocations per line). This test
// pins the optimised snprintf-based formatters as BYTE-IDENTICAL to the old
// iostream-based ones across the full clock range, then benchmarks both so the
// speedup is a measured number, not a claim.
//
// Standalone, standard C++ only. Build:
//   g++ -O2 -std=c++17 timestamp_format_test.cpp -o timestamp_format_test
//
// The OldStamp/OldLine functions below are verbatim copies of the pre-change
// Logging.cpp logic; NewStamp/NewLine mirror the shipped replacement.

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

// ---- OLD (pre-optimisation) -------------------------------------------------

static std::string OldStamp(const std::tm& tm, int ms) {
    std::ostringstream os;
    os << std::put_time(&tm, "%H:%M:%S") << '.'
       << std::setw(3) << std::setfill('0') << ms;
    return os.str();
}

static std::string OldLine(const std::string& stamp, const char* level,
                           const std::string& msg) {
    return "[" + stamp + "][" + level + "] " + msg + "\n";
}

// ---- NEW (shipped) ----------------------------------------------------------

static std::string NewStamp(const std::tm& tm, int ms) {
    char buf[16];
    int n = std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                          tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return std::string(buf, n < 0 ? 0 : static_cast<size_t>(n));
}

static std::string NewLine(const std::string& stamp, const char* level,
                           const std::string& msg) {
    std::string line;
    line.reserve(stamp.size() + msg.size() + 16);
    line += '[';
    line += stamp;
    line += "][";
    line += level;
    line += "] ";
    line += msg;
    line += '\n';
    return line;
}

// ---- harness ----------------------------------------------------------------

static int g_failures = 0;
static void Check(bool cond, const char* name) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++g_failures;
}

int main() {
    std::printf("Timestamp/line format equivalence + benchmark\n");

    const char* levels[] = {"INFO", "WARN", "ERROR"};
    const std::string msgs[] = {
        "Recentered.",
        "LocateGEngine: scanned 5600000 pointer candidates",
        "",
        std::string(900, 'x'),
    };

    // Exhaustive: every second of the day x every millisecond boundary that
    // matters (0, 1, 9, 10, 99, 100, 999) x every level x representative msgs.
    const int msTests[] = {0, 1, 9, 10, 99, 100, 500, 999};
    bool stampOk = true, lineOk = true;
    for (int h = 0; h < 24; ++h) {
        for (int m = 0; m < 60; ++m) {
            for (int s = 0; s < 60; ++s) {
                std::tm tm{};
                tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s;
                for (int ms : msTests) {
                    const std::string a = OldStamp(tm, ms);
                    const std::string b = NewStamp(tm, ms);
                    if (a != b) { stampOk = false; }
                    for (const char* lv : levels) {
                        for (const auto& msg : msgs) {
                            if (OldLine(a, lv, msg) != NewLine(b, lv, msg)) lineOk = false;
                        }
                    }
                }
            }
        }
    }
    Check(stampOk, "stamp byte-identical across all 24h*60m*60s*ms");
    Check(lineOk, "full line byte-identical across levels and messages");

    // Sanity on a known value.
    {
        std::tm tm{}; tm.tm_hour = 9; tm.tm_min = 5; tm.tm_sec = 3;
        Check(NewStamp(tm, 7) == "09:05:03.007", "known stamp 09:05:03.007");
    }

    // ---- benchmark ----
    auto bench = [](const char* tag, auto fn) {
        using clock = std::chrono::steady_clock;
        const int iters = 500000;
        volatile size_t sink = 0;
        auto t0 = clock::now();
        for (int i = 0; i < iters; ++i) sink += fn(i);
        auto t1 = clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
        std::printf("  %-14s %8.1f ns/line  (sink=%zu)\n", tag, ns, (size_t)sink);
        return ns;
    };

    std::tm tm{}; tm.tm_hour = 13; tm.tm_min = 37; tm.tm_sec = 42;
    const std::string msg = "InstallDecoupledHook: hooked UpdateCamera slot 196";

    double oldns = bench("old (iostream)", [&](int i) {
        std::string s = OldStamp(tm, i % 1000);
        return OldLine(s, "INFO", msg).size();
    });
    double newns = bench("new (snprintf)", [&](int i) {
        std::string s = NewStamp(tm, i % 1000);
        return NewLine(s, "INFO", msg).size();
    });
    std::printf("  speedup: %.2fx\n", oldns / newns);

    if (g_failures == 0) {
        std::printf("All timestamp/line format tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", g_failures);
    return 1;
}
