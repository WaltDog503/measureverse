#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace std;

// ============================================================================
// COMMENT LEARNING RENDITIONS & STRUCTURE DEFINITIONS
// ============================================================================

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
                    high = table.size(); // Terminate cleanly without break
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
                    high = catalog.size(); // Terminate cleanly without break
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

// Helper to extract double value from JSON substring
double extractJsonDouble(const string& body, const string& key) {
    double value = 0.0;
    size_t key_pos = body.find("\"" + key + "\"");
    if (key_pos != string::npos) {
        size_t colon_pos = body.find(":", key_pos);
        if (colon_pos != string::npos) {
            size_t start_pos = body.find_first_not_of(" \t\n\r", colon_pos + 1);
            size_t end_pos = body.find_first_of(",}\n\r", start_pos);
            if (start_pos != string::npos) {
                string val_str = body.substr(start_pos, end_pos - start_pos);
                stringstream ss(val_str);
                ss >> value;
            }
        }
    }
    return value;
}

// ============================================================================
// HTTP REST DAEMON
// ============================================================================

int main() {
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

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        cerr << "Failed to create socket." << endl;
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(8080);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        cerr << "Bind failed on port 8080." << endl;
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        cerr << "Listen failed." << endl;
        close(server_fd);
        return 1;
    }

    cout << "=====================================================" << endl;
    cout << "  SHAKINGMYHEAD (S.M.H.) MEASUREVERSE SERVICE LIVE   " << endl;
    cout << "  Listening on http://localhost:8080/api/v1/measure  " << endl;
    cout << "=====================================================" << endl;

    while (true) {
        int client_socket = accept(server_fd, nullptr, nullptr);
        if (client_socket < 0) {
            continue;
        }

        char buffer[4096] = {0};
        ssize_t bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            string request_str(buffer);
            size_t body_pos = request_str.find("\r\n\r\n");
            string body = (body_pos != string::npos) ? request_str.substr(body_pos + 4) : "";

            double raw_reading = extractJsonDouble(body, "raw_reading");
            double scale_factor = extractJsonDouble(body, "scale_factor");

            if (scale_factor == 0.0) {
                scale_factor = 1.0;
            }

            double target_dim = calculateDimension(calibration_table, raw_reading, scale_factor);
            InventoryItem match = searchSKUBin(live_catalog, target_dim);

            stringstream json_response;
            json_response << "{\n";
            json_response << "  \"target_dimension\": " << target_dim << ",\n";
            if (match.sku_id != "NOT_FOUND" && match.stock_quantity > 0) {
                json_response << "  \"status\": \"success\",\n";
                json_response << "  \"matched_sku\": \"" << match.sku_id << "\",\n";
                json_response << "  \"category\": \"" << match.category_name << "\",\n";
                json_response << "  \"stock_available\": " << match.stock_quantity << "\n";
            } else {
                json_response << "  \"status\": \"out_of_stock\",\n";
                json_response << "  \"matched_sku\": \"UNAVAILABLE\"\n";
            }
            json_response << "}";

            string json_payload = json_response.str();
            stringstream http_response;
            http_response << "HTTP/1.1 200 OK\r\n";
            http_response << "Content-Type: application/json\r\n";
            http_response << "Content-Length: " << json_payload.size() << "\r\n";
            http_response << "Connection: close\r\n\r\n";
            http_response << json_payload;

            string full_response = http_response.str();
            write(client_socket, full_response.c_str(), full_response.size());
        }

        close(client_socket);
    }

    close(server_fd);
    return 0;
}
