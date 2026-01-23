// =========================================================================
// Legacy Texture Processor - Format Detection & Shared Utilities
// =========================================================================

#ifdef _WIN64

#include "legacy_texture_processor_formats.h"
#include <tier0/dbg.h>
#include <algorithm>
#include <cctype>

namespace LegacyTextureProcessor {

// =========================================================================
// VMTParseResult Helper
// =========================================================================

std::string VMTParseResult::findValue(const std::string& key) const {
    std::string keyLower = key;
    std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);
    
    size_t pos = contentLower.find(keyLower);
    if (pos == std::string::npos) return "";
    
    // Move past the key
    pos += key.length();
    
    // Skip any whitespace and closing quote of the key (if key was quoted)
    while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '"' || content[pos] == '\'')) {
        pos++;
    }
    
    // Now we should be at the start of the value
    // Check if value is quoted
    if (pos < content.length() && (content[pos] == '"' || content[pos] == '\'')) {
        // Quoted value
        char quote = content[pos];
        size_t start = pos + 1;
        size_t end = content.find(quote, start);
        if (end == std::string::npos) return "";
        return content.substr(start, end - start);
    } else {
        // Unquoted value - read until whitespace or newline
        size_t start = pos;
        while (pos < content.length() && content[pos] != ' ' && content[pos] != '\t' && 
               content[pos] != '\r' && content[pos] != '\n' && content[pos] != '"' && content[pos] != '\'') {
            pos++;
        }
        return content.substr(start, pos - start);
    }
}

// =========================================================================
// Format Detection
// =========================================================================

namespace FormatHandler {

Format DetectFormat(const VMTParseResult& vmt) {
    // Check formats in priority order
    if (ExoPBR::Detect(vmt)) return Format::ExoPBR;
    if (GPBR::Detect(vmt)) return Format::GPBR;
    // MWB PBR must be checked BEFORE BFT - it has more specific patterns
    if (MWBPBR::Detect(vmt)) return Format::MWBPBR;
    if (BFTPseudoPBR::Detect(vmt)) return Format::BFTPseudoPBR;
    
    // SourceEngine is the fallback
    return Format::SourceEngine;
}

const char* GetFormatName(Format format) {
    switch (format) {
        case Format::ExoPBR: return "ExoPBR";
        case Format::GPBR: return "GPBR";
        case Format::MWBPBR: return "MWB-PBR";
        case Format::BFTPseudoPBR: return "BFT-PseudoPBR";
        case Format::SourceEngine: return "Source Engine";
        default: return "Unknown";
    }
}

} // namespace FormatHandler

// =========================================================================
// Shared Parsing Utilities
// =========================================================================

// Parse "[x y z]" or "{x y z}" format vectors
bool ParseVector3(const std::string& str, float& r, float& g, float& b) {
    if (str.empty()) return false;
    if (sscanf(str.c_str(), "[%f %f %f]", &r, &g, &b) == 3) return true;
    if (sscanf(str.c_str(), "{%f %f %f}", &r, &g, &b) == 3) return true;
    if (sscanf(str.c_str(), "%f %f %f", &r, &g, &b) == 3) return true;
    return false;
}

// Check approximate float equality
bool ApproxEqual(float a, float b, float epsilon = 0.1f) {
    float diff = a - b;
    if (diff < 0) diff = -diff;
    return diff < epsilon;
}

} // namespace LegacyTextureProcessor

#endif // _WIN64
