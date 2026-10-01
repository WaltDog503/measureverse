#pragma once
// Measureverse sizing core: calibration + bracket matching.
// Shared by the server and the test suite so tests exercise production code.

#include <string>
#include <vector>

namespace mv {

struct CalibrationPoint {
    double sensor_val;
    double offset_correction;
};

// Brackets must be sorted by min_val. Bracket i covers [min_val_i, min_val_{i+1});
// the last bracket covers [min_val, max_val]. This removes floating-point gaps
// between adjacent ranges (e.g. 33.55 between 33.5 and 33.6).
struct SizingBracket {
    double min_val;
    double max_val;
    std::string code;
};

inline const std::string kBespoke = "BESPOKE";

// Applies scale and the offset of the greatest calibration point <= raw.
// Inputs below the first point use the first point's offset (documented clamp).
// O(log n).
[[nodiscard]] double calibrateAxis(const std::vector<CalibrationPoint>& table,
                                   double raw, double scale);

// Returns the matching bracket code, or kBespoke if outside the covered range.
// O(log n).
[[nodiscard]] std::string matchBracket(const std::vector<SizingBracket>& brackets,
                                       double dimension);

// Classifies a silhouette from calibrated measurements.
[[nodiscard]] std::string classifySilhouette(double bust, double waist, double hip);

}  // namespace mv
