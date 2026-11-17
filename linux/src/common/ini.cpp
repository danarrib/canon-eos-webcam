#include "ini.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace canon_eos_webcam {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

void AppendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}

std::string Utf16leToUtf8(const std::string& in, size_t start) {
    std::string out;
    for (size_t i = start; i + 1 < in.size(); i += 2) {
        uint32_t c = uint8_t(in[i]) | uint32_t(uint8_t(in[i + 1])) << 8;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < in.size()) {
            uint32_t d = uint8_t(in[i + 2]) | uint32_t(uint8_t(in[i + 3])) << 8;
            if (d >= 0xDC00 && d < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i += 2;
            }
        }
        AppendUtf8(out, c);
    }
    return out;
}

}  // namespace

bool ReadTextFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 2 && uint8_t(s[0]) == 0xFF && uint8_t(s[1]) == 0xFE) s = Utf16leToUtf8(s, 2);
    else if (s.size() >= 3 && uint8_t(s[0]) == 0xEF && uint8_t(s[1]) == 0xBB && uint8_t(s[2]) == 0xBF) s.erase(0, 3);
    out = std::move(s);
    return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& content) {
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        std::string dir = path.substr(0, slash);
        // mkdir -p
        for (size_t i = 1; i <= dir.size(); i++)
            if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
    }
    std::string tmp = path + ".tmp" + std::to_string(getpid());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), std::streamsize(content.size()));
        if (!f) { unlink(tmp.c_str()); return false; }
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

void IniDoc::Parse(const std::string& text) {
    sections_.clear();
    sections_.push_back({"", {}});
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = Trim(line);
        if (!t.empty() && t[0] == '[' && t.back() == ']') {
            sections_.push_back({Trim(t.substr(1, t.size() - 2)), {}});
            continue;
        }
        Line l;
        l.raw = line;
        if (!t.empty() && t[0] != ';' && t[0] != '#') {
            size_t eq = t.find('=');
            if (eq != std::string::npos && eq > 0) {
                l.key = Trim(t.substr(0, eq));
                l.value = Trim(t.substr(eq + 1));
            }
        }
        sections_.back().lines.push_back(std::move(l));
    }
}

bool IniDoc::Load(const std::string& path) {
    std::string s;
    if (!ReadTextFile(path, s)) return false;
    Parse(s);
    return true;
}

std::string IniDoc::Serialize() const {
    std::string out;
    for (const Section& s : sections_) {
        if (!s.name.empty()) out += "[" + s.name + "]\n";
        for (const Line& l : s.lines) out += l.raw + "\n";
    }
    return out;
}

IniDoc::Section* IniDoc::Find(const std::string& name) {
    std::string n = Lower(name);
    for (Section& s : sections_)
        if (Lower(s.name) == n) return &s;
    return nullptr;
}
const IniDoc::Section* IniDoc::Find(const std::string& name) const {
    return const_cast<IniDoc*>(this)->Find(name);
}

bool IniDoc::Has(const std::string& section, const std::string& key) const {
    const Section* s = Find(section);
    if (!s) return false;
    std::string k = Lower(key);
    for (const Line& l : s->lines)
        if (!l.key.empty() && Lower(l.key) == k) return true;
    return false;
}

std::string IniDoc::Get(const std::string& section, const std::string& key, const std::string& def) const {
    const Section* s = Find(section);
    if (!s) return def;
    std::string k = Lower(key);
    std::string r = def;
    for (const Line& l : s->lines)
        if (!l.key.empty() && Lower(l.key) == k) r = l.value;  // the last one wins, like GetPrivateProfileString
    return r;
}

int IniDoc::GetInt(const std::string& section, const std::string& key, int def) const {
    std::string v = Get(section, key);
    if (v.empty()) return def;
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    return end != v.c_str() ? int(n) : def;
}

double IniDoc::GetDouble(const std::string& section, const std::string& key, double def) const {
    std::string v = Get(section, key);
    if (v.empty()) return def;
    std::replace(v.begin(), v.end(), ',', '.');
    char* end = nullptr;
    double d = strtod(v.c_str(), &end);  // the "C" locale: the service never calls setlocale
    return end != v.c_str() ? d : def;
}

void IniDoc::Set(const std::string& section, const std::string& key, const std::string& value) {
    Section* s = Find(section);
    if (!s) {
        sections_.push_back({section, {}});
        s = &sections_.back();
    }
    std::string k = Lower(key);
    for (Line& l : s->lines)
        if (!l.key.empty() && Lower(l.key) == k) {
            if (l.value != value) {
                l.value = value;
                l.raw = l.key + " = " + value;
            }
            return;
        }
    // appends after the last key=value line of the section (before trailing blank lines)
    size_t pos = s->lines.size();
    while (pos > 0 && Trim(s->lines[pos - 1].raw).empty()) pos--;
    Line l;
    l.key = key;
    l.value = value;
    l.raw = key + " = " + value;
    s->lines.insert(s->lines.begin() + ptrdiff_t(pos), std::move(l));
}

}  // namespace canon_eos_webcam
