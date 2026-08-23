#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sqlite3.h>

using namespace std;

// ============================================================================
// DATA MODELS
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
// DATABASE LAYER (SQLite3)
// ============================================================================

class DatabaseManager {
private:
    sqlite3* db;

public:
    DatabaseManager(const string& db_name) : db(nullptr) {
        if (sqlite3_open(db_name.c_str(), &db) != SQLITE_OK) {
            cerr << "Cannot open database: " << sqlite3_errmsg(db) << endl;
        }
    }

    ~DatabaseManager() {
        if (db) {
            sqlite3_close(db);
        }
    }

    vector<CalibrationPoint> loadCalibrationPoints() {
        vector<CalibrationPoint> points;
        const char* query = "SELECT sensor_val, offset_correction FROM calibration_points ORDER BY sensor_val ASC;";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                CalibrationPoint cp;
                cp.sensor_val = sqlite3_column_double(stmt, 0);
                cp.offset_correction = sqlite3_column_double(stmt, 1);
                points.push_back(cp);
            }
            sqlite3_finalize(stmt);
        }
        return points;
    }

    vector<InventoryItem> loadInventoryCatalog() {
        vector<InventoryItem> items;
        const char* query = "SELECT id, min_dim, max_dim, sku_id, category, stock_qty FROM inventory_items ORDER BY min_dim ASC;";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                InventoryItem item;
                item.id = sqlite3_column_int(stmt, 0);
                item.min_dimension = sqlite3_column_double(stmt, 1);
                item.max_dimension = sqlite3_column_double(stmt, 2);
                item.sku_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                item.category_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                item.stock_quantity = sqlite3_column_int(stmt, 5);
                items.push_back(item);
            }
            sqlite3_finalize(stmt);
        }
        return items;
    }

    void logMeasurement(double raw, double scale, double computed, const string& sku) {
        const char* query = "INSERT INTO measurement_logs (raw_reading, scale_factor, computed_dimension, matched_sku) VALUES (?, ?, ?, ?);";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_double(stmt, 1, raw);
            sqlite3_bind_double(stmt, 2, scale);
            sqlite3_bind_double(stmt, 3, computed);
            sqlite3_bind_text(stmt, 4, sku.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
};

// ============================================================================
// O(log n) SEARCH PIPELINE
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
// MAIN DAEMON
// ============================================================================

int main() {
    DatabaseManager db_manager("measureverse.db");

    vector<CalibrationPoint> calibration_table = db_manager.loadCalibrationPoints();
    vector<InventoryItem> live_catalog = db_manager.loadInventoryCatalog();

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
    cout << "  MEASUREVERSE SIZING ENGINE & DASHBOARD READY       " << endl;
    cout << "  Web UI:       http://localhost:8080/               " << endl;
    cout << "  API Endpoint: http://localhost:8080/api/v1/measure " << endl;
    cout << "=====================================================" << endl;

    while (true) {
        int client_socket = accept(server_fd, nullptr, nullptr);
        if (client_socket < 0) {
            continue;
        }

        char buffer[8192] = {0};
        ssize_t bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);

        if (bytes_read > 0) {
            string request_str(buffer);
            stringstream http_response;

            // 1. Handle CORS Preflight
            if (request_str.rfind("OPTIONS", 0) == 0) {
                http_response << "HTTP/1.1 204 No Content\r\n";
                http_response << "Access-Control-Allow-Origin: *\r\n";
                http_response << "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n";
                http_response << "Access-Control-Allow-Headers: Content-Type\r\n";
                http_response << "Connection: close\r\n\r\n";
            }
            // 2. Handle Root Dashboard Request (GET /)
            else if (request_str.rfind("GET / ", 0) == 0 || request_str.rfind("GET /index.html", 0) == 0) {
                ifstream html_file("index.html");
                if (html_file.is_open()) {
                    stringstream html_stream;
                    html_stream << html_file.rdbuf();
                    string html_content = html_stream.str();

                    http_response << "HTTP/1.1 200 OK\r\n";
                    http_response << "Content-Type: text/html; charset=utf-8\r\n";
                    http_response << "Content-Length: " << html_content.size() << "\r\n";
                    http_response << "Connection: close\r\n\r\n";
                    http_response << html_content;
                } else {
                    string not_found = "<h1>404 Not Found: index.html missing</h1>";
                    http_response << "HTTP/1.1 404 Not Found\r\n";
                    http_response << "Content-Type: text/html\r\n";
                    http_response << "Content-Length: " << not_found.size() << "\r\n";
                    http_response << "Connection: close\r\n\r\n" << not_found;
                }
            }
            // 3. Handle REST Sizing Measurement (POST /api/v1/measure)
            else {
                size_t body_pos = request_str.find("\r\n\r\n");
                string body = (body_pos != string::npos) ? request_str.substr(body_pos + 4) : "";

                double raw_reading = extractJsonDouble(body, "raw_reading");
                double scale_factor = extractJsonDouble(body, "scale_factor");

                if (scale_factor == 0.0) {
                    scale_factor = 1.0;
                }

                double target_dim = calculateDimension(calibration_table, raw_reading, scale_factor);
                InventoryItem match = searchSKUBin(live_catalog, target_dim);

                db_manager.logMeasurement(raw_reading, scale_factor, target_dim, match.sku_id);

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
                http_response << "HTTP/1.1 200 OK\r\n";
                http_response << "Access-Control-Allow-Origin: *\r\n";
                http_response << "Content-Type: application/json\r\n";
                http_response << "Content-Length: " << json_payload.size() << "\r\n";
                http_response << "Connection: close\r\n\r\n";
                http_response << json_payload;
            }

            string full_response = http_response.str();
            write(client_socket, full_response.c_str(), full_response.size());
        }

        close(client_socket);
    }

    close(server_fd);
    return 0;
}
