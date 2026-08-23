#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <cmath>
#include <random>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sqlite3.h>

using namespace std;

// ============================================================================
// DATA MODELS & STRUCTS
// ============================================================================

struct CalibrationPoint {
    double sensor_val;
    double offset_correction;
};

struct SizingBracket {
    double min_val;
    double max_val;
    string code;
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

    void logCoutureFit(double bust, double waist, double hip, const string& sku, const string& silhouette) {
        const char* query = "INSERT INTO measurement_logs (raw_reading, scale_factor, computed_dimension, matched_sku) VALUES (?, ?, ?, ?);";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_double(stmt, 1, bust);
            sqlite3_bind_double(stmt, 2, waist);
            sqlite3_bind_double(stmt, 3, hip);
            string full_entry = sku + " [" + silhouette + "]";
            sqlite3_bind_text(stmt, 4, full_entry.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    bool saveClientProfile(const string& token, const string& name, const string& email, double b, double w, double h, const string& sil, const string& sku) {
        bool success = false;
        const char* query = "INSERT INTO client_profiles (fit_token, client_name, client_email, bust, waist, hip, silhouette, matched_sku) VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, email.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_double(stmt, 4, b);
            sqlite3_bind_double(stmt, 5, w);
            sqlite3_bind_double(stmt, 6, h);
            sqlite3_bind_text(stmt, 7, sil.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 8, sku.c_str(), -1, SQLITE_STATIC);
            
            if (sqlite3_step(stmt) == SQLITE_DONE) {
                success = true;
            }
            sqlite3_finalize(stmt);
        }
        return success;
    }

    bool createOrderReservation(const string& order_tok, const string& fit_tok, const string& title, const string& price, double b, double w, double h, const string& sil) {
        bool success = false;
        const char* query = "INSERT INTO order_reservations (order_token, fit_token, garment_title, garment_price, bust, waist, hip, silhouette) VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db, query, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, order_tok.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, fit_tok.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, title.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, price.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_double(stmt, 5, b);
            sqlite3_bind_double(stmt, 6, w);
            sqlite3_bind_double(stmt, 7, h);
            sqlite3_bind_text(stmt, 8, sil.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) == SQLITE_DONE) {
                success = true;
            }
            sqlite3_finalize(stmt);
        }
        return success;
    }
};

// ============================================================================
// O(log n) SEARCH ALGORITHMS & HELPERS
// ============================================================================

double calibrateAxis(const vector<CalibrationPoint>& table, double raw_input, double scale) {
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

string matchBracket(const vector<SizingBracket>& brackets, double dimension) {
    string result = "BESPOKE";
    
    if (!brackets.empty()) {
        size_t low = 0;
        size_t high = brackets.size() - 1;
        bool found = false;
        
        while (low <= high && !found && high < brackets.size()) {
            size_t mid = low + (high - low) / 2;
            if (dimension >= brackets[mid].min_val && dimension <= brackets[mid].max_val) {
                result = brackets[mid].code;
                found = true;
            } else if (dimension < brackets[mid].min_val) {
                if (mid == 0) {
                    high = brackets.size();
                } else {
                    high = mid - 1;
                }
            } else {
                low = mid + 1;
            }
        }
    }
    
    return result;
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

string extractJsonString(const string& body, const string& key) {
    string value = "";
    size_t key_pos = body.find("\"" + key + "\"");
    if (key_pos != string::npos) {
        size_t colon_pos = body.find(":", key_pos);
        if (colon_pos != string::npos) {
            size_t first_quote = body.find("\"", colon_pos + 1);
            if (first_quote != string::npos) {
                size_t second_quote = body.find("\"", first_quote + 1);
                if (second_quote != string::npos) {
                    value = body.substr(first_quote + 1, second_quote - first_quote - 1);
                }
            }
        }
    }
    return value;
}

string generateToken(const string& prefix) {
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> dis(1000, 9999);
    return prefix + "-" + to_string(dis(gen));
}

// ============================================================================
// MAIN REST SERVICE & ROUTER
// ============================================================================

int main() {
    DatabaseManager db_manager("measureverse.db");

    const vector<CalibrationPoint> calibration_table = {
        {10.0, 0.25},
        {20.0, 0.40},
        {30.0, 0.65},
        {40.0, 0.90},
        {50.0, 1.15}
    };

    const vector<SizingBracket> bust_brackets = {
        {31.5, 33.5, "SMH-00 (0-2)"},
        {33.6, 35.5, "SMH-01 (4-6)"},
        {35.6, 38.0, "SMH-02 (8-10)"},
        {38.1, 41.5, "SMH-03 (12-14)"},
        {41.6, 46.0, "SMH-04 (16+)"}
    };

    const vector<SizingBracket> waist_brackets = {
        {24.0, 26.0, "W-PETITE"},
        {26.1, 28.5, "W-SLENDER"},
        {28.6, 31.5, "W-CLASSIC"},
        {31.6, 35.5, "W-CURVE"}
    };

    const vector<SizingBracket> hip_brackets = {
        {34.0, 36.5, "H-SLEEK"},
        {36.6, 39.5, "H-BALANCED"},
        {39.6, 43.0, "H-CONTOUR"},
        {43.1, 48.0, "H-VOLUPTUOUS"}
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
    cout << "  SHAKINGMYHEAD COUTURE ATELIER SIZING ENGINE        " << endl;
    cout << "  Listening on: http://localhost:8080/               " << endl;
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

            if (request_str.rfind("OPTIONS", 0) == 0) {
                http_response << "HTTP/1.1 204 No Content\r\n";
                http_response << "Access-Control-Allow-Origin: *\r\n";
                http_response << "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n";
                http_response << "Access-Control-Allow-Headers: Content-Type\r\n";
                http_response << "Connection: close\r\n\r\n";
            }
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
                    string not_found = "<h1>404: index.html missing</h1>";
                    http_response << "HTTP/1.1 404 Not Found\r\n";
                    http_response << "Content-Type: text/html\r\n";
                    http_response << "Content-Length: " << not_found.size() << "\r\n";
                    http_response << "Connection: close\r\n\r\n" << not_found;
                }
            }
            // ROUTE: Save to Client Sizing Vault
            else if (request_str.find("POST /api/v1/vault/save") != string::npos) {
                size_t body_pos = request_str.find("\r\n\r\n");
                string body = (body_pos != string::npos) ? request_str.substr(body_pos + 4) : "";

                string name = extractJsonString(body, "client_name");
                string email = extractJsonString(body, "client_email");
                double bust = extractJsonDouble(body, "bust");
                double waist = extractJsonDouble(body, "waist");
                double hip = extractJsonDouble(body, "hip");
                string silhouette = extractJsonString(body, "silhouette");
                string sku = extractJsonString(body, "matched_sku");

                string fit_token = generateToken("SMH-VAULT");
                bool saved = db_manager.saveClientProfile(fit_token, name, email, bust, waist, hip, silhouette, sku);

                stringstream json_response;
                json_response << "{\n";
                json_response << "  \"status\": \"" << (saved ? "success" : "error") << "\",\n";
                json_response << "  \"fit_token\": \"" << fit_token << "\",\n";
                json_response << "  \"client_name\": \"" << name << "\"\n";
                json_response << "}";

                string json_payload = json_response.str();
                http_response << "HTTP/1.1 200 OK\r\n";
                http_response << "Access-Control-Allow-Origin: *\r\n";
                http_response << "Content-Type: application/json\r\n";
                http_response << "Content-Length: " << json_payload.size() << "\r\n";
                http_response << "Connection: close\r\n\r\n";
                http_response << json_payload;
            }
            // ROUTE: Reserve Custom Atelier Order
            else if (request_str.find("POST /api/v1/orders/reserve") != string::npos) {
                size_t body_pos = request_str.find("\r\n\r\n");
                string body = (body_pos != string::npos) ? request_str.substr(body_pos + 4) : "";

                string fit_tok = extractJsonString(body, "fit_token");
                string title = extractJsonString(body, "garment_title");
                string price = extractJsonString(body, "garment_price");
                double bust = extractJsonDouble(body, "bust");
                double waist = extractJsonDouble(body, "waist");
                double hip = extractJsonDouble(body, "hip");
                string silhouette = extractJsonString(body, "silhouette");

                string order_tok = generateToken("SMH-COUTURE");
                bool ordered = db_manager.createOrderReservation(order_tok, fit_tok, title, price, bust, waist, hip, silhouette);

                stringstream json_response;
                json_response << "{\n";
                json_response << "  \"status\": \"" << (ordered ? "success" : "error") << "\",\n";
                json_response << "  \"order_token\": \"" << order_tok << "\",\n";
                json_response << "  \"garment\": \"" << title << "\",\n";
                json_response << "  \"price\": \"" << price << "\"\n";
                json_response << "}";

                string json_payload = json_response.str();
                http_response << "HTTP/1.1 200 OK\r\n";
                http_response << "Access-Control-Allow-Origin: *\r\n";
                http_response << "Content-Type: application/json\r\n";
                http_response << "Content-Length: " << json_payload.size() << "\r\n";
                http_response << "Connection: close\r\n\r\n";
                http_response << json_payload;
            }
            // ROUTE: Sizing Calculation (POST /api/v1/measure)
            else {
                size_t body_pos = request_str.find("\r\n\r\n");
                string body = (body_pos != string::npos) ? request_str.substr(body_pos + 4) : "";

                double raw_bust = extractJsonDouble(body, "bust");
                double raw_waist = extractJsonDouble(body, "waist");
                double raw_hip = extractJsonDouble(body, "hip");
                double scale = extractJsonDouble(body, "scale_factor");

                if (scale == 0.0) scale = 1.0;

                double cal_bust = calibrateAxis(calibration_table, raw_bust, scale);
                double cal_waist = calibrateAxis(calibration_table, raw_waist, scale);
                double cal_hip = calibrateAxis(calibration_table, raw_hip, scale);

                string bust_tier = matchBracket(bust_brackets, cal_bust);
                string waist_tier = matchBracket(waist_brackets, cal_waist);
                string hip_tier = matchBracket(hip_brackets, cal_hip);

                string silhouette = "Classic Atelier";
                double bust_waist_diff = cal_bust - cal_waist;
                double hip_waist_diff = cal_hip - cal_waist;

                if (bust_waist_diff >= 8.5 && hip_waist_diff >= 9.5) {
                    silhouette = "Hourglass Luxe";
                } else if (hip_waist_diff >= 10.0 && bust_waist_diff < 7.0) {
                    silhouette = "Sculpted Mermaid";
                } else if (bust_waist_diff >= 9.0 && hip_waist_diff < 7.0) {
                    silhouette = "Empire Architectural";
                } else if (abs(cal_bust - cal_hip) <= 2.5 && cal_waist >= cal_bust - 5.0) {
                    silhouette = "Minimalist Column";
                }

                string master_sku = bust_tier + " | " + waist_tier + " | " + hip_tier;
                db_manager.logCoutureFit(cal_bust, cal_waist, cal_hip, master_sku, silhouette);

                stringstream json_response;
                json_response << "{\n";
                json_response << "  \"calibrated_bust\": " << cal_bust << ",\n";
                json_response << "  \"calibrated_waist\": " << cal_waist << ",\n";
                json_response << "  \"calibrated_hip\": " << cal_hip << ",\n";
                json_response << "  \"matched_sku\": \"" << master_sku << "\",\n";
                json_response << "  \"silhouette\": \"" << silhouette << "\",\n";
                json_response << "  \"status\": \"success\"\n";
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
