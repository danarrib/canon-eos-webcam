// Names and encodings of the Canon camera properties (shared by the service, the panel and the `cam` command).
// Copied from src/app/panel.cpp and src/app/canon_camera.h.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace canon_eos_webcam {

enum : uint32_t {
    kPropISO = 0xD103,
    kPropExpComp = 0xD104,
    kPropWhiteBalance = 0xD109,
    kPropColorTemp = 0xD10A,
    kPropPictureStyle = 0xD110,
    kPropAlo = 0xD1C1,
};

std::string IsoName(uint32_t v);
std::string WbName(uint32_t v);
std::string StyleName(uint32_t v);
std::string AloName(uint32_t v);
// Exposure compensation: signed byte in 1/8 EV; the camera uses 3 and 5 for 1/3 and 2/3.
std::string ExpCompText(uint32_t raw);
std::string HexName(uint32_t v);
// Name of a property for diagnostics ("ISO", "White balance", ...), or "" if unknown.
std::string PropName(uint32_t prop);
// Human readable value of a property (uses the tables above; hex when unknown).
std::string PropValueText(uint32_t prop, uint32_t value);

// Parameter property of a Picture Style (0xD150...), 0 if the style has no known parameters.
uint32_t StylePropFor(uint32_t style);

// Picture Style parameter words (the raw bytes of the PropValueChanged, from offset 12).
// 28 bytes for the factory styles (parameters from word 1), 32 for the user styles (from word 2).
// Index: 0 contrast, 1 sharpness, 2 saturation, 3 color tone. Returns false if the raw data is too short.
bool StyleParamWords(const std::vector<uint8_t>& raw, std::vector<uint32_t>& words, size_t& base);
bool GetStyleParam(const std::vector<uint8_t>& raw, int index, int& value);
// Ranges of the Picture Style parameters.
void StyleParamRange(int index, int& lo, int& hi);

// Parses a value typed by the user: a number (decimal or 0x..) or, for the named properties, the English name
// (case-insensitive) such as "daylight" or "standard". False if it is neither.
bool ParseCamValue(const std::string& key, const std::string& text, uint32_t& out);

}  // namespace canon_eos_webcam
