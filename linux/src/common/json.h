// A small JSON value (parse + serialize), enough for the control protocol (one JSON object per line).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace canon_eos_webcam {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool b) : type_(Type::Bool), num_(b ? 1 : 0) {}
    Json(int n) : type_(Type::Number), num_(n) {}
    Json(int64_t n) : type_(Type::Number), num_(double(n)) {}
    Json(uint32_t n) : type_(Type::Number), num_(n) {}
    Json(uint64_t n) : type_(Type::Number), num_(double(n)) {}
    Json(double n) : type_(Type::Number), num_(n) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    static Json Array() { Json j; j.type_ = Type::Array; return j; }
    static Json Object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool IsObject() const { return type_ == Type::Object; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsArray() const { return type_ == Type::Array; }

    double Num(double def = 0) const { return type_ == Type::Number ? num_ : (type_ == Type::Bool ? num_ : def); }
    int Int(int def = 0) const { return type_ == Type::Number ? int(num_) : def; }
    bool Bool(bool def = false) const { return type_ == Type::Bool ? num_ != 0 : (type_ == Type::Number ? num_ != 0 : def); }
    const std::string& Str() const { return str_; }
    const std::vector<Json>& Items() const { return arr_; }
    const std::map<std::string, Json>& Members() const { return obj_; }

    bool Has(const std::string& k) const { return type_ == Type::Object && obj_.count(k) != 0; }
    const Json& operator[](const std::string& k) const {
        static const Json null;
        auto it = obj_.find(k);
        return it == obj_.end() ? null : it->second;
    }
    Json& Set(const std::string& k, Json v) { type_ = Type::Object; return obj_[k] = std::move(v); }
    void Push(Json v) { type_ = Type::Array; arr_.push_back(std::move(v)); }

    std::string Dump() const {
        std::string s;
        Write(s);
        return s;
    }
    // Returns false on a syntax error.
    static bool Parse(const std::string& text, Json& out) {
        size_t i = 0;
        if (!ParseValue(text, i, out, 0)) return false;
        SkipWs(text, i);
        return i == text.size();
    }

private:
    static void SkipWs(const std::string& t, size_t& i) {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
    }
    static void WriteString(std::string& o, const std::string& s) {
        o += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                    else o += char(c);
            }
        }
        o += '"';
    }
    void Write(std::string& o) const {
        switch (type_) {
            case Type::Null: o += "null"; break;
            case Type::Bool: o += num_ != 0 ? "true" : "false"; break;
            case Type::Number: {
                char b[40];
                if (num_ == double(int64_t(num_))) snprintf(b, sizeof b, "%lld", static_cast<long long>(num_));
                else snprintf(b, sizeof b, "%.6g", num_);
                o += b;
                break;
            }
            case Type::String: WriteString(o, str_); break;
            case Type::Array: {
                o += '[';
                bool first = true;
                for (const Json& j : arr_) { if (!first) o += ','; first = false; j.Write(o); }
                o += ']';
                break;
            }
            case Type::Object: {
                o += '{';
                bool first = true;
                for (auto& kv : obj_) {
                    if (!first) o += ',';
                    first = false;
                    WriteString(o, kv.first);
                    o += ':';
                    kv.second.Write(o);
                }
                o += '}';
                break;
            }
        }
    }
    static bool ParseString(const std::string& t, size_t& i, std::string& out) {
        if (i >= t.size() || t[i] != '"') return false;
        i++;
        out.clear();
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) return false;
            char e = t[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > t.size()) return false;
                    unsigned cp = unsigned(strtoul(t.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                    else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: out += e;
            }
        }
        return false;
    }
    static bool ParseValue(const std::string& t, size_t& i, Json& out, int depth) {
        if (depth > 16) return false;
        SkipWs(t, i);
        if (i >= t.size()) return false;
        char c = t[i];
        if (c == '{') {
            i++;
            out = Object();
            SkipWs(t, i);
            if (i < t.size() && t[i] == '}') { i++; return true; }
            for (;;) {
                SkipWs(t, i);
                std::string k;
                if (!ParseString(t, i, k)) return false;
                SkipWs(t, i);
                if (i >= t.size() || t[i++] != ':') return false;
                Json v;
                if (!ParseValue(t, i, v, depth + 1)) return false;
                out.obj_[k] = std::move(v);
                SkipWs(t, i);
                if (i >= t.size()) return false;
                if (t[i] == ',') { i++; continue; }
                if (t[i] == '}') { i++; return true; }
                return false;
            }
        }
        if (c == '[') {
            i++;
            out = Array();
            SkipWs(t, i);
            if (i < t.size() && t[i] == ']') { i++; return true; }
            for (;;) {
                Json v;
                if (!ParseValue(t, i, v, depth + 1)) return false;
                out.arr_.push_back(std::move(v));
                SkipWs(t, i);
                if (i >= t.size()) return false;
                if (t[i] == ',') { i++; continue; }
                if (t[i] == ']') { i++; return true; }
                return false;
            }
        }
        if (c == '"') {
            std::string s;
            if (!ParseString(t, i, s)) return false;
            out = Json(std::move(s));
            return true;
        }
        if (t.compare(i, 4, "true") == 0) { out = Json(true); i += 4; return true; }
        if (t.compare(i, 5, "false") == 0) { out = Json(false); i += 5; return true; }
        if (t.compare(i, 4, "null") == 0) { out = Json(); i += 4; return true; }
        char* end = nullptr;
        double d = strtod(t.c_str() + i, &end);
        if (end == t.c_str() + i) return false;
        i = size_t(end - t.c_str());
        out = Json(d);
        return true;
    }

    Type type_ = Type::Null;
    double num_ = 0;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
};

inline std::string Base64Encode(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += t[(v >> 18) & 63];
        o += t[(v >> 12) & 63];
        o += i + 1 < n ? t[(v >> 6) & 63] : '=';
        o += i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}

inline std::vector<uint8_t> Base64Decode(const std::string& s) {
    std::vector<uint8_t> o;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | uint32_t(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back(uint8_t(acc >> bits)); }
    }
    return o;
}

}  // namespace canon_eos_webcam
