// Tests the PRODUCTION sizing + JSON code (src/), not a copy of it.
// Uses a tiny self-contained harness so it runs anywhere, including with NDEBUG.

#include "../src/json_util.hpp"
#include "../src/sizing.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::cerr << "  FAIL " << __FILE__ << ':' << __LINE__ << "  " #cond "\n"; \
        }                                                                        \
    } while (0)

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

const std::vector<mv::SizingBracket> kCatalog = {
    {12.0, 18.0, "SMALL"}, {18.1, 24.0, "MED"}, {24.1, 32.0, "LARGE"}, {32.1, 42.0, "XLARGE"}};

void testBrackets() {
    std::cout << "[1] Bracket matching\n";
    CHECK(mv::matchBracket(kCatalog, 12.0) == "SMALL");   // exact min
    CHECK(mv::matchBracket(kCatalog, 18.0) == "SMALL");   // exact max of a bracket
    CHECK(mv::matchBracket(kCatalog, 18.05) == "SMALL");  // former gap is now covered
    CHECK(mv::matchBracket(kCatalog, 18.1) == "MED");     // next bracket's min
    CHECK(mv::matchBracket(kCatalog, 42.0) == "XLARGE");  // overall max
    CHECK(mv::matchBracket(kCatalog, 11.99) == mv::kBespoke);
    CHECK(mv::matchBracket(kCatalog, 42.01) == mv::kBespoke);
    CHECK(mv::matchBracket(kCatalog, std::nan("")) == mv::kBespoke);
    CHECK(mv::matchBracket({}, 20.0) == mv::kBespoke);
    CHECK(mv::matchBracket({{1.0, 2.0, "ONLY"}}, 1.5) == "ONLY");
}

void testCalibration() {
    std::cout << "[2] Calibration\n";
    const std::vector<mv::CalibrationPoint> t = {{10, 0.5}, {20, 0.85}, {30, 1.2}};
    CHECK(near(mv::calibrateAxis(t, 10.0, 1.0), 10.5));   // exact point
    CHECK(near(mv::calibrateAxis(t, 19.9, 1.0), 20.4));   // floor = 10
    CHECK(near(mv::calibrateAxis(t, 20.0, 2.0), 40.85));  // scale applied
    CHECK(near(mv::calibrateAxis(t, 99.0, 1.0), 100.2));  // above table -> last
    CHECK(near(mv::calibrateAxis(t, 5.0, 1.0), 5.5));     // below table -> first (clamp)
    CHECK(near(mv::calibrateAxis({}, 7.0, 3.0), 21.0));   // empty table
}

void testSilhouette() {
    std::cout << "[3] Silhouette\n";
    CHECK(mv::classifySilhouette(36, 26, 37) == "Hourglass Luxe");
    CHECK(mv::classifySilhouette(33, 28, 40) == "Sculpted Mermaid");
    CHECK(mv::classifySilhouette(40, 30, 34) == "Empire Architectural");
    CHECK(mv::classifySilhouette(36, 33, 37) == "Minimalist Column");
    CHECK(mv::classifySilhouette(36, 29, 36) == "Classic Atelier");
}

void testJson() {
    std::cout << "[4] JSON parsing/escaping\n";
    auto o = mv::json::parseFlatObject(
        R"({"note": "has \"bust\": 99 inside", "bust": 36.5, "name": "A\u00e9\\B", "x": null})");
    CHECK(o.has_value());
    if (o) {
        CHECK(near(mv::json::getNumber(*o, "bust").value_or(-1), 36.5));  // not fooled by value text
        CHECK(mv::json::getString(*o, "name") == "A\xC3\xA9\\B");
        CHECK(!mv::json::getNumber(*o, "missing").has_value());
        CHECK(mv::json::getString(*o, "x").empty());
    }
    auto s = mv::json::parseFlatObject(R"({"w": "28.5"})");
    CHECK(s && near(mv::json::getNumber(*s, "w").value_or(-1), 28.5));

    CHECK(!mv::json::parseFlatObject("").has_value());
    CHECK(!mv::json::parseFlatObject("{\"a\": }").has_value());
    CHECK(!mv::json::parseFlatObject("{\"a\": 1} trailing").has_value());
    CHECK(!mv::json::parseFlatObject("{\"a\": {\"nested\": 1}}").has_value());
    CHECK(mv::json::parseFlatObject("{}").has_value());

    const std::string esc = mv::json::escape("<img src=x onerror=\"alert(1)\">\n");
    CHECK(esc.find('<') == std::string::npos);
    CHECK(esc.find('>') == std::string::npos);
    CHECK(esc == "\\u003cimg src=x onerror=\\\"alert(1)\\\"\\u003e\\n");
}

void benchmark() {
    std::cout << "[5] O(log n) benchmark: 10,000 brackets x 100,000 lookups\n";
    std::vector<mv::SizingBracket> big;
    big.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        big.push_back({1.0 + i, 1.99 + i, "SYN-" + std::to_string(i + 1)});
    }
    size_t hits = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int q = 0; q < 100000; ++q) {
        hits += (mv::matchBracket(big, (q % 9500) + 1.25) != mv::kBespoke);
    }
    std::chrono::duration<double, std::milli> ms = std::chrono::steady_clock::now() - t0;
    CHECK(hits == 100000);
    std::cout << "    " << ms.count() << " ms total, "
              << (ms.count() / 100000.0) * 1000.0 << " us/query\n";
}

}  // namespace

int main() {
    testBrackets();
    testCalibration();
    testSilhouette();
    testJson();
    benchmark();
    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
