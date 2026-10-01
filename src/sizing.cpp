#include "sizing.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace mv {

double calibrateAxis(const std::vector<CalibrationPoint>& table, double raw, double scale) {
    if (table.empty()) {
        return raw * scale;
    }
    // First point with sensor_val > raw; the one before it is the floor.
    auto it = std::upper_bound(table.begin(), table.end(), raw,
        [](double v, const CalibrationPoint& p) { return v < p.sensor_val; });
    const CalibrationPoint& floor_pt = (it == table.begin()) ? table.front() : *std::prev(it);
    return raw * scale + floor_pt.offset_correction;
}

std::string matchBracket(const std::vector<SizingBracket>& brackets, double dimension) {
    if (brackets.empty() || std::isnan(dimension) ||
        dimension < brackets.front().min_val || dimension > brackets.back().max_val) {
        return kBespoke;
    }
    auto it = std::upper_bound(brackets.begin(), brackets.end(), dimension,
        [](double v, const SizingBracket& b) { return v < b.min_val; });
    return std::prev(it)->code;
}

std::string classifySilhouette(double bust, double waist, double hip) {
    const double bust_waist = bust - waist;
    const double hip_waist = hip - waist;

    if (bust_waist >= 8.5 && hip_waist >= 9.5) return "Hourglass Luxe";
    if (hip_waist >= 10.0 && bust_waist < 7.0) return "Sculpted Mermaid";
    if (bust_waist >= 9.0 && hip_waist < 7.0) return "Empire Architectural";
    if (std::abs(bust - hip) <= 2.5 && waist >= bust - 5.0) return "Minimalist Column";
    return "Classic Atelier";
}

}  // namespace mv
