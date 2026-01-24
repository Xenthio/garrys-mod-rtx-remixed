#ifdef _WIN64

#include "legacy_texture_processor_vmt.h"
#include "legacy_texture_processor_formats.h"
#include <tier0/dbg.h>
#include <filesystem.h>
#include <algorithm>
#include <vector>
#include <cstring>

namespace LegacyTextureProcessor {
namespace VMTParser {

// Parse a VMT file and extract properties
bool ParseVMTFile(IFileSystem* fileSystem, 
                  const std::string& materialName, 
                  VMTProperties& outProps, 
                  bool debugOutput,
                  bool parseCommented) {
    if (!fileSystem) return false;
    
    // Initialize all properties to defaults
    outProps = VMTProperties{};
    outProps.hasRefractAmount = false;
    outProps.refractAmount = 0.0f;
    outProps.hasTranslucent = false;
    outProps.translucent = false;
    outProps.hasEnvMap = false;
    outProps.hasRefractTintTexture = false;
    
    // Extended properties
    outProps.hasBaseTexture = false;
    outProps.hasBumpMap = false;
    outProps.hasNormalMap = false;
    outProps.hasEnvMapMask = false;
    outProps.hasPhongExponentTexture = false;
    outProps.hasPhong = false;
    outProps.phong = 0;
    outProps.hasPhongExponent = false;
    outProps.phongExponent = 0.0f;
    outProps.hasPhongBoost = false;
    outProps.phongBoost = 1.0f;
    outProps.hasSSBump = false;
    outProps.ssbump = 0;
    outProps.hasNormalMapAlphaEnvMapMask = false;
    outProps.normalMapAlphaEnvMapMask = 0;
    outProps.hasBaseMapAlphaPhongMask = false;
    outProps.baseMapAlphaPhongMask = 0;
    outProps.hasBaseAlphaEnvMapMask = false;
    outProps.baseAlphaEnvMapMask = 0;
    outProps.hasEnvMapTint = false;
    outProps.envMapTint[0] = outProps.envMapTint[1] = outProps.envMapTint[2] = 1.0f;
    outProps.hasPhongFresnelRanges = false;
    outProps.phongFresnelRanges[0] = outProps.phongFresnelRanges[1] = outProps.phongFresnelRanges[2] = 0.0f;
    outProps.hasSelfIllum = false;
    outProps.selfIllum = 0;
    
    // Initialize additional properties
    outProps.hasSelfIllumMask = false;
    outProps.hasSelfIllumTint = false;
    outProps.selfIllumTint[0] = outProps.selfIllumTint[1] = outProps.selfIllumTint[2] = 1.0f;
    outProps.hasRimLight = false;
    outProps.rimLight = 0;
    outProps.hasRimLightExponent = false;
    outProps.rimLightExponent = 4.0f;
    outProps.hasRimLightBoost = false;
    outProps.rimLightBoost = 1.0f;
    outProps.hasPhongAlbedoTint = false;
    outProps.phongAlbedoTint = 0;
    outProps.hasPhongAlbedoBoost = false;
    outProps.phongAlbedoBoost = 1.0f;
    outProps.hasPhongTint = false;
    outProps.phongTint[0] = outProps.phongTint[1] = outProps.phongTint[2] = 1.0f;
    outProps.hasParallaxMap = false;
    outProps.hasParallaxMapScale = false;
    outProps.parallaxMapScale = 0.05f;
    outProps.hasEnvMapContrast = false;
    outProps.envMapContrast = 0.0f;
    outProps.hasEnvMapSaturation = false;
    outProps.envMapSaturation = 1.0f;
    
    // ExoPBR format properties
    outProps.isExoPBR = false;
    outProps.hasTexture1 = false;
    outProps.hasTexture2 = false;
    outProps.hasTexture3 = false;
    outProps.hasEmissionScale = false;
    outProps.emissionScale = 1.0f;
    outProps.hasEmissionTint = false;
    outProps.emissionTint[0] = outProps.emissionTint[1] = outProps.emissionTint[2] = 1.0f;
    
    // GPBR (Strata Source) format properties
    outProps.isGPBR = false;
    outProps.hasMRAOTexture = false;
    outProps.hasMRAOScale = false;
    outProps.mraoScale = 1.0f;
    outProps.hasGPBREmissionTexture = false;
    outProps.hasGPBREmissionScale = false;
    outProps.gpbrEmissionScale = 1.0f;
    outProps.hasGPBRParallax = false;
    outProps.gpbrParallax = false;
    outProps.hasGPBRParallaxDepth = false;
    outProps.gpbrParallaxDepth = 0.1f;
    outProps.hasGPBRParallaxCenter = false;
    outProps.gpbrParallaxCenter = 0.5f;
    outProps.hasGPBRAlpha = false;
    outProps.gpbrAlpha = 1.0f;
    
    // BlueFlyTrap PseudoPBR format properties
    outProps.isBFTPseudoPBR = false;
    outProps.isBFTMetallicLayer = false;
    outProps.isBFTDiffuseLayer = false;
    
    // MWB PBR Gen format properties
    outProps.isMWBPBR = false;
    
    // Build VMT path
    std::string vmtPath = "materials/" + materialName;
    if (vmtPath.find(".vmt") == std::string::npos) {
        vmtPath += ".vmt";
    }
    
    FileHandle_t file = fileSystem->Open(vmtPath.c_str(), "rb", "GAME");
    if (!file) {
        // Try without materials/ prefix
        vmtPath = materialName;
        if (vmtPath.find(".vmt") == std::string::npos) {
            vmtPath += ".vmt";
        }
        file = fileSystem->Open(vmtPath.c_str(), "rb", "GAME");
        if (!file) {
            return false;
        }
    }
    
    // Get file size
    int fileSize = fileSystem->Size(file);
    if (fileSize <= 0 || fileSize > 64 * 1024) {  // Max 64KB VMT
        fileSystem->Close(file);
        return false;
    }
    
    // Read file content
    std::vector<char> buffer(fileSize + 1);
    int bytesRead = fileSystem->Read(buffer.data(), fileSize, file);
    fileSystem->Close(file);
    
    if (bytesRead != fileSize) {
        return false;
    }
    buffer[fileSize] = '\0';
    
    // Parse the VMT content - REMOVE COMMENTS FIRST (unless parseCommented enabled)
    std::string content(buffer.data());
    
    // Strip out commented lines (// style) to prevent parsing commented-out properties
    // UNLESS the user has enabled parsing of commented properties (for maps where
    // envmap/masks were disabled for vanilla Source performance but benefit RTX Remix)
    std::string contentWithoutComments;
    if (!parseCommented) {
        size_t lineStart = 0;
        for (size_t i = 0; i <= content.size(); ++i) {
            if (i == content.size() || content[i] == '\n' || content[i] == '\r') {
                if (i > lineStart) {
                    std::string line = content.substr(lineStart, i - lineStart);
                    
                    // Check if line starts with // (after trimming whitespace)
                    size_t firstChar = line.find_first_not_of(" \t");
                    bool isComment = false;
                    if (firstChar != std::string::npos && firstChar + 1 < line.size()) {
                        if (line[firstChar] == '/' && line[firstChar + 1] == '/') {
                            isComment = true;
                        }
                    }
                    
                    // If not a comment line, keep it
                    if (!isComment) {
                        contentWithoutComments += line;
                        if (i < content.size()) {
                            contentWithoutComments += content[i];  // Preserve newline
                        }
                    }
                }
                lineStart = i + 1;
            }
        }
        
        // Use the comment-free content for parsing
        content = contentWithoutComments;
    }
    // else: keep all content including commented lines
    
    // Convert to lowercase for case-insensitive matching
    std::string contentLower = content;
    std::transform(contentLower.begin(), contentLower.end(), contentLower.begin(), ::tolower);
    
    // Extract shader name (first non-whitespace word, possibly in quotes)
    // Skip any comment lines (// or /* */) at the start
    size_t start = 0;
    while (start < content.length()) {
        // Skip whitespace
        start = content.find_first_not_of(" \t\r\n", start);
        if (start == std::string::npos) break;
        
        // Skip single-line comments
        if (start + 1 < content.length() && content[start] == '/' && content[start + 1] == '/') {
            // Find end of line
            size_t eol = content.find('\n', start);
            if (eol == std::string::npos) break;
            start = eol + 1;
            continue;
        }
        
        // Skip multi-line comments
        if (start + 1 < content.length() && content[start] == '/' && content[start + 1] == '*') {
            size_t endComment = content.find("*/", start + 2);
            if (endComment == std::string::npos) break;
            start = endComment + 2;
            continue;
        }
        
        // Found actual content
        break;
    }
    
    if (start != std::string::npos && start < content.length()) {
        // Skip quotes if present
        if (content[start] == '"') {
            start++;
            size_t end = content.find('"', start);
            if (end != std::string::npos) {
                outProps.shaderName = content.substr(start, end - start);
            }
        } else {
            // Find end of shader name (whitespace or brace)
            size_t end = content.find_first_of(" \t\r\n{", start);
            if (end != std::string::npos) {
                outProps.shaderName = content.substr(start, end - start);
            }
        }
    }
    
    // Helper to find a key-value pair (case-insensitive key)
    auto findValue = [&contentLower, &content](const std::string& keyLower) -> std::string {
        size_t pos = contentLower.find(keyLower);
        if (pos == std::string::npos) return "";
        
        // Find the value after the key
        pos += keyLower.length();
        // Skip whitespace
        while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '"')) {
            pos++;
        }
        
        // Read value until whitespace, quote, or newline
        size_t valueStart = pos;
        while (pos < content.length() && content[pos] != '"' && content[pos] != '\r' && content[pos] != '\n' && content[pos] != ' ' && content[pos] != '\t') {
            pos++;
        }
        
        return content.substr(valueStart, pos - valueStart);
    };
    
    // Helper to parse a float value
    auto parseFloat = [](const std::string& str, float defaultVal) -> float {
        if (str.empty()) return defaultVal;
        try {
            return std::stof(str);
        } catch (...) {
            return defaultVal;
        }
    };
    
    // Helper to parse a vector of 3 floats (e.g., "[1 0.5 0.25]")
    auto parseVector3 = [&content, &contentLower](const std::string& keyLower, float outVec[3]) -> bool {
        size_t pos = contentLower.find(keyLower);
        if (pos == std::string::npos) return false;
        
        pos += keyLower.length();
        size_t bracketStart = content.find('[', pos);
        if (bracketStart == std::string::npos || bracketStart - pos > 20) return false;
        
        size_t bracketEnd = content.find(']', bracketStart);
        if (bracketEnd == std::string::npos) return false;
        
        std::string vecStr = content.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
        
        // Parse 3 floats from the string
        float v[3] = {1, 1, 1};
        int count = sscanf(vecStr.c_str(), "%f %f %f", &v[0], &v[1], &v[2]);
        if (count == 3) {
            outVec[0] = v[0];
            outVec[1] = v[1];
            outVec[2] = v[2];
            return true;
        }
        return false;
    };
    
    // Check for $refractamount
    size_t refractPos = contentLower.find("$refractamount");
    if (refractPos != std::string::npos) {
        outProps.hasRefractAmount = true;
        std::string valStr = findValue("$refractamount");
        outProps.refractAmount = parseFloat(valStr, 0.25f);
    }
    
    // Check for $translucent
    size_t translucentPos = contentLower.find("$translucent");
    if (translucentPos != std::string::npos) {
        outProps.hasTranslucent = true;
        std::string valStr = findValue("$translucent");
        outProps.translucent = (valStr == "1" || valStr == "true");
    }
    
    // Check for $surfaceprop
    size_t surfacePos = contentLower.find("$surfaceprop");
    if (surfacePos != std::string::npos) {
        outProps.surfaceProp = findValue("$surfaceprop");
    }
    
    // Check for $envmap
    size_t envmapPos = contentLower.find("$envmap");
    if (envmapPos != std::string::npos) {
        outProps.hasEnvMap = true;
        outProps.envMap = findValue("$envmap");
    }
    
    // Check for $refracttinttexture - the actual color texture for Refract shader
    size_t refractTintPos = contentLower.find("$refracttinttexture");
    if (refractTintPos != std::string::npos) {
        outProps.hasRefractTintTexture = true;
        outProps.refractTintTexture = findValue("$refracttinttexture");
    }
    
    // =========================================================================
    // Extended VMT property extraction to bypass DX6 shader FindVar limitations
    // =========================================================================
    
    // $basetexture
    size_t baseTexPos = contentLower.find("$basetexture");
    if (baseTexPos != std::string::npos) {
        outProps.baseTexture = findValue("$basetexture");
        outProps.hasBaseTexture = !outProps.baseTexture.empty();
    }
    
    // $bumpmap
    size_t bumpPos = contentLower.find("$bumpmap");
    if (bumpPos != std::string::npos) {
        outProps.bumpMap = findValue("$bumpmap");
        outProps.hasBumpMap = !outProps.bumpMap.empty();
    }
    
    // $normalmap (alternative to $bumpmap)
    size_t normalPos = contentLower.find("$normalmap");
    if (normalPos != std::string::npos) {
        outProps.normalMap = findValue("$normalmap");
        outProps.hasNormalMap = !outProps.normalMap.empty();
    }
    
    // $envmapmask
    size_t envmaskPos = contentLower.find("$envmapmask");
    if (envmaskPos != std::string::npos) {
        outProps.envMapMask = findValue("$envmapmask");
        outProps.hasEnvMapMask = !outProps.envMapMask.empty();
    }
    
    // $phongexponenttexture
    size_t phongExpTexPos = contentLower.find("$phongexponenttexture");
    if (phongExpTexPos != std::string::npos) {
        outProps.phongExponentTexture = findValue("$phongexponenttexture");
        outProps.hasPhongExponentTexture = !outProps.phongExponentTexture.empty();
    }
    
    // $phong
    size_t phongPos = contentLower.find("$phong");
    if (phongPos != std::string::npos && phongPos == contentLower.find("$phong")) {  // Not $phongexponent
        std::string valStr = findValue("$phong");
        outProps.hasPhong = !valStr.empty();
        outProps.phong = (valStr == "1" || valStr == "true") ? 1 : 0;
    }
    
    // $phongexponent
    size_t phongExpPos = contentLower.find("$phongexponent");
    if (phongExpPos != std::string::npos) {
        std::string valStr = findValue("$phongexponent");
        if (!valStr.empty()) {
            outProps.hasPhongExponent = true;
            outProps.phongExponent = parseFloat(valStr, 0.0f);
        }
    }
    
    // $phongboost
    size_t phongBoostPos = contentLower.find("$phongboost");
    if (phongBoostPos != std::string::npos) {
        std::string valStr = findValue("$phongboost");
        if (!valStr.empty()) {
            outProps.hasPhongBoost = true;
            outProps.phongBoost = parseFloat(valStr, 1.0f);
        }
    }
    
    // $ssbump
    size_t ssbumpPos = contentLower.find("$ssbump");
    if (ssbumpPos != std::string::npos) {
        std::string valStr = findValue("$ssbump");
        outProps.hasSSBump = !valStr.empty();
        outProps.ssbump = (valStr == "1" || valStr == "true") ? 1 : 0;
    }
    
    // $normalmapalphaenvmapmask
    size_t normalAlphaEnvPos = contentLower.find("$normalmapalphaenvmapmask");
    if (normalAlphaEnvPos != std::string::npos) {
        std::string valStr = findValue("$normalmapalphaenvmapmask");
        outProps.hasNormalMapAlphaEnvMapMask = !valStr.empty();
        outProps.normalMapAlphaEnvMapMask = (valStr == "1" || valStr == "true") ? 1 : 0;
    }
    
    // $basemapalphaphongmask
    size_t baseAlphaPhongPos = contentLower.find("$basemapalphaphongmask");
    if (baseAlphaPhongPos != std::string::npos) {
        std::string valStr = findValue("$basemapalphaphongmask");
        outProps.hasBaseMapAlphaPhongMask = !valStr.empty();
        outProps.baseMapAlphaPhongMask = (valStr == "1" || valStr == "true") ? 1 : 0;
    }
    
    // $basealphaenvmapmask
    size_t baseAlphaEnvPos = contentLower.find("$basealphaenvmapmask");
    if (baseAlphaEnvPos != std::string::npos) {
        std::string valStr = findValue("$basealphaenvmapmask");
        outProps.hasBaseAlphaEnvMapMask = !valStr.empty();
        outProps.baseAlphaEnvMapMask = (valStr == "1" || valStr == "true") ? 1 : 0;
    }
    
    // $envmaptint
    if (parseVector3("$envmaptint", outProps.envMapTint)) {
        outProps.hasEnvMapTint = true;
    }
    
    // $phongfresnelranges
    if (parseVector3("$phongfresnelranges", outProps.phongFresnelRanges)) {
        outProps.hasPhongFresnelRanges = true;
    }
    
    // $selfillum
    size_t selfIllumPos = contentLower.find("$selfillum");
    if (selfIllumPos != std::string::npos) {
        std::string valStr = findValue("$selfillum");
        outProps.hasSelfIllum = !valStr.empty();
        outProps.selfIllum = (valStr == "1" || valStr == "true") ? 1 : 0;
    }
    
    // Additional properties for comprehensive PBR extraction
    
    // $selfillummask
    size_t selfIllumMaskPos = contentLower.find("$selfillummask");
    if (selfIllumMaskPos != std::string::npos) {
        outProps.selfIllumMask = findValue("$selfillummask");
        outProps.hasSelfIllumMask = !outProps.selfIllumMask.empty();
    }
    
    // $selfillumtint
    if (parseVector3("$selfillumtint", outProps.selfIllumTint)) {
        outProps.hasSelfIllumTint = true;
    }
    
    // $rimlight
    size_t rimLightPos = contentLower.find("$rimlight");
    if (rimLightPos != std::string::npos) {
        std::string valStr = findValue("$rimlight");
        outProps.hasRimLight = !valStr.empty();
        outProps.rimLight = (valStr == "1" || valStr == "true") ? 1 : 0;
    }
    
    // $rimlightexponent
    size_t rimExpPos = contentLower.find("$rimlightexponent");
    if (rimExpPos != std::string::npos) {
        std::string valStr = findValue("$rimlightexponent");
        if (!valStr.empty()) {
            outProps.hasRimLightExponent = true;
            outProps.rimLightExponent = parseFloat(valStr, 4.0f);
        }
    }
    
    // $rimlightboost
    size_t rimBoostPos = contentLower.find("$rimlightboost");
    if (rimBoostPos != std::string::npos) {
        std::string valStr = findValue("$rimlightboost");
        if (!valStr.empty()) {
            outProps.hasRimLightBoost = true;
            outProps.rimLightBoost = parseFloat(valStr, 1.0f);
        }
    }
    
    // $phongalbedotint
    size_t phongAlbedoTintPos = contentLower.find("$phongalbedotint");
    if (phongAlbedoTintPos != std::string::npos) {
        std::string valStr = findValue("$phongalbedotint");
        outProps.hasPhongAlbedoTint = !valStr.empty();
        outProps.phongAlbedoTint = (valStr == "1" || valStr == "true") ? 1 : 0;
    }
    
    // $phongalbedoboost
    size_t phongAlbedoBoostPos = contentLower.find("$phongalbedoboost");
    if (phongAlbedoBoostPos != std::string::npos) {
        std::string valStr = findValue("$phongalbedoboost");
        if (!valStr.empty()) {
            outProps.hasPhongAlbedoBoost = true;
            outProps.phongAlbedoBoost = parseFloat(valStr, 1.0f);
        }
    }
    
    // $phongtint
    if (parseVector3("$phongtint", outProps.phongTint)) {
        outProps.hasPhongTint = true;
    }
    
    // $parallaxmap
    size_t parallaxPos = contentLower.find("$parallaxmap");
    if (parallaxPos != std::string::npos) {
        outProps.parallaxMap = findValue("$parallaxmap");
        outProps.hasParallaxMap = !outProps.parallaxMap.empty();
    }
    
    // $parallaxmapscale
    size_t parallaxScalePos = contentLower.find("$parallaxmapscale");
    if (parallaxScalePos != std::string::npos) {
        std::string valStr = findValue("$parallaxmapscale");
        if (!valStr.empty()) {
            outProps.hasParallaxMapScale = true;
            outProps.parallaxMapScale = parseFloat(valStr, 0.05f);
        }
    }
    
    // $envmapcontrast
    size_t envContrastPos = contentLower.find("$envmapcontrast");
    if (envContrastPos != std::string::npos) {
        std::string valStr = findValue("$envmapcontrast");
        if (!valStr.empty()) {
            outProps.hasEnvMapContrast = true;
            outProps.envMapContrast = parseFloat(valStr, 0.0f);
        }
    }
    
    // $envmapsaturation
    size_t envSatPos = contentLower.find("$envmapsaturation");
    if (envSatPos != std::string::npos) {
        std::string valStr = findValue("$envmapsaturation");
        if (!valStr.empty()) {
            outProps.hasEnvMapSaturation = true;
            outProps.envMapSaturation = parseFloat(valStr, 1.0f);
        }
    }
    
    // =========================================================================
    // Community PBR Format Detection
    // =========================================================================
    
    // ExoPBR format detection (screenspace_general_8tex shader)
    if (contentLower.find("screenspace_general_8tex") != std::string::npos ||
        contentLower.find("exopbr") != std::string::npos) {
        outProps.isExoPBR = true;
        
        // $texture1 - ARM map
        size_t tex1Pos = contentLower.find("$texture1");
        if (tex1Pos != std::string::npos) {
            outProps.texture1 = findValue("$texture1");
            outProps.hasTexture1 = !outProps.texture1.empty();
        }
        
        // $texture2 - Normal map
        size_t tex2Pos = contentLower.find("$texture2");
        if (tex2Pos != std::string::npos) {
            outProps.texture2 = findValue("$texture2");
            outProps.hasTexture2 = !outProps.texture2.empty();
        }
        
        // $texture3 - Emission map
        size_t tex3Pos = contentLower.find("$texture3");
        if (tex3Pos != std::string::npos) {
            outProps.texture3 = findValue("$texture3");
            outProps.hasTexture3 = !outProps.texture3.empty();
        }
        
        // $emissionscale
        size_t emissionScalePos = contentLower.find("$emissionscale");
        if (emissionScalePos != std::string::npos) {
            std::string valStr = findValue("$emissionscale");
            if (!valStr.empty()) {
                outProps.hasEmissionScale = true;
                outProps.emissionScale = parseFloat(valStr, 1.0f);
            }
        }
        
        // $emissiontint
        if (parseVector3("$emissiontint", outProps.emissionTint)) {
            outProps.hasEmissionTint = true;
        }
    }
    
    // GPBR format detection (shader name = "PBR")
    std::string shaderLower = outProps.shaderName;
    std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(), ::tolower);
    if (shaderLower == "pbr") {
        outProps.isGPBR = true;
        
        // $mraotexture
        size_t mraoPos = contentLower.find("$mraotexture");
        if (mraoPos != std::string::npos) {
            outProps.mraoTexture = findValue("$mraotexture");
            outProps.hasMRAOTexture = !outProps.mraoTexture.empty();
        }
        
        // $mraoscale
        size_t mraoScalePos = contentLower.find("$mraoscale");
        if (mraoScalePos != std::string::npos) {
            std::string valStr = findValue("$mraoscale");
            if (!valStr.empty()) {
                outProps.hasMRAOScale = true;
                outProps.mraoScale = parseFloat(valStr, 1.0f);
            }
        }
        
        // $emissiontexture (GPBR version)
        size_t gpbrEmissionPos = contentLower.find("$emissiontexture");
        if (gpbrEmissionPos != std::string::npos) {
            outProps.gpbrEmissionTexture = findValue("$emissiontexture");
            outProps.hasGPBREmissionTexture = !outProps.gpbrEmissionTexture.empty();
        }
        
        // $emissionscale (GPBR version)
        size_t gpbrEmissionScalePos = contentLower.find("$emissionscale");
        if (gpbrEmissionScalePos != std::string::npos) {
            std::string valStr = findValue("$emissionscale");
            if (!valStr.empty()) {
                outProps.hasGPBREmissionScale = true;
                outProps.gpbrEmissionScale = parseFloat(valStr, 1.0f);
            }
        }
        
        // $parallax
        size_t gpbrParallaxPos = contentLower.find("$parallax");
        if (gpbrParallaxPos != std::string::npos) {
            std::string valStr = findValue("$parallax");
            outProps.hasGPBRParallax = !valStr.empty();
            outProps.gpbrParallax = (valStr == "1" || valStr == "true");
        }
        
        // $parallaxdepth
        size_t gpbrParallaxDepthPos = contentLower.find("$parallaxdepth");
        if (gpbrParallaxDepthPos != std::string::npos) {
            std::string valStr = findValue("$parallaxdepth");
            if (!valStr.empty()) {
                outProps.hasGPBRParallaxDepth = true;
                outProps.gpbrParallaxDepth = parseFloat(valStr, 0.1f);
            }
        }
        
        // $parallaxcenter
        size_t gpbrParallaxCenterPos = contentLower.find("$parallaxcenter");
        if (gpbrParallaxCenterPos != std::string::npos) {
            std::string valStr = findValue("$parallaxcenter");
            if (!valStr.empty()) {
                outProps.hasGPBRParallaxCenter = true;
                outProps.gpbrParallaxCenter = parseFloat(valStr, 0.5f);
            }
        }
        
        // $alpha
        size_t gpbrAlphaPos = contentLower.find("$alpha");
        if (gpbrAlphaPos != std::string::npos) {
            std::string valStr = findValue("$alpha");
            if (!valStr.empty()) {
                outProps.hasGPBRAlpha = true;
                outProps.gpbrAlpha = parseFloat(valStr, 1.0f);
            }
        }
    }
    
    // MWB PBR Gen format detection
    // Detection: Uses modular MWBPBR::Detect() function
    {
        // Create VMTParseResult for modular detection
        VMTParseResult vmtParse;
        vmtParse.shaderName = outProps.shaderName;
        vmtParse.content = content;
        vmtParse.contentLower = contentLower;
        
        if (MWBPBR::Detect(vmtParse)) {
            outProps.isMWBPBR = true;
        }
    }
    
    // BlueFlyTrap PseudoPBR format detection
    // Uses the modular BFTPseudoPBR::Detect() function for comprehensive detection
    // This checks for:
    // 1. VertexlitGeneric shader
    // 2. $phongexponenttexture present
    // 3. Specific patterns like $color2 darkening, $blendtintbybasealpha, $translucent
    {
        // Create VMTParseResult for modular detection
        VMTParseResult vmtParse;
        vmtParse.shaderName = outProps.shaderName;
        vmtParse.content = content;
        vmtParse.contentLower = contentLower;
        
        if (BFTPseudoPBR::Detect(vmtParse)) {
            outProps.isBFTPseudoPBR = true;
            // The metallic/diffuse layer detection is done within the format handler
        }
    }
    
    // Debug output
    if (debugOutput) {
        Msg("[LegacyTextureProcessor] VMT direct parse for '%s':\n", materialName.c_str());
        Msg("  shader='%s', $basetexture='%s', $bumpmap='%s'\n",
            outProps.shaderName.c_str(), outProps.baseTexture.c_str(), outProps.bumpMap.c_str());
        Msg("  $phong=%d, $phongexponent=%.1f, $ssbump=%d, $envmap=%d\n",
            outProps.phong, outProps.phongExponent, outProps.ssbump, outProps.hasEnvMap);
        
        if (outProps.hasEnvMapTint) {
            Msg("  $envmaptint=[%.2f %.2f %.2f]\n", outProps.envMapTint[0], outProps.envMapTint[1], outProps.envMapTint[2]);
        }
        
        if (outProps.hasSelfIllum && outProps.selfIllum) {
            Msg("  $selfillum=1");
            if (outProps.hasSelfIllumMask) Msg(", $selfillummask='%s'", outProps.selfIllumMask.c_str());
            if (outProps.hasSelfIllumTint) Msg(", $selfillumtint=[%.2f %.2f %.2f]", outProps.selfIllumTint[0], outProps.selfIllumTint[1], outProps.selfIllumTint[2]);
            Msg("\n");
        }
        if (outProps.hasRimLight && outProps.rimLight) {
            Msg("  $rimlight=1, exponent=%.1f, boost=%.1f\n", outProps.rimLightExponent, outProps.rimLightBoost);
        }
        if (outProps.hasParallaxMap) {
            Msg("  $parallaxmap='%s', scale=%.3f\n", outProps.parallaxMap.c_str(), outProps.parallaxMapScale);
        }
        if (outProps.hasEnvMapContrast || outProps.hasEnvMapSaturation) {
            Msg("  $envmapcontrast=%.1f, $envmapsaturation=%.1f\n", outProps.envMapContrast, outProps.envMapSaturation);
        }
        
        // Community PBR format debug output
        if (outProps.isExoPBR) {
            Msg("  [ExoPBR] Detected community PBR format!\n");
            if (outProps.hasTexture1) Msg("    $texture1 (ARM)='%s'\n", outProps.texture1.c_str());
            if (outProps.hasTexture2) Msg("    $texture2 (Normal)='%s'\n", outProps.texture2.c_str());
            if (outProps.hasTexture3) Msg("    $texture3 (Emission)='%s'\n", outProps.texture3.c_str());
            if (outProps.hasEmissionScale) Msg("    $emissionscale=%.2f\n", outProps.emissionScale);
            if (outProps.hasEmissionTint) Msg("    $emissiontint=[%.2f %.2f %.2f]\n", 
                outProps.emissionTint[0], outProps.emissionTint[1], outProps.emissionTint[2]);
        }
        
        if (outProps.isGPBR) {
            Msg("  [GPBR] Detected GPBR (Strata Source) format!\n");
            if (outProps.hasMRAOTexture) Msg("    $mraotexture='%s'\n", outProps.mraoTexture.c_str());
            if (outProps.hasMRAOScale) Msg("    $mraoscale=%.2f\n", outProps.mraoScale);
            if (outProps.hasGPBREmissionTexture) Msg("    $emissiontexture='%s'\n", outProps.gpbrEmissionTexture.c_str());
            if (outProps.hasGPBREmissionScale) Msg("    $emissionscale=%.2f\n", outProps.gpbrEmissionScale);
            if (outProps.hasGPBRParallax) Msg("    $parallax=%d, depth=%.2f\n", outProps.gpbrParallax, outProps.gpbrParallaxDepth);
        }
        
        if (outProps.isMWBPBR) {
            Msg("  [MWB-PBR] Detected MWB PBR Gen format!\n");
            Msg("    Uses pow(gloss,4.0) encoding in $phongexponenttexture\n");
            Msg("    Metallic stored in green channel\n");
            if (outProps.hasPhongExponentTexture) {
                Msg("    $phongexponenttexture='%s' (roughness: pow^0.25 decode, metallic: green channel)\n", outProps.phongExponentTexture.c_str());
            }
        }
        // BlueFlyTrap PseudoPBR specific logging
        if (outProps.isBFTPseudoPBR) {
            Msg("  [BFT-PseudoPBR] Detected BlueFlyTrap PseudoPBR format!\n");
            if (outProps.hasPhongExponentTexture) {
                Msg("    $phongexponenttexture='%s' (roughness: linear inversion)\n", outProps.phongExponentTexture.c_str());
            }
            Msg("    $phongboost=%.2f\n", outProps.phongBoost);
            if (outProps.hasPhongFresnelRanges) {
                Msg("    $phongfresnelranges=[%.2f %.2f %.2f]\n", 
                    outProps.phongFresnelRanges[0], outProps.phongFresnelRanges[1], outProps.phongFresnelRanges[2]);
            }
            Msg("    Layer type: %s\n", outProps.isBFTMetallicLayer ? "METALLIC" : "BASE/DIELECTRIC");
        }
    }
    
    return true;
}

} // namespace VMTParser
} // namespace LegacyTextureProcessor

#endif // _WIN64
