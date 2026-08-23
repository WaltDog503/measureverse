#include <iostream>
#include <vector>
#include <string>
#include <sstream>

#include "crow_all.h"

using namespace std;

struct CalibrationPoint {
    double sensor_val;
    double offset_correction;
};

struct InventoryItem {
    double min_dimension;
    double max_dimension;
    string sku_id;
    string category_name;
    int stock_quantity;
};

// O(log n) Dimension Calibration
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
                    high = table.size();
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

// O(log n) SKU Bin Matching
InventoryItem searchSKUBin(const vector<InventoryItem>& catalog, double dimension) {
    InventoryItem match = {0.0, 0.0, "NOT_FOUND", "None", 0};
    
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
                    high = catalog.size();
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

int main() {
    crow::SimpleApp app;

    const vector<CalibrationPoint> calibration_table = {
        {10.0, 0.50},
        {20.0, 0.85},
        {30.0, 1.20},
        {40.0, 1.65},
        {50.0, 2.10}
    };

    const vector<InventoryItem> live_catalog = {
        {12.0, 18.0, "SMH-SKU-SMALL", "Topwear", 50},
        {18.1, 24.0, "SMH-SKU-MED",   "Topwear", 75},
        {24.1, 32.0, "SMH-SKU-LARGE", "Topwear", 60},
        {32.1, 42.0, "SMH-SKU-XLARGE", "Topwear", 25}
    };

    CROW_ROUTE(app, "/api/v1/measure").methods(crow::HTTPMethod::POST)(
        [&calibration_table, &live_catalog](const crow::request& req) {
            auto body = crow::json::load(req.body);
            crow::json::wvalue response;
            
            if (!body || !body.has("raw_reading") || !body.has("scale_factor")) {
                response["status"] = "error";
                response["message"] = "Invalid payload. Required: raw_reading, scale_factor";
                return crow::response(400, response);
            }
            
            double raw_reading = body["raw_reading"].d();
            double scale_factor = body["scale_factor"].d();
            
            double target_dim = calculateDimension(calibration_table, raw_reading, scale_factor);
            InventoryItem matched_item = searchSKUBin(live_catalog, target_dim);
            
            response["target_dimension"] = target_dim;
            if (matched_item.sku_id != "NOT_FOUND" && matched_item.stock_quantity > 0) {
                response["status"] = "success";
                response["matched_sku"] = matched_item.sku_id;
                response["category"] = matched_item.category_name;
                response["stock_available"] = matched_item.stock_quantity;
            } else {
                response["status"] = "out_of_stock";
                response["matched_sku"] = "UNAVAILABLE";
            }
            
            return crow::response(200, response);
        }
    );

    app.port(8080).multithreaded().run();
    return 0;
}
