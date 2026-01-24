#pragma once

#ifdef _WIN64

#include <string>
#include <cstdint>

// Forward declarations
class IFileSystem;

namespace LegacyTextureProcessor {

// VMT file properties parsed from file
// This is used to extract material properties that are not reliably available
// through the Source Engine's IMaterial interface (especially with DX6 fallback shaders)
struct VMTProperties {
    std::string shaderName;
    bool hasRefractAmount;
    float refractAmount;
    bool hasTranslucent;
    bool translucent;
    std::string surfaceProp;
    bool hasEnvMap;
    std::string envMap;
    std::string refractTintTexture;  // $refracttinttexture - color texture for Refract shader
    bool hasRefractTintTexture;
    
    // Extended properties - extracted directly from VMT file to bypass DX6 shader limitations
    std::string baseTexture;
    bool hasBaseTexture;
    std::string bumpMap;
    bool hasBumpMap;
    std::string normalMap;
    bool hasNormalMap;
    std::string envMapMask;
    bool hasEnvMapMask;
    std::string phongExponentTexture;
    bool hasPhongExponentTexture;
    
    bool hasPhong;
    int phong;
    bool hasPhongExponent;
    float phongExponent;
    bool hasPhongBoost;
    float phongBoost;
    
    bool hasSSBump;
    int ssbump;
    
    bool hasNormalMapAlphaEnvMapMask;
    int normalMapAlphaEnvMapMask;
    bool hasBaseMapAlphaPhongMask;
    int baseMapAlphaPhongMask;
    bool hasBaseAlphaEnvMapMask;
    int baseAlphaEnvMapMask;
    
    bool hasEnvMapTint;
    float envMapTint[3];
    bool hasPhongFresnelRanges;
    float phongFresnelRanges[3];
    
    bool hasSelfIllum;
    int selfIllum;
    
    // =========================================================================
    // Additional properties for comprehensive PBR extraction
    // =========================================================================
    
    // Self-illumination / Emissive
    std::string selfIllumMask;      // $selfillummask - separate emissive mask texture
    bool hasSelfIllumMask;
    float selfIllumTint[3];         // $selfillumtint - tint color for self-illumination
    bool hasSelfIllumTint;
    
    // Rim lighting (affects specular)
    bool hasRimLight;
    int rimLight;
    float rimLightExponent;
    bool hasRimLightExponent;
    float rimLightBoost;
    bool hasRimLightBoost;
    
    // Additional phong properties
    bool hasPhongAlbedoTint;
    int phongAlbedoTint;
    float phongAlbedoBoost;
    bool hasPhongAlbedoBoost;
    float phongTint[3];
    bool hasPhongTint;
    
    // Parallax/heightmap
    std::string parallaxMap;        // $parallaxmap - heightmap for parallax
    bool hasParallaxMap;
    float parallaxMapScale;
    bool hasParallaxMapScale;
    
    // Additional envmap properties  
    float envMapContrast;
    bool hasEnvMapContrast;
    float envMapSaturation;
    bool hasEnvMapSaturation;
    
    // =========================================================================
    // ExoPBR community PBR format support (screenspace_general_8tex shader)
    // =========================================================================
    bool isExoPBR;                  // Detected ExoPBR format (shader + proxy)
    std::string texture1;           // $texture1 - ARM map (AO/Roughness/Metallic), alpha=height
    bool hasTexture1;
    std::string texture2;           // $texture2 - Normal map (DirectX Y- format)
    bool hasTexture2;
    std::string texture3;           // $texture3 - Emission texture
    bool hasTexture3;
    float emissionScale;            // $emissionscale - emission intensity
    bool hasEmissionScale;
    float emissionTint[3];          // $emissiontint - emission color tint
    bool hasEmissionTint;
    
    // =========================================================================
    // GPBR (Strata Source) community PBR format support ("PBR" shader)
    // =========================================================================
    bool isGPBR;                    // Detected GPBR format (shader name = "PBR")
    std::string mraoTexture;        // $mraotexture - MRAO map (Metallic/Roughness/AO)
    bool hasMRAOTexture;
    float mraoScale;                // $mraoscale - MRAO intensity multiplier
    bool hasMRAOScale;
    std::string gpbrEmissionTexture; // $emissiontexture - Emission/glow map
    bool hasGPBREmissionTexture;
    float gpbrEmissionScale;        // $emissionscale - Emission intensity (different from ExoPBR)
    bool hasGPBREmissionScale;
    bool gpbrParallax;              // $parallax - Enable parallax mapping (height in normal alpha)
    bool hasGPBRParallax;
    float gpbrParallaxDepth;        // $parallaxdepth - Displacement depth
    bool hasGPBRParallaxDepth;
    float gpbrParallaxCenter;       // $parallaxcenter - Parallax center point
    bool hasGPBRParallaxCenter;
    float gpbrAlpha;                // $alpha - Transparency value
    bool hasGPBRAlpha;
    
    // =========================================================================
    // BlueFlyTrap PseudoPBR format support
    // =========================================================================
    bool isBFTPseudoPBR;            // Detected BlueFlyTrap PseudoPBR format
    bool isBFTMetallicLayer;        // This is the metallic layer
    bool isBFTDiffuseLayer;         // This is the diffuse layer using $blendTintByBaseAlpha
    
    // =========================================================================
    // MWB PBR Gen format support
    // =========================================================================
    bool isMWBPBR;                  // Detected MWB PBR Gen format
};

// VMT Parser namespace - handles parsing of Source Engine VMT material files
namespace VMTParser {

// Parse a VMT file and extract properties
// This is crucial because when running with DX6 fallback shaders, FindVar() returns
// incorrect or missing values for many material properties
//
// Parameters:
//   fileSystem - Source Engine filesystem interface
//   materialName - Material name (e.g., "models/props/metal_door")
//   outProps - Output structure to receive parsed properties
//   debugOutput - Whether to print debug messages
//   parseCommented - Whether to parse commented-out properties (for maps where properties
//                    were commented for vanilla Source performance but benefit RTX Remix)
//
// Returns: true if VMT file was successfully parsed, false otherwise
bool ParseVMTFile(IFileSystem* fileSystem, 
                  const std::string& materialName, 
                  VMTProperties& outProps, 
                  bool debugOutput,
                  bool parseCommented);

} // namespace VMTParser

} // namespace LegacyTextureProcessor

#endif // _WIN64
