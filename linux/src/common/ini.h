// Minimal INI document that preserves what it does not understand (unknown sections, keys and comments
// survive a read/modify/write cycle, so the [effects] / [background] sections written by the Windows
// version are kept). Accepts UTF-8 and UTF-16LE (with BOM, as Windows writes it); writes UTF-8.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace canon_eos_webcam {

class IniDoc {
public:
    // Parses text (already UTF-8).
    void Parse(const std::string& text);
    // Reads a file (UTF-8 or UTF-16LE with BOM). False if it can't be read.
    bool Load(const std::string& path);
    std::string Serialize() const;

    // Value of [section] key, or def. Section and key names are case-insensitive.
    std::string Get(const std::string& section, const std::string& key, const std::string& def = "") const;
    bool Has(const std::string& section, const std::string& key) const;
    int GetInt(const std::string& section, const std::string& key, int def) const;
    double GetDouble(const std::string& section, const std::string& key, double def) const;
    void Set(const std::string& section, const std::string& key, const std::string& value);

private:
    struct Line {
        std::string raw;   // original text (comments, blank lines, unparsed)
        std::string key;   // empty for non key=value lines
        std::string value;
    };
    struct Section {
        std::string name;  // empty = lines before the first section header
        std::vector<Line> lines;
    };
    Section* Find(const std::string& name);
    const Section* Find(const std::string& name) const;
    std::vector<Section> sections_;
};

// Reads a whole file; UTF-16LE is converted to UTF-8. False if the file doesn't exist.
bool ReadTextFile(const std::string& path, std::string& out);
// Atomic write (temp file in the same directory + rename), creating the directory if needed.
bool WriteFileAtomic(const std::string& path, const std::string& content);

}  // namespace canon_eos_webcam
