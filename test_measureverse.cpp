#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cassert>

using namespace std;

// ============================================================================
// COMMENT LEARNING STYLE RENDITIONS & DATA MODELS
// ============================================================================

struct CalibrationPoint {
    double sensor_val;
    double offset_correction;
};

struct InventoryItem {
    int id;
    double min_dimension;
    double max_dimension;
    string sku_id;
    string category_name;
    int stock_quantity;
};

// ============================================================================
// O(log n) SEARCH IMPLEMENTATION UNDER TEST
// ============================================================================

double calculateDimension(const vector<CalibrationPoint>& table, double raw_input, double scale) {
    double calculated_val = 0.0;
    
    if (!table.empty()) {
        size_t low = 0;
        size_t high = table.size() - 1;
        size_t best_idx = 0;
        
        while (low <= high && high < table.size()) {
            size_t mid = low + (high - low) / 2;
            if (table[mid].sensor_val <= raw_input) {
                best_idx = mid;
                low = mid + 1;
            } else {
                if (mid == 0) {
                    high = table.size(); // Terminate without break
                } else {
                    high = mid - 1;
                }
            }
        }
        calculated_val = (raw_input * scale) + table[best_idx].offset_correction;
    } else {
        calculated_val = raw_input * scale;
    }
    
    return calculated_val;
}

InventoryItem searchSKUBin(const vector<InventoryItem>& catalog, double dimension) {
    InventoryItem match = {0, 0.0, 0.0, "NOT_FOUND", "None", 0};
    
    if (!catalog.empty()) {
        size_t low = 0;
        size_t high = catalog.size() - 1;
        bool found = false;
        
        while (low <= high && !found && high < catalog.size()) {
            size_t mid = low + (high - low) / 2;
            if (dimension >= catalog[mid].min_dimension && dimension <= catalog[mid].max_dimension) {
                match = catalog[mid];
                found = true;
            } else if (dimension < catalog[mid].min_dimension) {
                if (mid == 0) {
                    high = catalog.size(); // Terminate without break
                } else {
                    high = mid - 1;
                }
            } else {
                low = mid + 1;
            }
        }
    }
    
    return match;
}

// ============================================================================
// TEST HARNESS & BENCHMARK
// ============================================================================

void runBoundaryTests() {
    cout << "[TEST SUITE 1] Running Boundary & Edge Value Checks..." << endl;

    const vector<CalibrationPoint> calibration_table = {
        {10.0, 0.50},
        {20.0, 0.85},
        {30.0, 1.20},
        {40.0, 1.65},
        {50.0, 2.10}
    };

    const vector<InventoryItem> live_catalog = {
        {1, 12.0, 18.0, "SMH-SKU-SMALL", "Topwear", 50},
        {2, 18.1, 24.0, "SMH-SKU-MED",   "Topwear", 75},
        {3, 24.1, 32.0, "SMH-SKU-LARGE", "Topwear", 60},
        {4, 32.1, 42.0, "SMH-SKU-XLARGE", "Topwear", 25}
    };

    // Edge Case 1: Exact minimum boundary match
    InventoryItem match_min = searchSKUBin(live_catalog, 12.0);
    assert(match_min.sku_id == "SMH-SKU-SMALL");

    // Edge Case 2: Exact maximum boundary match
    InventoryItem match_max = searchSKUBin(live_catalog, 18.0);
    assert(match_max.sku_id == "SMH-SKU-SMALL");

    // Edge Case 3: Gap value between small and medium (18.05)
    InventoryItem match_gap = searchSKUBin(live_catalog, 18.05);
    assert(match_gap.sku_id == "NOT_FOUND");

    // Edge Case 4: Below minimum sizing range
    InventoryItem match_under = searchSKUBin(live_catalog, 5.0);
    assert(match_under.sku_id == "NOT_FOUND");

    // Edge Case 5: Above maximum sizing range
    InventoryItem match_over = searchSKUBin(live_catalog, 55.0);
    assert(match_over.sku_id == "NOT_FOUND");

    cout << "  --> All 5 boundary tests passed successfully.\n" << endl;
}

void runScalingBenchmark() {
    cout << "[TEST SUITE 2] Generating 10,000 SKU Dataset for O(log n) Stress Test..." << endl;

    vector<InventoryItem> massive_catalog;
    massive_catalog.reserve(10000);

    double current_dim = 1.0;
    for (int i = 0; i < 10000; ++i) {
        InventoryItem item;
        item.id = i + 1;
        item.min_dimension = current_dim;
        item.max_dimension = current_dim + 0.99;
        item.sku_id = "SMH-SYNTH-" + to_string(i + 1);
        item.category_name = "BulkTest";
        item.stock_quantity = 100;
        massive_catalog.push_back(item);
        current_dim += 1.0;
    }

    cout << "  --> Catalog generated. Executing 100,000 random lookup queries..." << endl;

    auto start_time = chrono::high_resolution_clock::now();

    for (int q = 0; q < 100000; ++q) {
        double query_dim = (q % 9500) + 1.25;
        InventoryItem match = searchSKUBin(massive_catalog, query_dim);
        assert(match.sku_id != "NOT_FOUND");
    }

    auto end_time = chrono::high_resolution_clock::now();
    chrono::duration<double, milli> elapsed = end_time - start_time;

    cout << "  --> Completed 100,000 lookups in " << elapsed.count() << " ms." << endl;
    cout << "  --> Average latency per query: " << (elapsed.count() / 100000.0) * 1000.0 << " microseconds." << endl;
}

int main() {
    runBoundaryTests();
    runScalingBenchmark();
    cout << "\n=====================================================" << endl;
    cout << "  ALL VERIFICATION CHECKS & BENCHMARKS PASSED        " << endl;
    cout << "=====================================================" << endl;
    return 0;
}
