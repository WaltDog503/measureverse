#pragma once
// Minimal, strict parser for FLAT JSON objects ({"k": "str" | number | true | false | null}).
// Handles escapes correctly and only matches real top-level keys, never text inside values.
// Nested objects/arrays are rejected. Swap for nlohmann/json if payloads grow.

#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <variant>

namespace mv::json {

using Value = std::variant<std::nullptr_t, bool, double, std::string>;
using Object = std::map<std::string, Value>;

namespace detail {

inline void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

inline void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline std::optional<std::string> parseString(const std::string& s, size_t& i) {
    if (i >= s.size() || s[i] != '"') return std::nullopt;
    ++i;
    std::string out;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return out;
        if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
        if (c != '\\') { out += c; continue; }
        if (i >= s.size()) return std::nullopt;
        char e = s[i++];
        switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 > s.size()) return std::nullopt;
                unsigned cp = 0;
                for (int k = 0; k < 4; ++k) {
                    char h = s[i++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return std::nullopt;
                }
                appendUtf8(out, cp);
                break;
            }
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

inline std::optional<double> parseNumber(const std::string& s, size_t& i) {
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                            s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                            s[i] == '-' || s[i] == '+')) ++i;
    if (i == start) return std::nullopt;
    try {
        size_t used = 0;
        std::string tok = s.substr(start, i - start);
        double v = std::stod(tok, &used);
        if (used != tok.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace detail

// Returns nullopt on any syntax error.
inline std::optional<Object> parseFlatObject(const std::string& s) {
    using namespace detail;
    Object obj;
    size_t i = 0;
    skipWs(s, i);
    if (i >= s.size() || s[i] != '{') return std::nullopt;
    ++i;
    skipWs(s, i);
    if (i < s.size() && s[i] == '}') return obj;

    while (true) {
        skipWs(s, i);
        auto key = parseString(s, i);
        if (!key) return std::nullopt;
        skipWs(s, i);
        if (i >= s.size() || s[i] != ':') return std::nullopt;
        ++i;
        skipWs(s, i);
        if (i >= s.size()) return std::nullopt;

        Value val;
        if (s[i] == '"') {
            auto str = parseString(s, i);
            if (!str) return std::nullopt;
            val = *str;
        } else if (s.compare(i, 4, "true") == 0) { val = true; i += 4; }
        else if (s.compare(i, 5, "false") == 0) { val = false; i += 5; }
        else if (s.compare(i, 4, "null") == 0) { val = nullptr; i += 4; }
        else {
            auto num = parseNumber(s, i);
            if (!num) return std::nullopt;
            val = *num;
        }
        obj[*key] = std::move(val);

        skipWs(s, i);
        if (i >= s.size()) return std::nullopt;
        if (s[i] == ',') { ++i; continue; }
        if (s[i] == '}') { ++i; break; }
        return std::nullopt;
    }
    skipWs(s, i);
    return (i == s.size()) ? std::optional<Object>(obj) : std::nullopt;
}

// Accepts numbers or numeric strings (the front end sends both).
inline std::optional<double> getNumber(const Object& o, const std::string& key) {
    auto it = o.find(key);
    if (it == o.end()) return std::nullopt;
    if (const auto* d = std::get_if<double>(&it->second)) return *d;
    if (const auto* str = std::get_if<std::string>(&it->second)) {
        size_t i = 0;
        auto v = detail::parseNumber(*str, i);
        if (v && i == str->size()) return v;
    }
    return std::nullopt;
}

inline std::string getString(const Object& o, const std::string& key) {
    auto it = o.find(key);
    if (it == o.end()) return {};
    if (const auto* str = std::get_if<std::string>(&it->second)) return *str;
    return {};
}

// Escapes a string for safe embedding in JSON output. Also escapes <, >, &
// so values are inert even if a client mistakenly injects them into HTML.
inline std::string escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '<': out += "\\u003c"; break;
            case '>': out += "\\u003e"; break;
            case '&': out += "\\u0026"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

}  // namespace mv::json
