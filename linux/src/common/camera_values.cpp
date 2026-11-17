#include "camera_values.h"

#include "i18n.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <strings.h>

namespace canon_eos_webcam {

namespace {

struct NamedValue { uint32_t value; Str name; };
struct LiteralValue { uint32_t value; const char* name; };

const LiteralValue kIsoNames[] = {
    {0x0000, "Auto"}, {0x0001, "Auto"}, {0x0028, "6"}, {0x0030, "12"}, {0x0038, "25"}, {0x0040, "50"},
    {0x0043, "64"}, {0x0045, "80"}, {0x0048, "100"}, {0x004B, "125"}, {0x004D, "160"}, {0x0050, "200"},
    {0x0053, "250"}, {0x0055, "320"}, {0x0058, "400"}, {0x005B, "500"}, {0x005D, "640"}, {0x0060, "800"},
    {0x0063, "1000"}, {0x0065, "1250"}, {0x0068, "1600"}, {0x006B, "2000"}, {0x006D, "2500"},
    {0x0070, "3200"}, {0x0073, "4000"}, {0x0075, "5000"}, {0x0078, "6400"}, {0x007B, "8000"},
    {0x007D, "10000"}, {0x0080, "12800"}, {0x0083, "16000"}, {0x0085, "20000"}, {0x0088, "25600"},
};
const NamedValue kWbNames[] = {
    {0, Str::WbAuto}, {1, Str::WbDaylight}, {8, Str::WbShade}, {2, Str::WbCloudy}, {3, Str::WbTungsten},
    {4, Str::WbFluorescent}, {5, Str::WbFlash}, {6, Str::WbCustom}, {9, Str::WbColorTemp},
    {10, Str::WbPc1}, {11, Str::WbPc2}, {12, Str::WbPc3},
};
const NamedValue kStyleNames[] = {
    {0x81, Str::PsStandard}, {0x82, Str::PsPortrait}, {0x83, Str::PsLandscape}, {0x84, Str::PsNeutral},
    {0x85, Str::PsFaithful}, {0x86, Str::PsMonochrome}, {0x87, Str::PsAuto}, {0x88, Str::PsFineDetail},
    {0x21, Str::PsUser1}, {0x22, Str::PsUser2}, {0x23, Str::PsUser3},
};
// Auto Lighting Optimizer on the simpler models (T3): 0 standard, 1 low, 2 strong, 3 disabled.
const NamedValue kAloNames[] = {{0, Str::AloStandard}, {1, Str::AloLow}, {2, Str::AloStrong}, {3, Str::AloDisabled}};

template <size_t N>
std::string NameOf(const NamedValue (&t)[N], uint32_t v) {
    for (auto& e : t)
        if (e.value == v) return Tr(e.name);
    return HexName(v);
}

template <size_t N>
bool ParseNamed(const NamedValue (&t)[N], const std::string& text, uint32_t& out) {
    for (auto& e : t)
        if (strcasecmp(Tr(e.name, Lang::En), text.c_str()) == 0) { out = e.value; return true; }
    return false;
}

uint32_t Word(const std::vector<uint8_t>& r, size_t i) {
    return uint32_t(r[i * 4]) | uint32_t(r[i * 4 + 1]) << 8 | uint32_t(r[i * 4 + 2]) << 16 | uint32_t(r[i * 4 + 3]) << 24;
}

}  // namespace

std::string HexName(uint32_t v) {
    char b[32];
    snprintf(b, sizeof b, "0x%X", v);
    return b;
}

std::string IsoName(uint32_t v) {
    for (auto& e : kIsoNames)
        if (e.value == v) return e.name;
    return HexName(v);
}
std::string WbName(uint32_t v) { return NameOf(kWbNames, v); }
std::string StyleName(uint32_t v) { return NameOf(kStyleNames, v & 0xFF); }
std::string AloName(uint32_t v) { return NameOf(kAloNames, v); }

std::string ExpCompText(uint32_t raw) {
    int n = int(int8_t(raw & 0xFF));
    int thirds = int(std::lround(n * 3.0 / 8.0));
    int a = std::abs(thirds);
    char b[48];
    if (thirds == 0) return "0";
    const char* sign = thirds < 0 ? "−" : "+";
    if (a % 3 == 0) snprintf(b, sizeof b, "%s%d EV", sign, a / 3);
    else if (a < 3) snprintf(b, sizeof b, "%s%d/3 EV", sign, a % 3);
    else snprintf(b, sizeof b, "%s%d %d/3 EV", sign, a / 3, a % 3);
    return b;
}

std::string PropName(uint32_t prop) {
    switch (prop) {
        case kPropISO: return "ISO";
        case kPropExpComp: return "Exposure compensation";
        case kPropWhiteBalance: return "White balance";
        case kPropColorTemp: return "Color temperature";
        case kPropPictureStyle: return "Picture Style";
        case kPropAlo: return "Auto Lighting Optimizer";
        case 0xD1B0: return "EVF output device";
        case 0xD1B3: return "EVF mode";
    }
    if (prop >= 0xD150 && prop <= 0xD155) return "Picture Style parameters";
    if (prop >= 0xD160 && prop <= 0xD162) return "Picture Style parameters (user)";
    return "";
}

std::string PropValueText(uint32_t prop, uint32_t value) {
    switch (prop) {
        case kPropISO: return IsoName(value);
        case kPropExpComp: return ExpCompText(value);
        case kPropWhiteBalance: return WbName(value);
        case kPropColorTemp: return std::to_string(value) + " K";
        case kPropPictureStyle: return StyleName(value);
        case kPropAlo: return value < 0x100 ? AloName(value) : HexName(value);
    }
    return HexName(value);
}

uint32_t StylePropFor(uint32_t style) {
    switch (style & 0xFF) {
        case 0x81: return 0xD150;
        case 0x82: return 0xD151;
        case 0x83: return 0xD152;
        case 0x84: return 0xD153;
        case 0x85: return 0xD154;
        case 0x86: return 0xD155;
        case 0x21: return 0xD160;
        case 0x22: return 0xD161;
        case 0x23: return 0xD162;
    }
    return 0;
}

bool StyleParamWords(const std::vector<uint8_t>& raw, std::vector<uint32_t>& words, size_t& base) {
    if (raw.size() < 28) return false;
    size_t n = raw.size() / 4;
    words.resize(n);
    for (size_t i = 0; i < n; i++) words[i] = Word(raw, i);
    base = (raw.size() == 32) ? 2 : 1;  // user styles have one extra word (the base style)
    return base + 4 <= n;
}

bool GetStyleParam(const std::vector<uint8_t>& raw, int index, int& value) {
    std::vector<uint32_t> w;
    size_t base = 0;
    if (index < 0 || index > 3 || !StyleParamWords(raw, w, base)) return false;
    value = int(int32_t(w[base + size_t(index)]));
    return true;
}

void StyleParamRange(int index, int& lo, int& hi) {
    lo = index == 1 ? 0 : -4;
    hi = index == 1 ? 7 : 4;
}

bool ParseCamValue(const std::string& key, const std::string& text, uint32_t& out) {
    if (text.empty()) return false;
    if (key == "cam.iso" && text.compare(0, 2, "0x") != 0 && text.compare(0, 2, "0X") != 0) {
        for (auto& e : kIsoNames)  // "100" means ISO 100 (raw 0x48), not the raw value 100
            if (strcasecmp(e.name, text.c_str()) == 0) { out = e.value == 0 ? 1 : e.value; return true; }
        return false;
    }
    char* end = nullptr;
    long v = strtol(text.c_str(), &end, 0);
    if (end && *end == 0 && end != text.c_str()) {
        out = uint32_t(v);
        return true;
    }
    if (key == "cam.wb") return ParseNamed(kWbNames, text, out);
    if (key == "cam.style") return ParseNamed(kStyleNames, text, out);
    if (key == "cam.alo") return ParseNamed(kAloNames, text, out);
    return false;
}

}  // namespace canon_eos_webcam
