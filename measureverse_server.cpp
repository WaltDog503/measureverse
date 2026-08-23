#include <iostream>
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

    bool initSchema() {
        bool success = true;
        const char* schema_sql = 
            "CREATE TABLE IF NOT EXISTS calibration_points ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  sensor_val REAL NOT NULL,"
            "  offset_correction REAL NOT NULL"
            ");"
            "CREATE TABLE IF NOT EXISTS inventory_items ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  min_dim REAL NOT NULL,"
            "  max_dim REAL NOT NULL,"
            "  sku_id TEXT UNIQUE NOT NULL,"
            "  category TEXT NOT NULL,"
            "  stock_qty INTEGER NOT NULL"
            ");"
            "CREATE TABLE IF NOT EXISTS measurement_logs ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  raw_reading REAL NOT NULL,"
            "  scale_factor REAL NOT NULL,"
            "  computed_dimension REAL NOT NULL,"
            "  matched_sku TEXT NOT NULL,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");";

        char* err_msg = nullptr;
        if (sqlite3_exec(db, schema_sql, nullptr, nullptr, &err_msg) != SQLITE_OK) {
            cerr << "Schema creation error: " << err_msg << endl;
            sqlite3_free(err_msg);
            success = false;
        }

        // Seed initial data if tables are empty
        if (success) {
            const char* seed_sql = 
                "INSERT OR IGNORE INTO calibration_points (id, sensor_val, offset_correction) VALUES"
                "  (1, 10.0, 0.50), (2, 20.0, 0.85), (3, 30.0, 1.20), (4, 40.0, 1.65), (5, 50.0, 2.10);"
                "INSERT OR IGNORE INTO inventory_items (id, min_dim, max_dim, sku_id, category, stock_qty) VALUES"
                "  (1, 12.0, 18.0, 'SMH-SKU-SMALL', 'Topwear', 50),"
                "  (2, 18.1, 24.0, 'SMH-SKU-MED',   'Topwear', 75),"
                "  (3, 24.1, 32.0, 'SMH-SKU-LARGE', 'Topwear', 60),"
                "  (4, 32.1, 42.0, 'SMH-SKU-XLARGE', 'Topwear', 25);";
            sqlite3_exec(db, seed_sql, nullptr, nullptr, nullptr);
        }

        return success;
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
    if (!db_manager.initSchema()) {
        cerr << "Failed to initialize database schema." << endl;
        return 1;
    }

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
    cout << "  MEASUREVERSE SERVICE + SQLITE3 DATABASE ATTACHED   " << endl;
    cout << "  Database File: measureverse.db                     " << endl;
    cout << "  Listening on:  http://localhost:8080/api/v1/measure" << endl;
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

            // Log event to persistent SQLite database
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
