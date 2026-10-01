// ShakingMyHead (S.M.H.) Measureverse sizing engine - standalone POSIX HTTP server.
//
// Endpoints:
//   GET  /, /index.html          -> dashboard
//   POST /api/v1/measure         -> calibrate + match brackets + silhouette
//   POST /api/v1/vault/save      -> persist client fit profile, returns fit_token
//   POST /api/v1/orders/reserve  -> persist made-to-measure reservation
//   OPTIONS *                    -> CORS preflight
// Anything else -> 404 / 405.

#include "json_util.hpp"
#include "sizing.hpp"

#include <sqlite3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <signal.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr size_t kMaxHeaderBytes = 16 * 1024;
constexpr size_t kMaxBodyBytes = 64 * 1024;
constexpr double kMinMeasurement = 0.0;    // exclusive
constexpr double kMaxMeasurement = 120.0;  // inches, generous upper bound
constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 10.0;

// ============================================================================
// DATABASE LAYER (SQLite3, RAII)
// ============================================================================

class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("prepare failed: ") + sqlite3_errmsg(db));
        }
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int idx, const std::string& v) {
        // SQLITE_TRANSIENT: SQLite copies the text, so no lifetime coupling.
        sqlite3_bind_text(stmt_, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void bind(int idx, double v) { sqlite3_bind_double(stmt_, idx, v); }
    void bindNullable(int idx, const std::string& v) {
        if (v.empty()) sqlite3_bind_null(stmt_, idx); else bind(idx, v);
    }
    int step() { return sqlite3_step(stmt_); }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

class DatabaseManager {
public:
    explicit DatabaseManager(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
            sqlite3_close(db_);  // must close even on failure
            db_ = nullptr;
            throw std::runtime_error("cannot open database: " + msg);
        }
        sqlite3_busy_timeout(db_, 2000);
        createSchema();
    }
    ~DatabaseManager() { sqlite3_close(db_); }
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    bool logFit(double bust, double waist, double hip, const std::string& sku,
                const std::string& silhouette) {
        Statement st(db_,
            "INSERT INTO measurement_logs (bust, waist, hip, matched_sku, silhouette) "
            "VALUES (?, ?, ?, ?, ?);");
        st.bind(1, bust); st.bind(2, waist); st.bind(3, hip);
        st.bind(4, sku); st.bind(5, silhouette);
        return st.step() == SQLITE_DONE;
    }

    bool saveClientProfile(const std::string& token, const std::string& name,
                           const std::string& email, double b, double w, double h,
                           const std::string& sil, const std::string& sku) {
        Statement st(db_,
            "INSERT INTO client_profiles (fit_token, client_name, client_email, bust, waist, hip, "
            "silhouette, matched_sku) VALUES (?, ?, ?, ?, ?, ?, ?, ?);");
        st.bind(1, token); st.bind(2, name); st.bind(3, email);
        st.bind(4, b); st.bind(5, w); st.bind(6, h);
        st.bind(7, sil); st.bind(8, sku);
        return st.step() == SQLITE_DONE;
    }

    bool createOrderReservation(const std::string& order_tok, const std::string& fit_tok,
                                const std::string& title, const std::string& price,
                                double b, double w, double h, const std::string& sil) {
        Statement st(db_,
            "INSERT INTO order_reservations (order_token, fit_token, garment_title, garment_price, "
            "bust, waist, hip, silhouette) VALUES (?, ?, ?, ?, ?, ?, ?, ?);");
        st.bind(1, order_tok); st.bindNullable(2, fit_tok);
        st.bind(3, title); st.bind(4, price);
        st.bind(5, b); st.bind(6, w); st.bind(7, h); st.bind(8, sil);
        return st.step() == SQLITE_DONE;
    }

private:
    void createSchema() {
        static const char* kSchema = R"SQL(
            PRAGMA foreign_keys = ON;
            CREATE TABLE IF NOT EXISTS measurement_logs (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                bust        REAL NOT NULL,
                waist       REAL NOT NULL,
                hip         REAL NOT NULL,
                matched_sku TEXT NOT NULL,
                silhouette  TEXT NOT NULL,
                created_at  TEXT NOT NULL DEFAULT (datetime('now'))
            );
            CREATE TABLE IF NOT EXISTS client_profiles (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                fit_token    TEXT NOT NULL UNIQUE,
                client_name  TEXT NOT NULL,
                client_email TEXT NOT NULL,
                bust         REAL NOT NULL,
                waist        REAL NOT NULL,
                hip          REAL NOT NULL,
                silhouette   TEXT NOT NULL,
                matched_sku  TEXT NOT NULL,
                created_at   TEXT NOT NULL DEFAULT (datetime('now'))
            );
            CREATE TABLE IF NOT EXISTS order_reservations (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                order_token   TEXT NOT NULL UNIQUE,
                fit_token     TEXT REFERENCES client_profiles(fit_token),
                garment_title TEXT NOT NULL,
                garment_price TEXT NOT NULL,
                bust          REAL NOT NULL,
                waist         REAL NOT NULL,
                hip           REAL NOT NULL,
                silhouette    TEXT NOT NULL,
                created_at    TEXT NOT NULL DEFAULT (datetime('now'))
            );
        )SQL";
        char* err = nullptr;
        if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            throw std::runtime_error("schema creation failed: " + msg);
        }
    }

    sqlite3* db_ = nullptr;
};

// ============================================================================
// HELPERS
// ============================================================================

// 128-bit random hex token. One engine per process, seeded once.
std::string generateToken(const std::string& prefix) {
    static std::mt19937_64 gen{[] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }()};
    static const char* hex = "0123456789abcdef";
    std::string out = prefix + "-";
    for (int i = 0; i < 2; ++i) {
        uint64_t v = gen();
        for (int k = 0; k < 16; ++k) { out += hex[v & 0xF]; v >>= 4; }
    }
    return out;
}

bool validMeasurement(const std::optional<double>& v) {
    return v && *v > kMinMeasurement && *v <= kMaxMeasurement;
}

// ============================================================================
// HTTP LAYER
// ============================================================================

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

const char* reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool writeAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Reads headers, then exactly Content-Length body bytes. Returns error status on failure.
std::variant<HttpRequest, int> readRequest(int fd) {
    std::string buf;
    char chunk[4096];
    size_t header_end = std::string::npos;

    while ((header_end = buf.find("\r\n\r\n")) == std::string::npos) {
        if (buf.size() > kMaxHeaderBytes) return 413;
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return 400;
        buf.append(chunk, static_cast<size_t>(n));
    }

    HttpRequest req;
    std::istringstream head(buf.substr(0, header_end));
    std::string request_line;
    std::getline(head, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();
    {
        std::istringstream rl(request_line);
        std::string version;
        if (!(rl >> req.method >> req.path >> version)) return 400;
        if (auto q = req.path.find('?'); q != std::string::npos) req.path.resize(q);
    }

    size_t content_length = 0;
    std::string line;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (toLower(line.substr(0, colon)) == "content-length") {
            try {
                content_length = std::stoul(line.substr(colon + 1));
            } catch (...) {
                return 400;
            }
        }
    }
    if (content_length > kMaxBodyBytes) return 413;

    req.body = buf.substr(header_end + 4);
    while (req.body.size() < content_length) {
        ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return 400;
        req.body.append(chunk, static_cast<size_t>(n));
    }
    req.body.resize(content_length);
    return req;
}

std::string serialize(const HttpResponse& r) {
    std::ostringstream out;
    out << "HTTP/1.1 " << r.status << ' ' << reasonPhrase(r.status) << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type\r\n"
        << "X-Content-Type-Options: nosniff\r\n";
    if (r.status != 204) {
        out << "Content-Type: " << r.content_type << "\r\n"
            << "Content-Length: " << r.body.size() << "\r\n";
    }
    out << "Connection: close\r\n\r\n";
    if (r.status != 204) out << r.body;
    return out.str();
}

HttpResponse jsonError(int status, const std::string& msg) {
    return {status, "application/json",
            "{\"status\": \"error\", \"message\": \"" + mv::json::escape(msg) + "\"}"};
}

// ============================================================================
// APPLICATION / ROUTES
// ============================================================================

class App {
public:
    explicit App(DatabaseManager& db) : db_(db) {}

    HttpResponse handle(const HttpRequest& req) {
        if (req.method == "OPTIONS") return {204, "", ""};

        if (req.path == "/" || req.path == "/index.html") {
            if (req.method != "GET") return jsonError(405, "method not allowed");
            return serveIndex();
        }

        if (req.path == "/api/v1/measure" || req.path == "/api/v1/vault/save" ||
            req.path == "/api/v1/orders/reserve") {
            if (req.method != "POST") return jsonError(405, "method not allowed");
            auto obj = mv::json::parseFlatObject(req.body);
            if (!obj) return jsonError(400, "body must be a flat JSON object");

            try {
                if (req.path == "/api/v1/measure") return measure(*obj);
                if (req.path == "/api/v1/vault/save") return saveVault(*obj);
                return reserveOrder(*obj);
            } catch (const std::exception& e) {
                std::cerr << "[error] " << e.what() << '\n';
                return jsonError(500, "internal error");
            }
        }
        return jsonError(404, "not found");
    }

private:
    HttpResponse serveIndex() {
        std::ifstream f("index.html", std::ios::binary);
        if (!f) return {404, "text/html; charset=utf-8", "<h1>404: index.html missing</h1>"};
        std::ostringstream ss;
        ss << f.rdbuf();
        return {200, "text/html; charset=utf-8", ss.str()};
    }

    HttpResponse measure(const mv::json::Object& o) {
        auto bust = mv::json::getNumber(o, "bust");
        auto waist = mv::json::getNumber(o, "waist");
        auto hip = mv::json::getNumber(o, "hip");
        double scale = mv::json::getNumber(o, "scale_factor").value_or(1.0);

        if (!validMeasurement(bust) || !validMeasurement(waist) || !validMeasurement(hip)) {
            return jsonError(400, "bust, waist and hip must be numbers in (0, 120]");
        }
        if (scale < kMinScale || scale > kMaxScale) {
            return jsonError(400, "scale_factor must be in [0.1, 10]");
        }

        const double cb = mv::calibrateAxis(kCalibration, *bust, scale);
        const double cw = mv::calibrateAxis(kCalibration, *waist, scale);
        const double ch = mv::calibrateAxis(kCalibration, *hip, scale);

        const std::string sku = mv::matchBracket(kBust, cb) + " | " +
                                mv::matchBracket(kWaist, cw) + " | " +
                                mv::matchBracket(kHip, ch);
        const std::string silhouette = mv::classifySilhouette(cb, cw, ch);

        if (!db_.logFit(cb, cw, ch, sku, silhouette)) {
            std::cerr << "[warn] failed to log measurement\n";
        }

        std::ostringstream js;
        js << "{\n"
           << "  \"calibrated_bust\": " << cb << ",\n"
           << "  \"calibrated_waist\": " << cw << ",\n"
           << "  \"calibrated_hip\": " << ch << ",\n"
           << "  \"matched_sku\": \"" << mv::json::escape(sku) << "\",\n"
           << "  \"silhouette\": \"" << mv::json::escape(silhouette) << "\",\n"
           << "  \"status\": \"success\"\n}";
        return {200, "application/json", js.str()};
    }

    HttpResponse saveVault(const mv::json::Object& o) {
        const std::string name = mv::json::getString(o, "client_name");
        const std::string email = mv::json::getString(o, "client_email");
        auto b = mv::json::getNumber(o, "bust");
        auto w = mv::json::getNumber(o, "waist");
        auto h = mv::json::getNumber(o, "hip");
        const std::string sil = mv::json::getString(o, "silhouette");
        const std::string sku = mv::json::getString(o, "matched_sku");

        if (name.empty() || name.size() > 200) return jsonError(400, "client_name required (<= 200 chars)");
        if (email.size() > 254 || email.find('@') == std::string::npos)
            return jsonError(400, "valid client_email required");
        if (!validMeasurement(b) || !validMeasurement(w) || !validMeasurement(h))
            return jsonError(400, "bust, waist and hip must be numbers in (0, 120]");

        const std::string token = generateToken("SMH-VAULT");
        if (!db_.saveClientProfile(token, name, email, *b, *w, *h, sil, sku))
            return jsonError(500, "could not save profile");

        std::ostringstream js;
        js << "{\n"
           << "  \"status\": \"success\",\n"
           << "  \"fit_token\": \"" << token << "\",\n"
           << "  \"client_name\": \"" << mv::json::escape(name) << "\"\n}";
        return {200, "application/json", js.str()};
    }

    HttpResponse reserveOrder(const mv::json::Object& o) {
        const std::string fit_tok = mv::json::getString(o, "fit_token");
        const std::string title = mv::json::getString(o, "garment_title");
        const std::string price = mv::json::getString(o, "garment_price");
        auto b = mv::json::getNumber(o, "bust");
        auto w = mv::json::getNumber(o, "waist");
        auto h = mv::json::getNumber(o, "hip");
        const std::string sil = mv::json::getString(o, "silhouette");

        if (title.empty() || title.size() > 200) return jsonError(400, "garment_title required");
        if (price.empty() || price.size() > 50) return jsonError(400, "garment_price required");
        if (!validMeasurement(b) || !validMeasurement(w) || !validMeasurement(h))
            return jsonError(400, "bust, waist and hip must be numbers in (0, 120]");

        const std::string order_tok = generateToken("SMH-COUTURE");
        if (!db_.createOrderReservation(order_tok, fit_tok, title, price, *b, *w, *h, sil))
            return jsonError(400, "could not reserve order (unknown fit_token?)");

        std::ostringstream js;
        js << "{\n"
           << "  \"status\": \"success\",\n"
           << "  \"order_token\": \"" << order_tok << "\",\n"
           << "  \"garment\": \"" << mv::json::escape(title) << "\",\n"
           << "  \"price\": \"" << mv::json::escape(price) << "\"\n}";
        return {200, "application/json", js.str()};
    }

    DatabaseManager& db_;

    // Sorted by min_val. Each bracket now spans up to the next bracket's min (no gaps).
    const std::vector<mv::CalibrationPoint> kCalibration = {
        {10.0, 0.25}, {20.0, 0.40}, {30.0, 0.65}, {40.0, 0.90}, {50.0, 1.15}};
    const std::vector<mv::SizingBracket> kBust = {
        {31.5, 33.5, "SMH-00 (0-2)"}, {33.6, 35.5, "SMH-01 (4-6)"},
        {35.6, 38.0, "SMH-02 (8-10)"}, {38.1, 41.5, "SMH-03 (12-14)"},
        {41.6, 46.0, "SMH-04 (16+)"}};
    const std::vector<mv::SizingBracket> kWaist = {
        {24.0, 26.0, "W-PETITE"}, {26.1, 28.5, "W-SLENDER"},
        {28.6, 31.5, "W-CLASSIC"}, {31.6, 35.5, "W-CURVE"}};
    const std::vector<mv::SizingBracket> kHip = {
        {34.0, 36.5, "H-SLEEK"}, {36.6, 39.5, "H-BALANCED"},
        {39.6, 43.0, "H-CONTOUR"}, {43.1, 48.0, "H-VOLUPTUOUS"}};
};

volatile std::sig_atomic_t g_running = 1;
void onSignal(int) { g_running = 0; }

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);  // never die on a disconnected client
    // sigaction without SA_RESTART so a blocked accept() returns EINTR on Ctrl+C.
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int port = 8080;
    if (argc > 1) port = std::atoi(argv[1]);
    if (const char* env = std::getenv("MEASUREVERSE_PORT")) port = std::atoi(env);
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port.\n";
        return 1;
    }
    const char* db_path = std::getenv("MEASUREVERSE_DB");

    try {
        DatabaseManager db(db_path ? db_path : "measureverse.db");
        App app(db);

        int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) { std::cerr << "Failed to create socket.\n"; return 1; }

        int opt = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<uint16_t>(port));

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            std::cerr << "Bind failed on port " << port << ".\n";
            ::close(server_fd);
            return 1;
        }
        if (::listen(server_fd, 16) < 0) {
            std::cerr << "Listen failed.\n";
            ::close(server_fd);
            return 1;
        }

        std::cout << "=====================================================\n"
                  << "  SHAKINGMYHEAD COUTURE ATELIER SIZING ENGINE\n"
                  << "  Listening on: http://localhost:" << port << "/\n"
                  << "=====================================================" << std::endl;

        while (g_running) {
            int client = ::accept(server_fd, nullptr, nullptr);
            if (client < 0) continue;  // EINTR on shutdown signal lands here too

            timeval tv{5, 0};  // slow clients can't hang the single-threaded loop forever
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            auto parsed = readRequest(client);
            HttpResponse resp = std::holds_alternative<int>(parsed)
                ? jsonError(std::get<int>(parsed), "malformed request")
                : app.handle(std::get<HttpRequest>(parsed));

            if (!writeAll(client, serialize(resp))) {
                std::cerr << "[warn] client disconnected before response was sent\n";
            }
            ::close(client);
        }

        ::close(server_fd);
        std::cout << "\nShutting down cleanly.\n";
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
