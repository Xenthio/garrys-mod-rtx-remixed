#ifdef _WIN64

#include "to_pbr.h"
#include "formats.h"
#include "vtf.h"
#include "usda.h"
#include "../material_filter.h"
#include <tier0/dbg.h>
#include <materialsystem/imaterialsystem.h>
#include <materialsystem/imaterial.h>
#include <materialsystem/imaterialvar.h>
#include <materialsystem/itexture.h>
#include <filesystem.h>
#include <d3d9.h>
#include <Windows.h>
#include "../../d3d9_texture_tracker.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <direct.h>  // For _mkdir on Windows
#include <cstdio>    // For std::rename, std::remove (staging file atomic rename)
#include <cstdlib>   // For std::strtoull (hash string parsing)

// External globals
extern IMaterialSystem* materials;
extern remix::Interface* g_remix;

namespace MaterialPipeline {
namespace ToPBR {

// PBR conversion constants
constexpr float MAX_PHONG_EXPONENT = 150.0f;  // Typical max in Source Engine

// DDS format constants
constexpr uint32_t DDS_MAGIC = 0x20534444;  // "DDS "
constexpr uint32_t DDSD_CAPS = 0x1;
constexpr uint32_t DDSD_HEIGHT = 0x2;
constexpr uint32_t DDSD_WIDTH = 0x4;
constexpr uint32_t DDSD_PIXELFORMAT = 0x1000;
constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000;
constexpr uint32_t DDSD_LINEARSIZE = 0x80000;
constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
constexpr uint32_t DDPF_RGB = 0x40;
constexpr uint32_t DDSCAPS_TEXTURE = 0x1000;
constexpr uint32_t DDSCAPS_MIPMAP = 0x400000;
constexpr uint32_t DDSCAPS_COMPLEX = 0x8;

// DDS header structures
#pragma pack(push, 1)
struct DDSPixelFormat {
    uint32_t size;
    uint32_t flags;
    uint32_t fourCC;
    uint32_t rgbBitCount;
    uint32_t rBitMask;
    uint32_t gBitMask;
    uint32_t bBitMask;
    uint32_t aBitMask;
};

struct DDSHeader {
    uint32_t magic;
    uint32_t size;
    uint32_t flags;
    uint32_t height;
    uint32_t width;
    uint32_t pitchOrLinearSize;
    uint32_t depth;
    uint32_t mipMapCount;
    uint32_t reserved1[11];
    DDSPixelFormat pixelFormat;
    uint32_t caps;
    uint32_t caps2;
    uint32_t caps3;
    uint32_t caps4;
    uint32_t reserved2;
};
#pragma pack(pop)

// Static filesystem pointer
static IFileSystem* s_pFileSystem = nullptr;

// Get filesystem interface
static IFileSystem* GetFileSystemInterface() {
    if (s_pFileSystem) return s_pFileSystem;
    
    HMODULE fsModule = GetModuleHandle("filesystem_stdio.dll");
    if (!fsModule) {
        fsModule = GetModuleHandle("filesystem.dll");
    }
    
    if (fsModule) {
        typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);
        CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(fsModule, "CreateInterface");
        if (createInterface) {
            s_pFileSystem = (IFileSystem*)createInterface("VFileSystem022", nullptr);
            if (!s_pFileSystem) {
                s_pFileSystem = (IFileSystem*)createInterface("VFileSystem021", nullptr);
            }
        }
    }
    
    return s_pFileSystem;
}

//=============================================================================
// TextureProcessor Implementation
//=============================================================================

TextureProcessor& TextureProcessor::Instance() {
    static TextureProcessor instance;
    return instance;
}

TextureProcessor::TextureProcessor()
    : m_remixInterface(nullptr)
    , m_fileSystem(nullptr)
    , m_initialized(false)
    , m_autoProcessing(true)
    , m_debugOutput(false)
    , m_metallicGenerationEnabled(false)  // Disabled by default - experimental feature
    , m_autoDiscoverEnabled(true)         // Enabled by default - helps find unreferenced textures
    , m_parseCommentedPropertiesEnabled(false)  // Disabled by default - respects VMT comments
    , m_needsUSDAUpdate(false)
    , m_lastKnownMaterialCount(0)
    , m_allMaterialsProcessed(false)
    , m_workerRunning(false)
    , m_shutdownRequested(false)
    , m_backgroundProcessing(false)
    , m_lastProcessedCount(0) {
    m_stats = {};
}

TextureProcessor::~TextureProcessor() {
    Shutdown();
}

bool TextureProcessor::Initialize(remix::Interface* remixInterface) {
    if (m_initialized) {
        // Already initialized - this is success, not failure
        return true;
    }
    
    if (!remixInterface) {
        Warning("[MaterialPipeline::ToPBR] Invalid Remix interface\n");
        return false;
    }
    
    m_remixInterface = remixInterface;
    m_fileSystem = GetFileSystemInterface();
    
    if (!m_fileSystem) {
        Warning("[MaterialPipeline::ToPBR] Could not get filesystem interface\n");
        return false;
    }
    
    // Set default output directory for generated textures
    // The executable is at bin/win64/gmod.exe, we need to go up to the game root
    // Output should be: GarrysModWithRTXAgain/rtx-remix/mods/~gmod_topbr/textures/
    char gamePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, gamePath, MAX_PATH)) {
        std::string gameDir(gamePath);
        // Remove executable name (gmod.exe)
        size_t lastSlash = gameDir.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            gameDir = gameDir.substr(0, lastSlash);
            // Remove "win64" directory
            lastSlash = gameDir.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                gameDir = gameDir.substr(0, lastSlash);
                // Remove "bin" directory to get to game root
                lastSlash = gameDir.find_last_of("\\/");
                if (lastSlash != std::string::npos) {
                    gameDir = gameDir.substr(0, lastSlash);
                }
            }
            m_outputDirectory = gameDir + "\\rtx-remix\\mods\\~gmod_topbr\\textures";
        }
    }
    
    // Load existing hashes from USDA file to skip already-processed materials.
    // If the generator version changed since the USDA was written, discard it -
    // stale entries would otherwise block regeneration of improved materials.
    if (!m_outputDirectory.empty()) {
        std::string modDir = USDA::GetModDirectory(m_outputDirectory);
        std::string materialsUsdaPath = modDir + "/materials.usda";
        std::string versionPath = modDir + "/topbr_generation.txt";
        
        int writtenVersion = 0;
        {
            std::ifstream versionFile(versionPath);
            if (versionFile.is_open()) {
                versionFile >> writtenVersion;
            }
        }
        
        if (writtenVersion != OUTPUT_GENERATION_VERSION) {
            if (std::remove(materialsUsdaPath.c_str()) == 0) {
                Msg("[MaterialPipeline::ToPBR] Output generation changed (%d -> %d) - discarded stale materials.usda for full regeneration\n",
                    writtenVersion, OUTPUT_GENERATION_VERSION);
            }
            EnsureOutputDirectory();
            std::ofstream versionFile(versionPath, std::ios::trunc);
            if (versionFile.is_open()) {
                versionFile << OUTPUT_GENERATION_VERSION << "\n";
            }
        } else {
            std::unordered_set<uint64_t> existingHashes;
            if (USDA::LoadExistingHashes(materialsUsdaPath, existingHashes, true)) {
                // Bulk insert existing hashes
                m_materialsWrittenToUSDA.insert(existingHashes.begin(), existingHashes.end());
                
                if (!existingHashes.empty()) {
                    Msg("[MaterialPipeline::ToPBR] Loaded %zu existing material hashes from USDA\n", 
                        existingHashes.size());
                }
            }
        }
    }
    
    // Load persistent ineligible cache (materials known to not qualify for PBR)
    LoadIneligibleCache();
    
    // Pre-populate m_processedMaterials from the ineligible cache so the in-memory
    // fast path (m_processedMaterials.find()) works immediately without re-evaluation
    if (!m_ineligibleCache.empty()) {
        m_processedMaterials.insert(m_ineligibleCache.begin(), m_ineligibleCache.end());
    }
    
    m_initialized = true;
    Msg("[MaterialPipeline::ToPBR] Initialized successfully\n");
    Msg("[MaterialPipeline::ToPBR] Output directory: %s\n", m_outputDirectory.c_str());
    return true;
}

void TextureProcessor::Shutdown() {
    if (!m_initialized) return;
    
    // Save persistent ineligible cache before shutdown (force bypass throttle)
    SaveIneligibleCache(true);
    
    // Stop background worker thread first
    StopWorkerThread();
    
    // Destroy all uploaded textures
    for (auto& pair : m_textureHandles) {
        if (m_remixInterface && pair.second) {
            m_remixInterface->DestroyTexture(pair.second);
        }
    }
    m_textureHandles.clear();
    
    m_uploadedTextures.clear();
    m_processedMaterials.clear();
    m_processedMaterialInfo.clear();
    m_writtenTexturePaths.clear();
    m_materialsWrittenToUSDA.clear();
    
    // Clear queue
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        while (!m_materialQueue.empty()) m_materialQueue.pop();
        m_queuedMaterials.clear();
    }
    
    // Clear pending USDA
    {
        std::lock_guard<std::mutex> lock(m_pendingUSDAMutex);
        m_pendingUSDAMaterials.clear();
    }
    
    m_remixInterface = nullptr;
    m_initialized = false;
    
    Msg("[MaterialPipeline::ToPBR] Shutdown complete\n");
}

// =========================================================================
// Persistent Ineligible Cache - avoids re-running ExtractMaterialPBR on
// materials that will never qualify for PBR (e.g. spawnmenu RT icons).
// =========================================================================

std::string TextureProcessor::GetIneligibleCachePath() const {
    if (m_outputDirectory.empty()) return "";
    std::string modDir = USDA::GetModDirectory(m_outputDirectory);
    return modDir + "/pbr_ineligible_cache.txt";
}

void TextureProcessor::LoadIneligibleCache() {
    std::string cachePath = GetIneligibleCachePath();
    if (cachePath.empty()) return;
    
    std::ifstream file(cachePath);
    if (!file.is_open()) return;
    
    std::string line;
    
    // First line must be version header
    if (!std::getline(file, line)) return;
    
    // Parse "CACHE_VERSION <N>"
    int version = 0;
    if (sscanf(line.c_str(), "CACHE_VERSION %d", &version) != 1 || 
        version != INELIGIBLE_CACHE_VERSION) {
        // Version mismatch - discard stale cache
        file.close();
        Msg("[MaterialPipeline::ToPBR] Ineligible cache version mismatch (got %d, expected %d) - rebuilding\n",
            version, INELIGIBLE_CACHE_VERSION);
        return;
    }
    
    // Read material names
    while (std::getline(file, line)) {
        if (!line.empty()) {
            m_ineligibleCache.insert(line);
        }
    }
    file.close();
    
    if (!m_ineligibleCache.empty()) {
        Msg("[MaterialPipeline::ToPBR] Loaded %zu cached PBR-ineligible materials\n", 
            m_ineligibleCache.size());
    }
}

void TextureProcessor::SaveIneligibleCache(bool force) {
    if (!m_ineligibleCacheDirty) return;
    
    // Throttle disk writes: at most once every 5 seconds during rapid discovery
    // Bypass throttle when force=true (e.g. during Shutdown)
    if (!force) {
        static auto lastSaveTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastSaveTime).count();
        if (elapsed < 5) return;
        lastSaveTime = now;
    }
    
    std::string cachePath = GetIneligibleCachePath();
    if (cachePath.empty()) return;
    
    // Ensure directory exists
    std::string modDir = USDA::GetModDirectory(m_outputDirectory);
    _mkdir(modDir.c_str());
    
    std::ofstream file(cachePath, std::ios::trunc);
    if (!file.is_open()) {
        Warning("[MaterialPipeline::ToPBR] Failed to write ineligible cache to %s\n", cachePath.c_str());
        return;
    }
    
    file << "CACHE_VERSION " << INELIGIBLE_CACHE_VERSION << "\n";
    
    for (const auto& name : m_ineligibleCache) {
        file << name << "\n";
    }
    file.close();
    
    m_ineligibleCacheDirty = false;
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Saved %zu PBR-ineligible materials to cache\n", 
            m_ineligibleCache.size());
    }
}

void TextureProcessor::SetOutputDirectory(const std::string& path) {
    m_outputDirectory = path;
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Output directory set to: %s\n", path.c_str());
    }
}

IFileSystem* TextureProcessor::GetFileSystem() {
    return m_fileSystem;
}

bool TextureProcessor::EnsureOutputDirectory() {
    if (m_outputDirectory.empty()) {
        Warning("[MaterialPipeline::ToPBR] Output directory not set\n");
        return false;
    }
    
    // Create directory hierarchy
    std::string path = m_outputDirectory;
    
    // Replace forward slashes with backslashes for Windows
    for (char& c : path) {
        if (c == '/') c = '\\';
    }
    
    // Create each directory level
    size_t pos = 0;
    while ((pos = path.find('\\', pos + 1)) != std::string::npos) {
        std::string subPath = path.substr(0, pos);
        _mkdir(subPath.c_str());
    }
    _mkdir(path.c_str());
    
    // Check if directory exists now
    DWORD attrib = GetFileAttributesA(m_outputDirectory.c_str());
    if (attrib == INVALID_FILE_ATTRIBUTES || !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
        Warning("[MaterialPipeline::ToPBR] Failed to create output directory: %s\n", m_outputDirectory.c_str());
        return false;
    }
    
    return true;
}

std::string TextureProcessor::GenerateOutputPath(const std::string& sourceName, const std::string& suffix) {
    // Build a readable filename from the game texture path, e.g.
    // "models/props_c17/furniture_metal001a" -> "models_props_c17_furniture_metal001a_normal.dds"
    // Source texture paths are case-insensitive, so lowercase for stable names.
    std::string name;
    name.reserve(sourceName.size());
    for (char c : sourceName) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            name += c;
        } else if (c >= 'A' && c <= 'Z') {
            name += static_cast<char>(c - 'A' + 'a');
        } else {
            name += '_';
        }
    }
    
    if (name.empty()) {
        name = "unnamed";
    }
    
    // Keep the full path under MAX_PATH: truncate very long names and append a
    // short hash of the original name to preserve uniqueness.
    constexpr size_t kMaxNameLength = 160;
    if (name.size() > kMaxNameLength) {
        std::ostringstream tail;
        tail << std::hex << std::uppercase << std::hash<std::string>{}(sourceName);
        name = name.substr(0, kMaxNameLength) + "_" + tail.str();
    }
    
    return m_outputDirectory + "\\" + name + suffix + ".dds";
}

// Calculate number of mipmap levels for a given dimension
static uint32_t CalculateMipLevels(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    uint32_t size = max(width, height);
    while (size > 1) {
        size /= 2;
        levels++;
    }
    return levels;
}

bool TextureProcessor::WriteDDSHeader(std::ofstream& file, uint32_t width, uint32_t height, bool hasAlpha, uint32_t mipCount) {
    DDSHeader header = {};
    
    header.magic = DDS_MAGIC;
    header.size = 124;  // Size of header minus magic number
    header.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_LINEARSIZE | DDSD_MIPMAPCOUNT;
    header.height = height;
    header.width = width;
    header.pitchOrLinearSize = width * height * (hasAlpha ? 4 : 3);
    header.depth = 1;
    header.mipMapCount = mipCount;
    
    // Pixel format for RGBA8888 or RGB888
    header.pixelFormat.size = 32;
    header.pixelFormat.flags = DDPF_RGB | (hasAlpha ? DDPF_ALPHAPIXELS : 0);
    header.pixelFormat.rgbBitCount = hasAlpha ? 32 : 24;
    header.pixelFormat.rBitMask = 0x00FF0000;  // Red
    header.pixelFormat.gBitMask = 0x0000FF00;  // Green
    header.pixelFormat.bBitMask = 0x000000FF;  // Blue
    header.pixelFormat.aBitMask = hasAlpha ? 0xFF000000 : 0;  // Alpha
    
    header.caps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
    
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    return file.good();
}

bool TextureProcessor::WriteTextureToDDS(const ConvertedTexture& texture, const std::string& outputPath) {
    if (texture.pixelData.empty()) {
        Warning("[MaterialPipeline::ToPBR] Cannot write empty texture\n");
        return false;
    }
    
    std::ofstream file(outputPath, std::ios::binary);
    if (!file.is_open()) {
        Warning("[MaterialPipeline::ToPBR] Failed to open file for writing: %s\n", outputPath.c_str());
        return false;
    }
    
    // Calculate number of mip levels
    uint32_t mipCount = CalculateMipLevels(texture.width, texture.height);
    
    // Write DDS header with mipmap info
    if (!WriteDDSHeader(file, texture.width, texture.height, true, mipCount)) {
        Warning("[MaterialPipeline::ToPBR] Failed to write DDS header\n");
        return false;
    }
    
    // Convert from RGBA to BGRA (DDS expects BGRA)
    std::vector<uint8_t> bgraData(texture.pixelData.size());
    for (size_t i = 0; i < texture.pixelData.size(); i += 4) {
        bgraData[i + 0] = texture.pixelData[i + 2];  // B <- R
        bgraData[i + 1] = texture.pixelData[i + 1];  // G <- G
        bgraData[i + 2] = texture.pixelData[i + 0];  // R <- B
        bgraData[i + 3] = texture.pixelData[i + 3];  // A <- A
    }
    
    // Write base mip level (level 0)
    file.write(reinterpret_cast<const char*>(bgraData.data()), bgraData.size());
    
    // Generate and write subsequent mip levels
    uint32_t mipWidth = texture.width;
    uint32_t mipHeight = texture.height;
    std::vector<uint8_t> currentMip = bgraData;
    
    for (uint32_t mip = 1; mip < mipCount; mip++) {
        uint32_t newWidth = max(1u, mipWidth / 2);
        uint32_t newHeight = max(1u, mipHeight / 2);
        
        std::vector<uint8_t> newMip(newWidth * newHeight * 4);
        
        // Box filter downscale (2x2 average)
        for (uint32_t y = 0; y < newHeight; y++) {
            for (uint32_t x = 0; x < newWidth; x++) {
                uint32_t srcX = x * 2;
                uint32_t srcY = y * 2;
                
                // Sample 2x2 block from source
                uint32_t r = 0, g = 0, b = 0, a = 0;
                int sampleCount = 0;
                
                for (int dy = 0; dy < 2 && (srcY + dy) < mipHeight; dy++) {
                    for (int dx = 0; dx < 2 && (srcX + dx) < mipWidth; dx++) {
                        size_t srcIdx = ((srcY + dy) * mipWidth + (srcX + dx)) * 4;
                        b += currentMip[srcIdx + 0];
                        g += currentMip[srcIdx + 1];
                        r += currentMip[srcIdx + 2];
                        a += currentMip[srcIdx + 3];
                        sampleCount++;
                    }
                }
                
                size_t dstIdx = (y * newWidth + x) * 4;
                newMip[dstIdx + 0] = static_cast<uint8_t>(b / sampleCount);
                newMip[dstIdx + 1] = static_cast<uint8_t>(g / sampleCount);
                newMip[dstIdx + 2] = static_cast<uint8_t>(r / sampleCount);
                newMip[dstIdx + 3] = static_cast<uint8_t>(a / sampleCount);
            }
        }
        
        // Write this mip level
        file.write(reinterpret_cast<const char*>(newMip.data()), newMip.size());
        
        // Prepare for next iteration
        currentMip = std::move(newMip);
        mipWidth = newWidth;
        mipHeight = newHeight;
    }
    
    if (!file.good()) {
        Warning("[MaterialPipeline::ToPBR] Failed to write texture data\n");
        return false;
    }
    
    file.close();
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Wrote DDS file: %s (%dx%d, %d mips)\n", outputPath.c_str(), texture.width, texture.height, mipCount);
    }
    
    return true;
}

bool TextureProcessor::GenerateRoughnessTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture) {
    // =========================================================================
    // ROUGHNESS SOURCE PRIORITY (reorganized with phong-first approach)
    // =========================================================================
    // PHONG MATERIALS ($phong=1): Use phong-specific properties first
    //   1. $phongexponenttexture - dedicated per-pixel phong exponent (BEST)
    //   2. $basemapalphaphongmask - base texture alpha as phong mask  
    //   3. $normalmapalphaenvmapmask - normal map alpha as phong mask
    //   4. $phong + $bumpmap - default Source Engine behavior (normal alpha)
    //
    // NON-PHONG MATERIALS: Use envmap-based properties
    //   5. $envmapmask - separate envmap mask texture
    //   6. $basealphaenvmapmask - base texture alpha as envmap mask
    //   7. Auto-discovered _mask/_spec textures
    //   8. $envmap + normal map alpha as last resort
    //   9. $envmap + base texture alpha as last resort
    //
    // If no valid source, return false -> use constant roughness in USDA
    // =========================================================================
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] GenerateRoughnessTexture for %s:\n", props.materialName.c_str());
        Msg("  hasPhong=%d, hasPhongExpTex=%d (%s)\n", props.hasPhong, props.hasPhongExponentTexture, props.phongExponentTexturePath.c_str());
        Msg("  normMapAlphaEnvMapMask=%d, hasBaseMapAlphaPhongMask=%d\n", props.normalMapAlphaEnvMapMask, props.hasBaseMapAlphaPhongMask);
        Msg("  hasBump=%d (%s)\n", props.hasBumpMap, props.bumpMapPath.c_str());
        Msg("  hasEnvMapMask=%d (%s), hasBaseAlphaEnvMapMask=%d\n", props.hasEnvMapMask, props.envMapMaskPath.c_str(), props.hasBaseAlphaEnvMapMask);
        Msg("  hasEnvMap=%d, hasEnvMapTint=%d\n", props.hasEnvMap, props.hasEnvMapTint);
        Msg("  hasDiscoveredMask=%d (%s)\n", props.hasDiscoveredMask, props.discoveredMaskPath.c_str());
        Msg("  baseTexturePath=%s\n", props.baseTexturePath.c_str());
    }
    
    std::string vtfPath;
    bool useAlphaChannel = false;
    bool isPhongExponentTexture = false;
    bool isInvertedMask = false;  // For $basealphaenvmapmask where white=masked(matte), black=reflective(shiny)
    
    // =========================================================================
    // PHONG MATERIAL PATH - prioritize phong-specific properties
    // =========================================================================
    if (props.hasPhong) {
        // Priority 1: $phongexponenttexture (best quality - dedicated roughness data)
        if (props.hasPhongExponentTexture && !props.phongExponentTexturePath.empty()) {
            vtfPath = props.phongExponentTexturePath;
            useAlphaChannel = false;
            isPhongExponentTexture = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [PHONG] Using $phongexponenttexture (best quality)\n", props.materialName.c_str());
            }
        }
        // Priority 2: $basemapalphaphongmask - base texture alpha as phong mask
        else if (props.hasBaseMapAlphaPhongMask && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [PHONG] Using base texture alpha ($basemapalphaphongmask)\n", props.materialName.c_str());
            }
        }
        // Priority 3: $normalmapalphaenvmapmask - normal map alpha as mask
        else if (props.normalMapAlphaEnvMapMask && props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [PHONG] Using normal map alpha ($normalmapalphaenvmapmask)\n", props.materialName.c_str());
            }
        }
        // Priority 4: Default Source Engine behavior - phong + bumpmap = normal alpha has phong mask
        else if (props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [PHONG] Using normal map alpha (default Source behavior)\n", props.materialName.c_str());
            }
        }
    }
    
    // =========================================================================
    // NON-PHONG / FALLBACK PATH - use envmap-based properties
    // =========================================================================
    if (vtfPath.empty()) {
        // Priority 5: $envmapmask - separate envmap mask texture
        if (props.hasEnvMapMask && !props.envMapMaskPath.empty()) {
            vtfPath = props.envMapMaskPath;
            useAlphaChannel = false;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Using $envmapmask for roughness\n", props.materialName.c_str());
            }
        }
        // Priority 6: $basealphaenvmapmask - base texture alpha as envmap mask (INVERTED!)
        else if (props.hasBaseAlphaEnvMapMask && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            isInvertedMask = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Using base texture alpha ($basealphaenvmapmask - INVERTED)\n", props.materialName.c_str());
            }
        }
        // Priority 7: Auto-discovered _mask/_spec textures
        else if (props.hasDiscoveredMask && !props.discoveredMaskPath.empty()) {
            vtfPath = props.discoveredMaskPath;
            useAlphaChannel = false;  // Use the RGB channels of the discovered mask
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Using auto-discovered mask/spec texture: %s\n", props.materialName.c_str(), props.discoveredMaskPath.c_str());
            }
        }
        // Priority 8: $envmap + normal map alpha (implicit $normalmapalphaenvmapmask)
        else if ((props.hasEnvMap || props.hasEnvMapTint) && props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Trying normal map alpha (implicit envmap roughness)\n", props.materialName.c_str());
            }
        }
        // Priority 9: $envmap + base texture alpha (implicit $basealphaenvmapmask)
        else if ((props.hasEnvMap || props.hasEnvMapTint) && !props.baseTexturePath.empty()) {
            vtfPath = props.baseTexturePath;
            useAlphaChannel = true;
            isInvertedMask = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Trying base texture alpha (implicit envmap - INVERTED)\n", props.materialName.c_str());
            }
        }
        // Priority 10 (LAST RESORT): Try normal map alpha anyway for materials with bumpmap
        else if (props.hasBumpMap && !props.bumpMapPath.empty()) {
            vtfPath = props.bumpMapPath;
            useAlphaChannel = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Last resort - trying normal map alpha\n", props.materialName.c_str());
            }
        }
    }
    
    // No valid roughness source found - use constant value in USDA
    if (vtfPath.empty()) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] No roughness texture source for %s, will use constant %.2f\n",
                props.materialName.c_str(), props.roughness);
        }
        return false;
    }
    
    // Try to read the texture
    std::vector<uint8_t> fileData;
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Attempting to read texture for roughness: %s\n", vtfPath.c_str());
    }
    
    if (!ReadVTFFile(vtfPath, fileData)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Failed to read VTF: %s (will use constant)\n", vtfPath.c_str());
        }
        return false;
    }
    
    VTFFileHeader header;
    if (!ParseVTFHeader(fileData, header)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Failed to parse VTF header: %s\n", vtfPath.c_str());
        }
        return false;
    }
    
    ConvertedTexture sourceTex;
    if (!ExtractVTFPixelData(fileData, header, sourceTex, false)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Failed to extract pixel data: %s\n", vtfPath.c_str());
        }
        return false;
    }
    
    // If using alpha channel, check if the alpha actually has variation
    // DXT1 textures only have 1-bit alpha (0 or 255), so they're not useful for masks
    if (useAlphaChannel) {
        bool hasAlphaVariation = false;
        uint8_t firstAlpha = sourceTex.pixelData.size() >= 4 ? sourceTex.pixelData[3] : 255;
        for (size_t i = 3; i < sourceTex.pixelData.size(); i += 4) {
            if (sourceTex.pixelData[i] != firstAlpha) {
                hasAlphaVariation = true;
                break;
            }
        }
        if (!hasAlphaVariation) {
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Alpha channel has no variation (all %d), will use constant roughness\n", 
                    vtfPath.c_str(), firstAlpha);
            }
            return false;
        }
    }
    
    // Convert source texture to roughness
    outTexture.width = sourceTex.width;
    outTexture.height = sourceTex.height;
    outTexture.pixelData.resize(sourceTex.width * sourceTex.height * 4);
    
    for (size_t i = 0; i < sourceTex.pixelData.size(); i += 4) {
        uint8_t roughness;
        
        if (isPhongExponentTexture) {
            // $phongexponenttexture: pixel value is the phong exponent (0-255 maps to exponent)
            // Higher exponent = shinier = LOWER roughness
            // The texture typically uses the red channel (or all channels for grayscale)
            uint8_t exponentValue = sourceTex.pixelData[i];  // Red channel
            
            // Convert exponent to roughness using perceptual curve
            // Exponent 0 (value 0) = very rough (roughness ~0.85)
            // Exponent 255 (max) = very shiny (roughness ~0.20)
            float normalizedExp = exponentValue / 255.0f;
            // Use sqrt curve to maintain perceptual half-shininess at half-value
            float shininessPerceptual = sqrtf(normalizedExp);
            float roughnessF = 0.85f - (shininessPerceptual * 0.65f);  // 0->0.85, 255->0.20
            roughness = static_cast<uint8_t>(std::clamp(roughnessF * 255.0f, 0.0f, 255.0f));
        } else if (useAlphaChannel) {
            // Use alpha channel from normal map or base texture
            uint8_t sourceValue = sourceTex.pixelData[i + 3];
            
            // Handle inverted mask semantics for $basealphaenvmapmask
            // Normal masks (phong, $normalmapalphaenvmapmask): bright = shiny areas = LOW roughness
            // Inverted masks ($basealphaenvmapmask): bright = MASKED (matte), dark = reflective (shiny)
            if (isInvertedMask) {
                // $basealphaenvmapmask: white (255) = masked/no reflection/matte, black (0) = reflective/shiny
                // Invert the source value first, then apply the same curve
                sourceValue = 255 - sourceValue;
            }
            
            // The mask represents "shininess" intensity (0-255) after potential inversion
            // Half mask value should give HALF SHININESS perception, not half roughness
            // Since roughness is roughly inverse-square to shininess perception,
            // we apply the perceptual curve to the shininess value first
            //
            // Shininess perception: half mask (127) = half shininess
            // Map: mask 0 -> low shininess -> high roughness (0.85)
            //      mask 255 -> high shininess -> low roughness (0.20)
            // 
            // Use sqrt on the mask value to preserve perceptual half-shininess at half-mask
            float normalizedMask = sourceValue / 255.0f;
            // Apply sqrt curve so half-mask gives perceptually half-shiny appearance
            float shininessPerceptual = sqrtf(normalizedMask);
            // Map to roughness range: full shininess (1.0) -> 0.20, no shininess (0.0) -> 0.85
            float roughnessF = 0.85f - (shininessPerceptual * 0.65f);
            roughness = static_cast<uint8_t>(std::clamp(roughnessF * 255.0f, 0.0f, 255.0f));
        } else {
            // Use the red channel (envmap mask)
            // Envmap mask controls environment reflections - similar to phong mask
            uint8_t sourceValue = sourceTex.pixelData[i];
            
            // Same perceptual curve as phong mask
            float normalizedMask = sourceValue / 255.0f;
            float shininessPerceptual = sqrtf(normalizedMask);
            float roughnessF = 0.85f - (shininessPerceptual * 0.65f);  // 0->0.85, 255->0.20
            roughness = static_cast<uint8_t>(std::clamp(roughnessF * 255.0f, 0.0f, 255.0f));
        }
        
        outTexture.pixelData[i + 0] = roughness;
        outTexture.pixelData[i + 1] = roughness;
        outTexture.pixelData[i + 2] = roughness;
        outTexture.pixelData[i + 3] = 255;
    }
    
    outTexture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    outTexture.mipLevels = 1;
    
    if (m_debugOutput) {
        const char* sourceType = isPhongExponentTexture ? "phong exponent texture" :
                                 useAlphaChannel ? "alpha channel (phong mask)" : "envmap mask";
        Msg("[MaterialPipeline::ToPBR] Generated roughness from %s: %s (%dx%d)\n",
            sourceType, vtfPath.c_str(), outTexture.width, outTexture.height);
    }
    return true;
}

bool TextureProcessor::GenerateMetallicTexture(const MaterialPBRProperties& props, ConvertedTexture& outTexture) {
    // Generate per-pixel metallic maps from base texture brightness
    // In Source Engine, dark areas + envmap = metallic (like chrome/metal parts)
    // Brighter areas = non-metallic (diffuse surfaces)
    
    // Only generate metallic map if material has envmap and average brightness suggests some metallic areas
    if (!props.hasEnvMap || props.baseTextureBrightness >= 0.4f || props.metallic <= 0.05f) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: No metallic texture needed (hasEnvMap=%d, avgBrightness=%.2f, metallic=%.2f)\n",
                props.materialName.c_str(), props.hasEnvMap ? 1 : 0, props.baseTextureBrightness, props.metallic);
        }
        return false;
    }
    
    // Read the base texture VTF
    std::vector<uint8_t> fileData;
    if (!ReadVTFFile(props.baseTexturePath, fileData)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: Could not read base texture for metallic map generation\n",
                props.materialName.c_str());
        }
        return false;
    }
    
    // Parse VTF header
    VTFFileHeader header;
    if (!ParseVTFHeader(fileData, header)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: Could not parse base texture VTF header for metallic map\n",
                props.materialName.c_str());
        }
        return false;
    }
    
    // Extract pixel data
    ConvertedTexture sourceTex;
    if (!ExtractVTFPixelData(fileData, header, sourceTex, false)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: Could not extract base texture pixel data for metallic map\n",
                props.materialName.c_str());
        }
        return false;
    }
    
    // Generate metallic map from per-pixel brightness
    // Dark pixels (brightness < 0.3) = metallic (scaled), bright pixels = non-metallic
    outTexture.width = sourceTex.width;
    outTexture.height = sourceTex.height;
    outTexture.pixelData.resize(sourceTex.width * sourceTex.height * 4);
    
    // Metallic map: store metallic value in R channel (grayscale)
    // Pixels darker than threshold become metallic, brighter = non-metallic
    constexpr float METALLIC_THRESHOLD = 0.30f;  // Brightness below this is considered metallic
    
    for (uint32_t y = 0; y < sourceTex.height; y++) {
        for (uint32_t x = 0; x < sourceTex.width; x++) {
            size_t srcIdx = (y * sourceTex.width + x) * 4;
            size_t dstIdx = (y * sourceTex.width + x) * 4;
            
            uint8_t r = sourceTex.pixelData[srcIdx];
            uint8_t g = sourceTex.pixelData[srcIdx + 1];
            uint8_t b = sourceTex.pixelData[srcIdx + 2];
            
            // Calculate per-pixel brightness (luminance)
            float brightness = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
            
            // Calculate metallic value: darker = more metallic
            // brightness 0.0 -> metallic 1.0
            // brightness 0.3 -> metallic 0.0
            // brightness 0.3+ -> metallic 0.0
            float metallic = 0.0f;
            if (brightness < METALLIC_THRESHOLD) {
                metallic = std::clamp(1.0f - (brightness / METALLIC_THRESHOLD), 0.0f, 1.0f);
            }
            
            uint8_t metallicByte = static_cast<uint8_t>(metallic * 255.0f);
            
            // Store as grayscale (R=G=B=metallic, A=255)
            outTexture.pixelData[dstIdx] = metallicByte;
            outTexture.pixelData[dstIdx + 1] = metallicByte;
            outTexture.pixelData[dstIdx + 2] = metallicByte;
            outTexture.pixelData[dstIdx + 3] = 255;
        }
    }
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] %s: Generated %dx%d per-pixel metallic map from base texture brightness\n",
            props.materialName.c_str(), sourceTex.width, sourceTex.height);
    }
    
    return true;
}

bool TextureProcessor::ReadVTFFile(const std::string& path, std::vector<uint8_t>& outData) {
    if (!m_fileSystem) {
        Warning("[MaterialPipeline::ToPBR] Filesystem not available\n");
        return false;
    }
    // Delegate to VTF module
    return VTF::ReadVTFFile(m_fileSystem, path, outData, m_debugOutput);
}

bool TextureProcessor::ParseVTFHeader(const std::vector<uint8_t>& fileData, VTFFileHeader& outHeader) {
    // Delegate to VTF module
    return VTF::ParseVTFHeader(fileData, outHeader, m_debugOutput);
}

// DXT decompression - delegate to VTF module
bool TextureProcessor::DecompressDXT1(const uint8_t* compressedData, uint32_t width, uint32_t height,
                                          std::vector<uint8_t>& outRGBA) {
    return VTF::DecompressDXT1(compressedData, width, height, outRGBA);
}

bool TextureProcessor::DecompressDXT5(const uint8_t* compressedData, uint32_t width, uint32_t height,
                                          std::vector<uint8_t>& outRGBA) {
    return VTF::DecompressDXT5(compressedData, width, height, outRGBA);
}

bool TextureProcessor::ExtractVTFPixelData(const std::vector<uint8_t>& fileData, 
                                               const VTFFileHeader& header,
                                               ConvertedTexture& outTexture, 
                                               bool isNormalMap) {
    // Delegate to VTF module
    return VTF::ExtractVTFPixelData(fileData, header, outTexture, isNormalMap, m_debugOutput);
}

void TextureProcessor::ConvertNormalMapToOctahedral(ConvertedTexture& texture) {
    // Delegate to VTF module
    VTF::ConvertNormalMapToOctahedral(texture, m_debugOutput);
}

void TextureProcessor::ConvertSSBumpToNormal(ConvertedTexture& texture) {
    // Delegate to VTF module
    VTF::ConvertSSBumpToNormal(texture, m_debugOutput);
}

bool TextureProcessor::IsSolidColorTexture(const std::vector<uint8_t>& pixelData, uint32_t width, uint32_t height) {
    // Handle edge cases
    if (width == 0 || height == 0) {
        return false;  // Invalid dimensions
    }
    
    // Validate pixel data size matches expected dimensions
    // Use size_t for all calculations to avoid overflow
    size_t pixelCount = static_cast<size_t>(width) * height;
    size_t expectedSize = pixelCount * 4;
    
    // Check for overflow in size calculation
    if (pixelCount / static_cast<size_t>(height) != static_cast<size_t>(width) || 
        expectedSize / 4 != pixelCount) {
        return false;  // Overflow detected
    }
    
    if (pixelData.size() < expectedSize) {
        return false;  // Data is truncated or invalid
    }
    
    // Get the first pixel color (RGBA)
    uint8_t r = pixelData[0];
    uint8_t g = pixelData[1];
    uint8_t b = pixelData[2];
    uint8_t a = pixelData[3];
    
    // For large textures, sample pixels instead of checking every single one
    // This provides a good balance between accuracy and performance
    const size_t maxSamples = 256;  // Sample up to 256 pixels
    size_t sampleStep = (pixelCount > maxSamples) ? (pixelCount / maxSamples) : 1;
    
    for (size_t i = 0; i < pixelCount; i += sampleStep) {
        size_t offset = i * 4;
        // offset calculation uses size_t, so no overflow if expectedSize calculation succeeded
        if (offset + 3 >= pixelData.size()) {
            break;
        }
        
        if (pixelData[offset] != r || 
            pixelData[offset + 1] != g || 
            pixelData[offset + 2] != b || 
            pixelData[offset + 3] != a) {
            return false;  // Found a different pixel
        }
    }
    
    // Also check last pixel to ensure we didn't miss edge cases
    if (pixelCount > 1) {
        size_t lastOffset = (pixelCount - 1) * 4;
        // Fixed bounds check to avoid underflow when pixelData.size() is 0
        if (pixelData.size() > 0 && lastOffset + 3 < pixelData.size()) {
            if (pixelData[lastOffset] != r || 
                pixelData[lastOffset + 1] != g || 
                pixelData[lastOffset + 2] != b || 
                pixelData[lastOffset + 3] != a) {
                return false;
            }
        }
    }
    
    return true;  // All sampled pixels are the same color
}

uint64_t TextureProcessor::GenerateTextureHashWithPixelData(const std::string& path, uint32_t width, uint32_t height, 
                                                              const std::vector<uint8_t>& pixelData) {
    // Start with the basic hash
    uint64_t hash = 14695981039346656037ULL;
    
    for (char c : path) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    
    // Mix in dimensions
    hash ^= width;
    hash *= 1099511628211ULL;
    hash ^= height;
    hash *= 1099511628211ULL;
    
    // Only check for solid color if pixel data is available
    // This avoids unnecessary computation for textures without pixel data
    if (!pixelData.empty()) {
        bool isSolidColor = IsSolidColorTexture(pixelData, width, height);
        
        // For solid color textures, add the actual color values to the hash
        // This ensures that textures with the same dimensions but different colors
        // get different hashes, preventing hash collisions in RTX Remix
        if (isSolidColor && pixelData.size() >= 4) {
            // Mix in RGBA values
            hash ^= static_cast<uint64_t>(pixelData[0]);  // R
            hash *= 1099511628211ULL;
            hash ^= static_cast<uint64_t>(pixelData[1]);  // G
            hash *= 1099511628211ULL;
            hash ^= static_cast<uint64_t>(pixelData[2]);  // B
            hash *= 1099511628211ULL;
            hash ^= static_cast<uint64_t>(pixelData[3]);  // A
            hash *= 1099511628211ULL;
            
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] Solid color texture detected: %s (RGBA: %d,%d,%d,%d) - hash with color data\n",
                    path.c_str(), pixelData[0], pixelData[1], pixelData[2], pixelData[3]);
            }
        }
    }
    
    // Ensure it's not 0
    if (hash == 0) hash = 1;
    
    return hash;
}


// =========================================================================
// D3D9 Texture Reading - Read pixel data directly from D3D9 textures
// This is used for runtime textures (e.g., render targets) that don't have VTF files
// =========================================================================

bool TextureProcessor::ReadD3D9TexturePixelData(IDirect3DTexture9* texture, 
                                                  std::vector<uint8_t>& outPixelData,
                                                  uint32_t& outWidth, 
                                                  uint32_t& outHeight) {
    if (!texture) {
        return false;
    }
    
    // Get texture description
    D3DSURFACE_DESC desc;
    HRESULT hr = texture->GetLevelDesc(0, &desc);
    if (FAILED(hr)) {
        if (m_debugOutput) {
            Warning("[MaterialPipeline::ToPBR] Failed to get D3D9 texture description: 0x%08X\n", hr);
        }
        return false;
    }
    
    outWidth = desc.Width;
    outHeight = desc.Height;
    
    // Validate texture size to prevent excessive memory allocation
    const uint32_t MAX_TEXTURE_SIZE = 4096;
    if (outWidth > MAX_TEXTURE_SIZE || outHeight > MAX_TEXTURE_SIZE) {
        if (m_debugOutput) {
            Warning("[MaterialPipeline::ToPBR] D3D9 texture too large: %dx%d (max %dx%d)\n", 
                outWidth, outHeight, MAX_TEXTURE_SIZE, MAX_TEXTURE_SIZE);
        }
        return false;
    }
    
    // Get the device from the texture
    IDirect3DDevice9* pDevice = nullptr;
    hr = texture->GetDevice(&pDevice);
    if (FAILED(hr) || !pDevice) {
        if (m_debugOutput) {
            Warning("[MaterialPipeline::ToPBR] Failed to get D3D9 device from texture: 0x%08X\n", hr);
        }
        return false;
    }
    
    // Try direct lock first (works for managed/system memory textures)
    D3DLOCKED_RECT lockedRect;
    hr = texture->LockRect(0, &lockedRect, nullptr, D3DLOCK_READONLY);
    
    // If direct lock fails (common for render targets in D3DPOOL_DEFAULT), 
    // use GetRenderTargetData to copy to a system memory surface
    IDirect3DSurface9* pSysSurface = nullptr;
    IDirect3DSurface9* pTexSurface = nullptr;
    bool usedRenderTargetCopy = false;
    
    if (FAILED(hr)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Direct lock failed (0x%08X), trying GetRenderTargetData...\n", hr);
        }
        
        // Get the texture's surface
        hr = texture->GetSurfaceLevel(0, &pTexSurface);
        if (FAILED(hr)) {
            if (m_debugOutput) {
                Warning("[MaterialPipeline::ToPBR] Failed to get texture surface: 0x%08X\n", hr);
            }
            pDevice->Release();
            return false;
        }
        
        // Create a system memory surface to copy to
        hr = pDevice->CreateOffscreenPlainSurface(
            desc.Width, desc.Height, desc.Format,
            D3DPOOL_SYSTEMMEM, &pSysSurface, nullptr);
        if (FAILED(hr)) {
            if (m_debugOutput) {
                Warning("[MaterialPipeline::ToPBR] Failed to create system memory surface: 0x%08X\n", hr);
            }
            pTexSurface->Release();
            pDevice->Release();
            return false;
        }
        
        // Copy render target data to system memory
        hr = pDevice->GetRenderTargetData(pTexSurface, pSysSurface);
        if (FAILED(hr)) {
            if (m_debugOutput) {
                Warning("[MaterialPipeline::ToPBR] GetRenderTargetData failed: 0x%08X\n", hr);
            }
            pSysSurface->Release();
            pTexSurface->Release();
            pDevice->Release();
            return false;
        }
        
        // Lock the system memory surface instead
        hr = pSysSurface->LockRect(&lockedRect, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            if (m_debugOutput) {
                Warning("[MaterialPipeline::ToPBR] Failed to lock system memory surface: 0x%08X\n", hr);
            }
            pSysSurface->Release();
            pTexSurface->Release();
            pDevice->Release();
            return false;
        }
        
        usedRenderTargetCopy = true;
    }
    
    // Allocate output buffer (RGBA8888)
    size_t pixelCount = static_cast<size_t>(outWidth) * outHeight;
    outPixelData.resize(pixelCount * 4);
    
    // Convert based on format
    const uint8_t* srcData = static_cast<const uint8_t*>(lockedRect.pBits);
    
    switch (desc.Format) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            // BGRA -> RGBA conversion (D3D stores as BGRA in memory)
            for (size_t y = 0; y < outHeight; y++) {
                const uint8_t* srcRow = srcData + y * lockedRect.Pitch;
                uint8_t* dstRow = outPixelData.data() + y * outWidth * 4;
                for (size_t x = 0; x < outWidth; x++) {
                    dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // R from B
                    dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G
                    dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // B from R
                    dstRow[x * 4 + 3] = (desc.Format == D3DFMT_X8R8G8B8) ? 255 : srcRow[x * 4 + 3]; // A
                }
            }
            break;
            
        case D3DFMT_R8G8B8:
            // BGR -> RGBA conversion (D3D stores as BGR in memory, despite the name)
            for (size_t y = 0; y < outHeight; y++) {
                const uint8_t* srcRow = srcData + y * lockedRect.Pitch;
                uint8_t* dstRow = outPixelData.data() + y * outWidth * 4;
                for (size_t x = 0; x < outWidth; x++) {
                    dstRow[x * 4 + 0] = srcRow[x * 3 + 2]; // R from B
                    dstRow[x * 4 + 1] = srcRow[x * 3 + 1]; // G
                    dstRow[x * 4 + 2] = srcRow[x * 3 + 0]; // B from R
                    dstRow[x * 4 + 3] = 255;               // A
                }
            }
            break;
            
        case D3DFMT_A8B8G8R8:
            // RGBA - direct copy (this format is already RGBA order)
            for (size_t y = 0; y < outHeight; y++) {
                const uint8_t* srcRow = srcData + y * lockedRect.Pitch;
                uint8_t* dstRow = outPixelData.data() + y * outWidth * 4;
                memcpy(dstRow, srcRow, outWidth * 4);
            }
            break;
            
        default:
            // Unsupported format - return false instead of guessing
            if (m_debugOutput) {
                Warning("[MaterialPipeline::ToPBR] Unsupported D3D9 texture format: %d\n", desc.Format);
            }
            if (usedRenderTargetCopy) {
                pSysSurface->UnlockRect();
                pSysSurface->Release();
                pTexSurface->Release();
            } else {
                texture->UnlockRect(0);
            }
            pDevice->Release();
            return false;
    }
    
    // Cleanup
    if (usedRenderTargetCopy) {
        pSysSurface->UnlockRect();
        pSysSurface->Release();
        pTexSurface->Release();
    } else {
        texture->UnlockRect(0);
    }
    pDevice->Release();
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Read D3D9 texture: %dx%d, format %d%s\n", 
            outWidth, outHeight, desc.Format, 
            usedRenderTargetCopy ? " (via GetRenderTargetData)" : "");
    }
    
    return true;
}

bool TextureProcessor::UploadTextureToRemix(const ConvertedTexture& texture, 
                                                remixapi_TextureHandle* outHandle) {
    if (!m_remixInterface) {
        Warning("[MaterialPipeline::ToPBR] Remix interface not available\n");
        return false;
    }
    
    remixapi_TextureInfo texInfo = {};
    texInfo.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    texInfo.pNext = nullptr;
    texInfo.hash = texture.hash;
    texInfo.width = texture.width;
    texInfo.height = texture.height;
    texInfo.depth = 1;
    texInfo.mipLevels = texture.mipLevels;
    texInfo.format = texture.format;
    texInfo.data = texture.pixelData.data();
    texInfo.dataSize = texture.pixelData.size();
    
    auto result = m_remixInterface->CreateTexture(texInfo);
    if (!result) {
        Warning("[MaterialPipeline::ToPBR] Failed to create texture: error %d\n", result.status());
        return false;
    }
    
    if (outHandle) {
        *outHandle = result.value();
    }
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Uploaded texture: %dx%d, hash 0x%llX\n", 
            texture.width, texture.height, texture.hash);
    }
    
    return true;
}

uint64_t TextureProcessor::ConvertAndUploadTexture(const std::string& vtfPath, bool isNormalMap) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // Check cache
    auto it = m_uploadedTextures.find(vtfPath);
    if (it != m_uploadedTextures.end()) {
        return it->second;
    }
    
    // Read VTF file
    std::vector<uint8_t> fileData;
    if (!ReadVTFFile(vtfPath, fileData)) {
        return 0;
    }
    
    // Parse header
    VTFFileHeader header;
    if (!ParseVTFHeader(fileData, header)) {
        return 0;
    }
    
    // Extract pixel data
    ConvertedTexture texture;
    texture.sourcePath = vtfPath;
    if (!ExtractVTFPixelData(fileData, header, texture, isNormalMap)) {
        return 0;
    }
    
    // Generate hash with pixel data (handles solid color detection)
    texture.hash = GenerateTextureHashWithPixelData(vtfPath, texture.width, texture.height, texture.pixelData);
    
    // Upload to Remix
    remixapi_TextureHandle handle = nullptr;
    if (!UploadTextureToRemix(texture, &handle)) {
        m_stats.failedConversions++;
        return 0;
    }
    
    // Cache the results
    m_uploadedTextures[vtfPath] = texture.hash;
    m_textureHandles[texture.hash] = handle;
    m_stats.texturesUploaded++;
    
    return texture.hash;
}

float TextureProcessor::PhongToRoughness(float phongExponent) {
    // Default to max roughness (most Source materials without phong are fully matte photoscans)
    if (phongExponent <= 0) return 1.0f;
    
    // Clamp to reasonable range
    phongExponent = std::clamp(phongExponent, 1.0f, 512.0f);
    
    // Standard formula to convert Phong exponent to PBR roughness:
    // roughness = sqrt(2 / (phongExponent + 2))
    // 
    // This is derived from the relationship between Phong specular and PBR GGX:
    // phongExponent 1   -> roughness ~0.82 (very broad highlight)
    // phongExponent 10  -> roughness ~0.41 (moderate)
    // phongExponent 25  -> roughness ~0.27 (fairly smooth)
    // phongExponent 50  -> roughness ~0.19 (smooth, like glossy plastic)
    // phongExponent 150 -> roughness ~0.11 (very smooth, like polished metal)
    // phongExponent 256 -> roughness ~0.09 (highly glossy)
    
    float roughness = sqrtf(2.0f / (phongExponent + 2.0f));
    
    // Clamp minimum to avoid perfectly mirror-like reflections (which can look broken)
    // and maximum to ensure some specular response for phong materials
    return std::clamp(roughness, 0.05f, 0.90f);
}

float TextureProcessor::CalculateRoughness(const MaterialPBRProperties& props) {
    // Start with roughness from phong exponent (defaults to fairly rough)
    float roughness = PhongToRoughness(props.phongExponent);
    
    // For materials without phong enabled, check if they have $envmap
    // This is common for LightmappedGeneric brushes (floors, walls, etc.)
    if (!props.hasPhong) {
        if (props.hasEnvMap) {
            // Material has $envmap, so it should be reflective
            // Base roughness depends on envmap tint
            if (props.hasEnvMapTint) {
                float tintIntensity = (props.envMapTint[0] + props.envMapTint[1] + props.envMapTint[2]) / 3.0f;
                // tintIntensity 1.0 (full) -> roughness 0.30 (shiny)
                // tintIntensity 0.5 (half) -> roughness 0.50 (semi-shiny)
                // tintIntensity 0.25 (quarter) -> roughness 0.60 (moderate)
                // tintIntensity 0.0 (none) -> roughness 0.75 (matte)
                roughness = 0.75f - (tintIntensity * 0.45f);
            } else {
                // Has envmap but no tint specified - assume moderate reflectivity
                roughness = 0.50f;
            }
            
            // If material has envmap mask, it will use per-pixel roughness later
            // Here we just set a reasonable constant for the USDA fallback
            if (props.hasEnvMapMask) {
                // Will use texture, but constant should be moderate
                roughness = min(roughness, 0.50f);
            }
            
            // NEW: If we detected metallic from dark base texture, use much lower roughness
            // Metallic materials like chrome need very low roughness to look right
            if (props.hasBaseTextureBrightness && props.baseTextureBrightness < 0.20f) {
                // Very dark base texture = polished metal = low roughness
                // brightness 0.0 -> roughness 0.05 (perfect chrome)
                // brightness 0.1 -> roughness 0.10 (polished metal)
                // brightness 0.2 -> roughness 0.15 (brushed metal)
                roughness = 0.05f + (props.baseTextureBrightness * 0.50f);
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] %s: Metallic roughness from dark texture: brightness=%.3f -> roughness=%.2f\n",
                        props.materialName.c_str(), props.baseTextureBrightness, roughness);
                }
            }
            
            return std::clamp(roughness, 0.05f, 0.75f);
        }
        // No phong and no envmap - just a matte photoscanned surface
        return 1.0f;
    }
    
    // If there's an envmap with tint, the tint controls reflection intensity
    // LOWER tint = LESS reflective = HIGHER roughness
    if (props.hasEnvMapTint && props.hasEnvMap) {
        float tintIntensity = (props.envMapTint[0] + props.envMapTint[1] + props.envMapTint[2]) / 3.0f;
        
        // Low tint means the material reflects less, so increase roughness a bit
        // But don't completely override the phong-based roughness
        if (tintIntensity < 0.5f) {
            roughness = roughness + (0.5f - tintIntensity) * 0.3f;
        }
        roughness = std::clamp(roughness, 0.30f, 0.85f);
    }
    
    // Phong boost affects highlight brightness
    // Higher boost suggests intentionally shiny material - DECREASE roughness slightly
    if (props.phongBoost > 1.0f) {
        // phongBoost 2 -> small decrease
        // phongBoost 5 -> moderate decrease
        // phongBoost 10 -> capped decrease (don't go below 0.30)
        float boostFactor = min((props.phongBoost - 1.0f) * 0.03f, 0.15f);
        roughness = max(0.30f, roughness - boostFactor);
    }
    
    // NEW: Rim lighting affects perceived glossiness
    // Materials with rim lighting typically have shiny edges, suggesting lower roughness overall
    if (props.hasRimLight) {
        // Rim lighting active - material is shinier
        // rimlightboost 0.5 -> small decrease
        // rimlightboost 1.0 -> moderate decrease
        // rimlightboost 2.0+ -> significant decrease
        float rimFactor = 0.05f;  // Base rim factor
        if (props.hasRimLightBoost && props.rimLightBoost > 0.5f) {
            rimFactor = min((props.rimLightBoost - 0.5f) * 0.04f + 0.05f, 0.15f);
        }
        roughness = max(0.30f, roughness - rimFactor);
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: Rim light adjustment: rimFactor=%.2f, roughness=%.2f\n",
                props.materialName.c_str(), rimFactor, roughness);
        }
    }
    
    // NEW: $envmapcontrast affects perceived glossiness
    // Higher contrast means sharper reflections = lower roughness
    if (props.hasEnvMapContrast && props.envMapContrast > 0.0f) {
        float contrastFactor = min(props.envMapContrast * 0.05f, 0.10f);
        roughness = max(0.25f, roughness - contrastFactor);
    }
    
    // NEW: $phongalbedotint with high boost suggests color-tinted metal-like reflections
    if (props.phongAlbedoTint && props.hasPhongAlbedoBoost && props.phongAlbedoBoost > 1.0f) {
        float albedoBoostFactor = min((props.phongAlbedoBoost - 1.0f) * 0.02f, 0.08f);
        roughness = max(0.30f, roughness - albedoBoostFactor);
    }
    
    return std::clamp(roughness, 0.30f, 0.85f);
}

float TextureProcessor::EstimateMetallic(const MaterialPBRProperties& props) {
    float metallic = 0.0f;
    
    // NEW: Experimental metallic detection from base texture brightness
    // This feature is DISABLED by default because:
    // - In Source Engine: black texture + envmap = chrome look (envmap provides reflections)
    // - In PBR: metallic = 1 means "use base color as reflection color", so black = no reflections
    // The correct approach for most Source Engine materials is to use low roughness, not metallic.
    //
    // Enable with rtx_topbr_metallic 1 for experimentation
    if (m_metallicGenerationEnabled && props.hasEnvMap && props.hasBaseTextureBrightness) {
        // Brightness threshold for metallic detection:
        // Very dark textures (brightness < 0.1) with strong envmap = highly metallic
        // The metallic value decreases as brightness increases
        // At brightness 0.3+, we consider it non-metallic (just reflective)
        if (props.baseTextureBrightness < 0.30f) {
            // Inverse relationship: darker = more metallic
            // brightness 0.0 -> metallic 1.0
            // brightness 0.1 -> metallic 0.8
            // brightness 0.3 -> metallic 0.0
            metallic = std::clamp(1.0f - (props.baseTextureBrightness / 0.30f), 0.0f, 1.0f);
            
            // Scale by envmap tint intensity if available (brighter envmap = more reflective)
            if (props.hasEnvMapTint) {
                float envmapIntensity = (props.envMapTint[0] + props.envMapTint[1] + props.envMapTint[2]) / 3.0f;
                metallic *= envmapIntensity;
            }
            
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [EXPERIMENTAL] Metallic from base texture darkness: brightness=%.3f -> metallic=%.2f\n",
                    props.materialName.c_str(), props.baseTextureBrightness, metallic);
            }
        }
    }
    
    // High phong boost suggests metal-like reflections (fallback, only if metallic generation enabled)
    if (m_metallicGenerationEnabled && props.phongBoost > 2.0f) {
        float phongMetallic = std::clamp((props.phongBoost - 2.0f) / 8.0f, 0.0f, 0.5f);
        metallic = max(metallic, phongMetallic);
    }
    
    // Having an envmap mask suggests reflective surface (only if metallic generation enabled)
    if (m_metallicGenerationEnabled && props.hasEnvMapMask) {
        metallic = max(metallic, 0.2f);
    }
    
    return metallic;
}

// Analyze base texture brightness to detect metallic materials
// Black textures + envmap = metallic, grey/colored textures = non-metallic
bool TextureProcessor::AnalyzeBaseTextureBrightness(const std::string& texturePath, float& outBrightness) {
    if (texturePath.empty()) {
        return false;
    }
    
    // Read the VTF file
    std::vector<uint8_t> fileData;
    if (!ReadVTFFile(texturePath, fileData)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Failed to read base texture for brightness analysis: %s\n", texturePath.c_str());
        }
        return false;
    }
    
    VTFFileHeader header;
    if (!ParseVTFHeader(fileData, header)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Failed to parse VTF header for brightness analysis: %s\n", texturePath.c_str());
        }
        return false;
    }
    
    ConvertedTexture sourceTex;
    if (!ExtractVTFPixelData(fileData, header, sourceTex, false)) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Failed to extract pixel data for brightness analysis: %s\n", texturePath.c_str());
        }
        return false;
    }
    
    // Calculate average brightness (luminance) from the texture
    // Using standard luminance weights: 0.299*R + 0.587*G + 0.114*B
    double totalLuminance = 0.0;
    size_t pixelCount = 0;
    
    for (size_t i = 0; i < sourceTex.pixelData.size(); i += 4) {
        float r = sourceTex.pixelData[i] / 255.0f;
        float g = sourceTex.pixelData[i + 1] / 255.0f;
        float b = sourceTex.pixelData[i + 2] / 255.0f;
        
        // Standard luminance calculation
        float luminance = 0.299f * r + 0.587f * g + 0.114f * b;
        totalLuminance += luminance;
        pixelCount++;
    }
    
    if (pixelCount == 0) {
        return false;
    }
    
    outBrightness = static_cast<float>(totalLuminance / pixelCount);
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] %s: Base texture brightness analyzed: %.3f (pixels=%zu)\n",
            texturePath.c_str(), outBrightness, pixelCount);
    }
    
    return true;
}

// Discover companion textures that might not be explicitly referenced in the VMT
// E.g., if basetexture is "metal/metal001", look for "metal/metal001_normal", "_height", "_mask", "_spec"
void TextureProcessor::DiscoverCompanionTextures(const std::string& baseTexturePath, MaterialPBRProperties& props) {
    // Initialize discovered texture flags
    props.hasDiscoveredNormal = false;
    props.hasDiscoveredHeight = false;
    props.hasDiscoveredMask = false;
    props.hasDiscoveredAO = false;
    props.discoveredNormalPath = "";
    props.discoveredHeightPath = "";
    props.discoveredMaskPath = "";
    props.discoveredAOPath = "";
    
    if (baseTexturePath.empty() || !m_autoDiscoverEnabled) {
        return;
    }
    
    // Get the base path without extension
    std::string basePath = baseTexturePath;
    
    // Remove any .vtf extension if present
    size_t extPos = basePath.rfind(".vtf");
    if (extPos != std::string::npos) {
        basePath = basePath.substr(0, extPos);
    }
    extPos = basePath.rfind(".VTF");
    if (extPos != std::string::npos) {
        basePath = basePath.substr(0, extPos);
    }
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Searching for companion textures for: %s\n", basePath.c_str());
    }
    
    // List of suffix variants to check for each type
    // We'll try to read the VTF file to verify it exists
    
    // Helper lambda to check if a VTF texture exists
    auto TextureExists = [this](const std::string& texPath) -> bool {
        std::vector<uint8_t> fileData;
        return ReadVTFFile(texPath, fileData);
    };
    
    // Normal map variants - only discover if we don't already have a bumpmap
    if (!props.hasBumpMap) {
        const char* normalSuffixes[] = { "_normal", "_n", "_norm", "_nrm", "_Normal", "_N" };
        for (const char* suffix : normalSuffixes) {
            std::string candidatePath = basePath + suffix;
            if (TextureExists(candidatePath)) {
                props.discoveredNormalPath = candidatePath;
                props.hasDiscoveredNormal = true;
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] Discovered normal map: %s\n", candidatePath.c_str());
                }
                break;
            }
        }
    }
    
    // Height/parallax map variants - only discover if we don't already have one
    if (!props.hasParallaxMap) {
        const char* heightSuffixes[] = { "_height", "_h", "_bump", "_disp", "_Height", "_H" };
        for (const char* suffix : heightSuffixes) {
            std::string candidatePath = basePath + suffix;
            if (TextureExists(candidatePath)) {
                props.discoveredHeightPath = candidatePath;
                props.hasDiscoveredHeight = true;
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] Discovered height map: %s\n", candidatePath.c_str());
                }
                break;
            }
        }
    }
    
    // Mask/spec map variants (for roughness) - only discover if we don't have any roughness source
    if (!props.hasEnvMapMask && !props.hasPhongExponentTexture) {
        const char* maskSuffixes[] = { "_mask", "_spec", "_gloss", "_roughness", "_s", "_Mask", "_Spec" };
        for (const char* suffix : maskSuffixes) {
            std::string candidatePath = basePath + suffix;
            if (TextureExists(candidatePath)) {
                props.discoveredMaskPath = candidatePath;
                props.hasDiscoveredMask = true;
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] Discovered mask/spec map: %s\n", candidatePath.c_str());
                }
                break;
            }
        }
    }
    
    // AO (ambient occlusion) map variants
    const char* aoSuffixes[] = { "_ao", "_occlusion", "_AO", "_Occlusion" };
    for (const char* suffix : aoSuffixes) {
        std::string candidatePath = basePath + suffix;
        if (TextureExists(candidatePath)) {
            props.discoveredAOPath = candidatePath;
            props.hasDiscoveredAO = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] Discovered AO map: %s\n", candidatePath.c_str());
            }
            break;
        }
    }
}

// Helper function to check if a file exists
static bool FileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

// Helper struct for VMT properties parsed from file
struct VMTProperties {
    std::string shaderName;
    bool hasRefractAmount;
    float refractAmount;
    bool hasTranslucent;
    bool translucent;
    std::string surfaceProp;
    bool hasEnvMap;
    std::string envMap;
    bool hasEnvMapSphere;            // $envmapsphere - spherical (not cubemap) environment projection
    bool envMapSphere;
    bool hasEnvMapMode;              // $envmapmode - alternate envmap UV mode used with $envmapsphere
    bool envMapMode;
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
    // NEW: Additional properties for comprehensive PBR extraction
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

// Parse a VMT file and extract properties that FindVar doesn't reliably expose
// This is crucial because when running with DX6 fallback shaders, FindVar() returns
// incorrect or missing values for many material properties
static bool ParseVMTFile(IFileSystem* fileSystem, const std::string& materialName, VMTProperties& outProps, bool debugOutput, int depth = 0) {
    if (!fileSystem) return false;
    
    // Guard against include cycles in patch VMT chains
    if (depth > 4) {
        return false;
    }
    
    // Initialize all properties to defaults
    outProps = VMTProperties{};
    outProps.hasRefractAmount = false;
    outProps.refractAmount = 0.0f;
    outProps.hasTranslucent = false;
    outProps.translucent = false;
    outProps.hasEnvMap = false;
    outProps.hasEnvMapSphere = false;
    outProps.envMapSphere = false;
    outProps.hasEnvMapMode = false;
    outProps.envMapMode = false;
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
    
    // NEW: Initialize additional properties
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
    
    // Parse the VMT content - REMOVE COMMENTS FIRST (unless convar enabled)
    std::string content(buffer.data());
    
    // Strip out commented lines (// style) to prevent parsing commented-out properties
    // UNLESS the user has enabled parsing of commented properties (for maps where
    // envmap/masks were disabled for vanilla Source performance but benefit RTX Remix)
    std::string contentWithoutComments;
    if (!TextureProcessor::Instance().IsParseCommentedPropertiesEnabled()) {
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
    
    // vbsp cubemap-patched brush materials (e.g. "maps/<map>/<material>_X_Y_Z")
    // use the "patch" shader wrapping the real VMT via "include". Parse the
    // included base VMT so water and other brush materials resolve their true
    // shader and texture properties.
    {
        std::string shaderLower = outProps.shaderName;
        std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (shaderLower == "patch") {
            std::string includePath = findValue("include");
            if (includePath.empty()) {
                return false;
            }
            if (debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: patch VMT - resolving include '%s'\n",
                    materialName.c_str(), includePath.c_str());
            }
            return ParseVMTFile(fileSystem, includePath, outProps, debugOutput, depth + 1);
        }
    }
    
    // Check for $refractamount
    size_t refractPos = contentLower.find("$refractamount");
    if (refractPos != std::string::npos) {
        outProps.hasRefractAmount = true;
        std::string valStr = findValue("$refractamount");
        if (!valStr.empty()) {
            try {
                outProps.refractAmount = std::stof(valStr);
            } catch (...) {
                outProps.refractAmount = 0.25f;  // Default
            }
        }
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
    
    // $envmapsphere + $envmapmode together mean a fully reflective chrome
    // surface with no envmap mask - Source draws these as mirror-like metal.
    size_t envmapSpherePos = contentLower.find("$envmapsphere");
    if (envmapSpherePos != std::string::npos) {
        outProps.hasEnvMapSphere = true;
        std::string valStr = findValue("$envmapsphere");
        outProps.envMapSphere = (valStr == "1" || valStr == "true");
    }
    size_t envmapModePos = contentLower.find("$envmapmode");
    if (envmapModePos != std::string::npos) {
        outProps.hasEnvMapMode = true;
        std::string valStr = findValue("$envmapmode");
        outProps.envMapMode = (valStr == "1" || valStr == "true");
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
    
    // Texture paths
    if (contentLower.find("$basetexture") != std::string::npos) {
        outProps.baseTexture = findValue("$basetexture");
        outProps.hasBaseTexture = !outProps.baseTexture.empty();
    }
    
    if (contentLower.find("$bumpmap") != std::string::npos) {
        outProps.bumpMap = findValue("$bumpmap");
        outProps.hasBumpMap = !outProps.bumpMap.empty();
    }
    
    if (contentLower.find("$normalmap") != std::string::npos) {
        outProps.normalMap = findValue("$normalmap");
        outProps.hasNormalMap = !outProps.normalMap.empty();
    }
    
    if (contentLower.find("$envmapmask") != std::string::npos) {
        outProps.envMapMask = findValue("$envmapmask");
        outProps.hasEnvMapMask = !outProps.envMapMask.empty();
    }
    
    if (contentLower.find("$phongexponenttexture") != std::string::npos) {
        outProps.phongExponentTexture = findValue("$phongexponenttexture");
        outProps.hasPhongExponentTexture = !outProps.phongExponentTexture.empty();
    }
    
    // Phong properties
    if (contentLower.find("$phong") != std::string::npos) {
        std::string val = findValue("$phong");
        if (!val.empty()) {
            outProps.hasPhong = true;
            outProps.phong = (val == "1" || val == "true") ? 1 : 0;
        }
    }
    
    if (contentLower.find("$phongexponent") != std::string::npos && 
        contentLower.find("$phongexponenttexture") == std::string::npos) {  // Don't match the texture
        std::string val = findValue("$phongexponent");
        if (!val.empty()) {
            try {
                outProps.phongExponent = std::stof(val);
                outProps.hasPhongExponent = true;
            } catch (...) {}
        }
    }
    
    if (contentLower.find("$phongboost") != std::string::npos) {
        std::string val = findValue("$phongboost");
        if (!val.empty()) {
            try {
                outProps.phongBoost = std::stof(val);
                outProps.hasPhongBoost = true;
            } catch (...) {}
        }
    }
    
    // SSBump
    if (contentLower.find("$ssbump") != std::string::npos) {
        std::string val = findValue("$ssbump");
        if (!val.empty()) {
            outProps.hasSSBump = true;
            outProps.ssbump = (val == "1" || val == "true") ? 1 : 0;
        }
    }
    
    // Mask properties
    if (contentLower.find("$normalmapalphaenvmapmask") != std::string::npos) {
        std::string val = findValue("$normalmapalphaenvmapmask");
        if (!val.empty()) {
            outProps.hasNormalMapAlphaEnvMapMask = true;
            outProps.normalMapAlphaEnvMapMask = (val == "1" || val == "true") ? 1 : 0;
        }
    }
    
    if (contentLower.find("$basemapalphaphongmask") != std::string::npos) {
        std::string val = findValue("$basemapalphaphongmask");
        if (!val.empty()) {
            outProps.hasBaseMapAlphaPhongMask = true;
            outProps.baseMapAlphaPhongMask = (val == "1" || val == "true") ? 1 : 0;
        }
    }
    
    if (contentLower.find("$basealphaenvmapmask") != std::string::npos) {
        std::string val = findValue("$basealphaenvmapmask");
        if (!val.empty()) {
            outProps.hasBaseAlphaEnvMapMask = true;
            outProps.baseAlphaEnvMapMask = (val == "1" || val == "true") ? 1 : 0;
        }
    }
    
    // $selfillum
    if (contentLower.find("$selfillum") != std::string::npos) {
        std::string val = findValue("$selfillum");
        if (!val.empty()) {
            outProps.hasSelfIllum = true;
            outProps.selfIllum = (val == "1" || val == "true") ? 1 : 0;
        }
    }
    
    // Helper for parsing vector values like "[x y z]" or "x y z"
    auto parseVector3 = [&content, &contentLower](const std::string& keyLower, float out[3]) -> bool {
        size_t pos = contentLower.find(keyLower);
        if (pos == std::string::npos) return false;
        
        pos += keyLower.length();
        // Skip to value
        while (pos < content.length() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '"')) {
            pos++;
        }
        
        // Read the rest of the line
        size_t endPos = content.find_first_of("\r\n", pos);
        if (endPos == std::string::npos) endPos = content.length();
        std::string valueStr = content.substr(pos, endPos - pos);
        
        // Try parsing [x y z] format
        float x = 0, y = 0, z = 0;
        if (sscanf(valueStr.c_str(), "[%f %f %f]", &x, &y, &z) == 3 ||
            sscanf(valueStr.c_str(), "[ %f %f %f ]", &x, &y, &z) == 3 ||
            sscanf(valueStr.c_str(), "%f %f %f", &x, &y, &z) == 3) {
            out[0] = x; out[1] = y; out[2] = z;
            return true;
        }
        // Try single value (uniform)
        if (sscanf(valueStr.c_str(), "%f", &x) == 1) {
            out[0] = out[1] = out[2] = x;
            return true;
        }
        return false;
    };
    
    // $envmaptint
    if (contentLower.find("$envmaptint") != std::string::npos) {
        if (parseVector3("$envmaptint", outProps.envMapTint)) {
            outProps.hasEnvMapTint = true;
        }
    }
    
    // $phongfresnelranges
    if (contentLower.find("$phongfresnelranges") != std::string::npos) {
        if (parseVector3("$phongfresnelranges", outProps.phongFresnelRanges)) {
            outProps.hasPhongFresnelRanges = true;
        }
    }
    
    // =========================================================================
    // NEW: Parse additional properties for comprehensive PBR extraction
    // =========================================================================
    
    // $selfillummask - separate mask texture for emissive areas
    if (contentLower.find("$selfillummask") != std::string::npos) {
        outProps.selfIllumMask = findValue("$selfillummask");
        outProps.hasSelfIllumMask = !outProps.selfIllumMask.empty();
    }
    
    // $selfillumtint - tint color for self-illumination
    if (contentLower.find("$selfillumtint") != std::string::npos) {
        if (parseVector3("$selfillumtint", outProps.selfIllumTint)) {
            outProps.hasSelfIllumTint = true;
        }
    }
    
    // $rimlight - enables rim lighting
    if (contentLower.find("$rimlight") != std::string::npos) {
        std::string val = findValue("$rimlight");
        if (!val.empty()) {
            outProps.hasRimLight = true;
            outProps.rimLight = (val == "1" || val == "true") ? 1 : 0;
        }
    }
    
    // $rimlightexponent - sharpness of rim light
    if (contentLower.find("$rimlightexponent") != std::string::npos) {
        std::string val = findValue("$rimlightexponent");
        if (!val.empty()) {
            try {
                outProps.rimLightExponent = std::stof(val);
                outProps.hasRimLightExponent = true;
            } catch (...) {}
        }
    }
    
    // $rimlightboost - intensity of rim light
    if (contentLower.find("$rimlightboost") != std::string::npos) {
        std::string val = findValue("$rimlightboost");
        if (!val.empty()) {
            try {
                outProps.rimLightBoost = std::stof(val);
                outProps.hasRimLightBoost = true;
            } catch (...) {}
        }
    }
    
    // $phongalbedotint - uses base color to tint phong highlight
    if (contentLower.find("$phongalbedotint") != std::string::npos) {
        std::string val = findValue("$phongalbedotint");
        if (!val.empty()) {
            outProps.hasPhongAlbedoTint = true;
            outProps.phongAlbedoTint = (val == "1" || val == "true") ? 1 : 0;
        }
    }
    
    // $phongalbedoboost - boost for albedo tint effect
    if (contentLower.find("$phongalbedoboost") != std::string::npos) {
        std::string val = findValue("$phongalbedoboost");
        if (!val.empty()) {
            try {
                outProps.phongAlbedoBoost = std::stof(val);
                outProps.hasPhongAlbedoBoost = true;
            } catch (...) {}
        }
    }
    
    // $phongtint - direct tint for phong highlight
    if (contentLower.find("$phongtint") != std::string::npos) {
        if (parseVector3("$phongtint", outProps.phongTint)) {
            outProps.hasPhongTint = true;
        }
    }
    
    // $parallaxmap - heightmap texture for parallax effect
    if (contentLower.find("$parallaxmap") != std::string::npos) {
        outProps.parallaxMap = findValue("$parallaxmap");
        outProps.hasParallaxMap = !outProps.parallaxMap.empty();
    }
    
    // $parallaxmapscale - depth scale for parallax effect
    if (contentLower.find("$parallaxmapscale") != std::string::npos) {
        std::string val = findValue("$parallaxmapscale");
        if (!val.empty()) {
            try {
                outProps.parallaxMapScale = std::stof(val);
                outProps.hasParallaxMapScale = true;
            } catch (...) {}
        }
    }
    
    // $envmapcontrast - contrast for environment reflections
    if (contentLower.find("$envmapcontrast") != std::string::npos) {
        std::string val = findValue("$envmapcontrast");
        if (!val.empty()) {
            try {
                outProps.envMapContrast = std::stof(val);
                outProps.hasEnvMapContrast = true;
            } catch (...) {}
        }
    }
    
    // $envmapsaturation - saturation for environment reflections
    if (contentLower.find("$envmapsaturation") != std::string::npos) {
        std::string val = findValue("$envmapsaturation");
        if (!val.empty()) {
            try {
                outProps.envMapSaturation = std::stof(val);
                outProps.hasEnvMapSaturation = true;
            } catch (...) {}
        }
    }
    
    // =========================================================================
    // ExoPBR community PBR format detection
    // ExoPBR uses screenspace_general_8tex shader with ExoPBR proxy
    // =========================================================================
    std::string shaderLower = outProps.shaderName;
    std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(), ::tolower);
    
    if (shaderLower == "screenspace_general_8tex") {
        // Check for ExoPBR proxy
        if (contentLower.find("exopbr") != std::string::npos) {
            outProps.isExoPBR = true;
            
            // $texture1 - ARM map (Ambient Occlusion, Roughness, Metallic)
            // Red: AO, Green: Roughness, Blue: Metallic, Alpha: Height
            if (contentLower.find("$texture1") != std::string::npos) {
                outProps.texture1 = findValue("$texture1");
                outProps.hasTexture1 = !outProps.texture1.empty();
            }
            
            // $texture2 - Normal map (DirectX Y- format)
            if (contentLower.find("$texture2") != std::string::npos) {
                outProps.texture2 = findValue("$texture2");
                outProps.hasTexture2 = !outProps.texture2.empty();
            }
            
            // $texture3 - Emission texture
            if (contentLower.find("$texture3") != std::string::npos) {
                outProps.texture3 = findValue("$texture3");
                outProps.hasTexture3 = !outProps.texture3.empty();
            }
            
            // $emissionscale - emission intensity
            if (contentLower.find("$emissionscale") != std::string::npos) {
                std::string val = findValue("$emissionscale");
                if (!val.empty()) {
                    try {
                        outProps.emissionScale = std::stof(val);
                        outProps.hasEmissionScale = true;
                    } catch (...) {}
                }
            }
            
            // $emissiontint - emission color tint (vector3)
            size_t emTintPos = contentLower.find("$emissiontint");
            if (emTintPos != std::string::npos) {
                size_t bracketStart = content.find('[', emTintPos);
                size_t bracketEnd = content.find(']', emTintPos);
                if (bracketStart != std::string::npos && bracketEnd != std::string::npos) {
                    std::string vectorStr = content.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
                    float r = 1.0f, g = 1.0f, b = 1.0f;
                    if (sscanf(vectorStr.c_str(), "%f %f %f", &r, &g, &b) >= 3) {
                        outProps.emissionTint[0] = r;
                        outProps.emissionTint[1] = g;
                        outProps.emissionTint[2] = b;
                        outProps.hasEmissionTint = true;
                    }
                }
            }
        }
    }
    
    // =========================================================================
    // GPBR (Strata Source) community PBR format detection
    // GPBR uses the "PBR" shader name
    // =========================================================================
    if (shaderLower == "pbr") {
        outProps.isGPBR = true;
        
        // $mraotexture - MRAO map (Metallic=R, Roughness=G, AO=B)
        if (contentLower.find("$mraotexture") != std::string::npos) {
            outProps.mraoTexture = findValue("$mraotexture");
            outProps.hasMRAOTexture = !outProps.mraoTexture.empty();
        }
        
        // $mraoscale - MRAO intensity multiplier
        if (contentLower.find("$mraoscale") != std::string::npos) {
            std::string value = findValue("$mraoscale");
            if (!value.empty()) {
                outProps.mraoScale = (float)atof(value.c_str());
                outProps.hasMRAOScale = true;
            }
        }
        
        // $emissiontexture - Emission/glow map
        if (contentLower.find("$emissiontexture") != std::string::npos) {
            outProps.gpbrEmissionTexture = findValue("$emissiontexture");
            outProps.hasGPBREmissionTexture = !outProps.gpbrEmissionTexture.empty();
        }
        
        // $emissionscale - Emission intensity (note: reuses name from ExoPBR but separate field)
        if (contentLower.find("$emissionscale") != std::string::npos) {
            std::string value = findValue("$emissionscale");
            if (!value.empty()) {
                // Handle both single float and vector format
                if (value[0] == '[') {
                    float r = 1.0f, g = 1.0f, b = 1.0f;
                    if (sscanf(value.c_str(), "[%f %f %f]", &r, &g, &b) >= 3) {
                        // Use average as scale
                        outProps.gpbrEmissionScale = (r + g + b) / 3.0f;
                    } else {
                        outProps.gpbrEmissionScale = (float)atof(value.c_str() + 1);
                    }
                } else {
                    outProps.gpbrEmissionScale = (float)atof(value.c_str());
                }
                outProps.hasGPBREmissionScale = true;
            }
        }
        
        // $parallax - Enable parallax mapping (height in normal map alpha)
        if (contentLower.find("$parallax") != std::string::npos) {
            std::string value = findValue("$parallax");
            if (!value.empty()) {
                outProps.gpbrParallax = (atoi(value.c_str()) != 0);
                outProps.hasGPBRParallax = true;
            }
        }
        
        // $parallaxdepth - Parallax/displacement depth
        if (contentLower.find("$parallaxdepth") != std::string::npos) {
            std::string value = findValue("$parallaxdepth");
            if (!value.empty()) {
                outProps.gpbrParallaxDepth = (float)atof(value.c_str());
                outProps.hasGPBRParallaxDepth = true;
            }
        }
        
        // $parallaxcenter - Parallax center point
        if (contentLower.find("$parallaxcenter") != std::string::npos) {
            std::string value = findValue("$parallaxcenter");
            if (!value.empty()) {
                outProps.gpbrParallaxCenter = (float)atof(value.c_str());
                outProps.hasGPBRParallaxCenter = true;
            }
        }
        
        // $alpha - Transparency value
        if (contentLower.find("$alpha") != std::string::npos) {
            std::string value = findValue("$alpha");
            if (!value.empty()) {
                outProps.gpbrAlpha = (float)atof(value.c_str());
                outProps.hasGPBRAlpha = true;
            }
        }
    }
    
    // =========================================================================
    // MWB PBR Gen format detection (must check BEFORE BFT)
    // Uses _rgb suffix, pbr\output\ path, MwEnvMapTint/Arc9EnvMapTint proxies
    // =========================================================================
    if (!outProps.isExoPBR && !outProps.isGPBR) {
        // Create VMTParseResult for modular detection
        VMTParseResult vmtParse;
        vmtParse.shaderName = outProps.shaderName;
        vmtParse.content = content;
        vmtParse.contentLower = contentLower;
        
        if (MWBPBR::Detect(vmtParse)) {
            outProps.isMWBPBR = true;
            // ExtractProperties is called later in CreatePBRMaterial with MaterialPBRProperties
        }
    }
    
    // =========================================================================
    // BlueFlyTrap PseudoPBR format detection (only if not MWB)
    // Uses the modular BFTPseudoPBR::Detect() function for comprehensive detection
    // =========================================================================
    if (!outProps.isExoPBR && !outProps.isGPBR && !outProps.isMWBPBR) {
        // Create VMTParseResult for modular detection
        VMTParseResult vmtParse;
        vmtParse.shaderName = outProps.shaderName;
        vmtParse.content = content;
        vmtParse.contentLower = contentLower;
        
        if (BFTPseudoPBR::Detect(vmtParse)) {
            outProps.isBFTPseudoPBR = true;
            
            // Detect metallic layer: $translucent "1" + $phongalbedotint "1"
            bool hasTranslucent = outProps.hasTranslucent && outProps.translucent;
            bool hasAlbedoTint = outProps.hasPhongAlbedoTint && (outProps.phongAlbedoTint != 0);
            outProps.isBFTMetallicLayer = hasTranslucent && hasAlbedoTint;
            
            // Also check for $blendTintByBaseAlpha diffuse layer pattern
            if (contentLower.find("$blendtintbybasealpha") != std::string::npos) {
                std::string blendTint = findValue("$blendtintbybasealpha");
                if (!blendTint.empty() && atoi(blendTint.c_str()) == 1) {
                    outProps.isBFTDiffuseLayer = true;
                }
            }
        }
    }
    
    if (debugOutput) {
        Msg("[MaterialPipeline::ToPBR] VMT direct parse for '%s':\n", materialName.c_str());
        Msg("  shader='%s', $basetexture='%s', $bumpmap='%s'\n",
            outProps.shaderName.c_str(), outProps.baseTexture.c_str(), outProps.bumpMap.c_str());
        Msg("  $phong=%d, $phongexponent=%.1f, $ssbump=%d, $envmap=%d\n",
            outProps.phong, outProps.phongExponent, outProps.ssbump, outProps.hasEnvMap ? 1 : 0);
        if (outProps.hasEnvMapTint) {
            Msg("  $envmaptint=[%.2f %.2f %.2f]\n", outProps.envMapTint[0], outProps.envMapTint[1], outProps.envMapTint[2]);
        }
        // NEW: Log additional properties
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
        // ExoPBR specific logging
        if (outProps.isExoPBR) {
            Msg("  [ExoPBR] Detected community PBR format!\n");
            if (outProps.hasTexture1) Msg("    $texture1 (ARM)='%s'\n", outProps.texture1.c_str());
            if (outProps.hasTexture2) Msg("    $texture2 (Normal)='%s'\n", outProps.texture2.c_str());
            if (outProps.hasTexture3) Msg("    $texture3 (Emission)='%s'\n", outProps.texture3.c_str());
            if (outProps.hasEmissionScale) Msg("    $emissionscale=%.2f\n", outProps.emissionScale);
            if (outProps.hasEmissionTint) Msg("    $emissiontint=[%.2f %.2f %.2f]\n", outProps.emissionTint[0], outProps.emissionTint[1], outProps.emissionTint[2]);
        }
        // GPBR specific logging
        if (outProps.isGPBR) {
            Msg("  [GPBR] Detected Strata Source PBR format!\n");
            if (outProps.hasMRAOTexture) Msg("    $mraotexture='%s'\n", outProps.mraoTexture.c_str());
            if (outProps.hasMRAOScale) Msg("    $mraoscale=%.2f\n", outProps.mraoScale);
            if (outProps.hasGPBREmissionTexture) Msg("    $emissiontexture='%s'\n", outProps.gpbrEmissionTexture.c_str());
            if (outProps.hasGPBREmissionScale) Msg("    $emissionscale=%.2f\n", outProps.gpbrEmissionScale);
            if (outProps.hasGPBRParallax) Msg("    $parallax=%d, depth=%.3f\n", outProps.gpbrParallax ? 1 : 0, outProps.gpbrParallaxDepth);
            if (outProps.hasGPBRAlpha) Msg("    $alpha=%.2f\n", outProps.gpbrAlpha);
        }
        // MWB PBR Gen specific logging
        if (outProps.isMWBPBR) {
            Msg("  [MWB-PBR] Detected MWB PBR Gen format!\n");
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

bool TextureProcessor::ExtractMaterialPBR(const std::string& materialName, 
                                              MaterialPBRProperties& outProps) {
    if (!materials) {
        Warning("[MaterialPipeline::ToPBR] Material system not available\n");
        return false;
    }
    
    IMaterial* pMaterial = materials->FindMaterial(materialName.c_str(), TEXTURE_GROUP_OTHER, false);
    if (!pMaterial || pMaterial->IsErrorMaterial()) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Material not found: %s\n", materialName.c_str());
        }
        return false;
    }
    
    outProps.materialName = materialName;
    outProps.hasPhong = false;
    outProps.hasBumpMap = false;
    outProps.isSSBump = false;
    outProps.hasEnvMapMask = false;
    outProps.hasPhongExponentTexture = false;
    outProps.isSelfIllum = false;
    outProps.isTranslucent = false;
    outProps.phongExponent = 0;
    outProps.phongBoost = 1.0f;
    outProps.roughness = 1.0f;  // Default to max roughness (matte)
    outProps.metallic = 0.0f;   // Default to non-metallic
    
    // Extended properties
    outProps.normalMapAlphaEnvMapMask = false;
    outProps.hasPhongFresnelRanges = false;
    outProps.phongFresnelRanges[0] = 0.0f;
    outProps.phongFresnelRanges[1] = 0.0f;
    outProps.phongFresnelRanges[2] = 0.0f;
    outProps.hasEnvMapTint = false;
    outProps.envMapTint[0] = 1.0f;
    outProps.envMapTint[1] = 1.0f;
    outProps.envMapTint[2] = 1.0f;
    outProps.hasEnvMap = false;
    outProps.hasBaseMapAlphaPhongMask = false;
    outProps.baseMapAlphaPhongMask = 0.0f;
    outProps.hasBaseAlphaEnvMapMask = false;  // $basealphaenvmapmask for LightmappedGeneric
    
    // Glass properties
    outProps.isGlass = false;
    outProps.isWater = false;
    outProps.isRefractShader = false;
    outProps.shaderName = "";
    outProps.surfaceProp = "";
    outProps.refractTintTexturePath = "";
    outProps.isEnvmapSphereChrome = false;
    
    // Metallic detection from base texture brightness
    outProps.baseTextureBrightness = 0.5f;  // Default to mid-grey (non-metallic)
    outProps.hasBaseTextureBrightness = false;
    
    // NEW: Initialize additional properties
    // Self-illumination / Emissive
    outProps.selfIllumMaskPath = "";
    outProps.hasSelfIllumMask = false;
    outProps.selfIllumTint[0] = outProps.selfIllumTint[1] = outProps.selfIllumTint[2] = 1.0f;
    outProps.hasSelfIllumTint = false;
    
    // Rim lighting
    outProps.hasRimLight = false;
    outProps.rimLightExponent = 4.0f;
    outProps.rimLightBoost = 1.0f;
    outProps.hasRimLightExponent = false;
    outProps.hasRimLightBoost = false;
    
    // Additional phong properties
    outProps.phongAlbedoTint = false;
    outProps.phongAlbedoBoost = 1.0f;
    outProps.hasPhongAlbedoBoost = false;
    outProps.phongTint[0] = outProps.phongTint[1] = outProps.phongTint[2] = 1.0f;
    outProps.hasPhongTint = false;
    
    // Parallax/heightmap
    outProps.parallaxMapPath = "";
    outProps.hasParallaxMap = false;
    outProps.parallaxMapScale = 0.05f;
    outProps.hasParallaxMapScale = false;
    
    // Additional envmap properties
    outProps.envMapContrast = 0.0f;
    outProps.hasEnvMapContrast = false;
    outProps.envMapSaturation = 1.0f;
    outProps.hasEnvMapSaturation = false;
    
    // Secondary envmap mask
    outProps.envMapMask2Path = "";
    outProps.hasEnvMapMask2 = false;
    
    // Auto-discovered companion textures
    outProps.discoveredNormalPath = "";
    outProps.hasDiscoveredNormal = false;
    outProps.discoveredHeightPath = "";
    outProps.hasDiscoveredHeight = false;
    outProps.discoveredMaskPath = "";
    outProps.hasDiscoveredMask = false;
    outProps.discoveredAOPath = "";
    outProps.hasDiscoveredAO = false;
    
    // ExoPBR community PBR format
    outProps.isExoPBR = false;
    outProps.armTexturePath = "";
    outProps.hasARMTexture = false;
    outProps.exoNormalPath = "";
    outProps.hasExoNormal = false;
    outProps.emissionTexturePath = "";
    outProps.hasEmissionTexture = false;
    outProps.emissionScale = 1.0f;
    outProps.hasEmissionScale = false;
    outProps.emissionTint[0] = outProps.emissionTint[1] = outProps.emissionTint[2] = 1.0f;
    outProps.hasEmissionTint = false;
    
    // GPBR (Strata Source) community PBR format
    outProps.isGPBR = false;
    outProps.mraoTexturePath = "";
    outProps.hasMRAOTexture = false;
    outProps.mraoScale = 1.0f;
    outProps.hasMRAOScale = false;
    outProps.gpbrEmissionPath = "";
    outProps.hasGPBREmission = false;
    outProps.gpbrEmissionScale = 1.0f;
    outProps.hasGPBREmissionScale = false;
    outProps.gpbrParallax = false;
    outProps.gpbrParallaxDepth = 0.1f;
    outProps.gpbrAlpha = 1.0f;
    outProps.hasGPBRAlpha = false;
    
    // BlueFlyTrap PseudoPBR format
    outProps.isBFTPseudoPBR = false;
    outProps.isBFTMetallicLayer = false;
    outProps.isBFTDiffuseLayer = false;
    outProps.bftExponentTexturePath = "";
    outProps.hasBFTExponentTexture = false;
    outProps.bftColor2[0] = 1.0f;
    outProps.bftColor2[1] = 1.0f;
    outProps.bftColor2[2] = 1.0f;
    outProps.hasBFTColor2 = false;
    
    // MWB PBR Gen format
    outProps.isMWBPBR = false;
    
    // Get the shader name
    const char* shaderName = pMaterial->GetShaderName();
    if (shaderName && shaderName[0] != '\0') {
        outProps.shaderName = shaderName;
    }
    
    // =========================================================================
    // IMPORTANT: Parse VMT file directly to get material properties
    // This is crucial because when running with DX6 fallback shaders (which Remix
    // forces), FindVar() returns incorrect or missing values for many properties.
    // We use VMT-parsed values as the primary source, falling back to FindVar
    // only when VMT parsing fails.
    // =========================================================================
    VMTProperties vmtParsed;
    bool hasVMTParsed = ParseVMTFile(m_fileSystem, materialName, vmtParsed, m_debugOutput);
    
    // If VMT parsing succeeded, prefer VMT shader name over the DX6 fallback name
    if (hasVMTParsed && !vmtParsed.shaderName.empty()) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: Using VMT shader '%s' instead of runtime '%s'\n", 
                materialName.c_str(), vmtParsed.shaderName.c_str(), outProps.shaderName.c_str());
        }
        outProps.shaderName = vmtParsed.shaderName;
    }
    
    // Helper lambda to check if a texture path is valid (not a placeholder/internal texture)
    auto IsValidTexturePath = [](const std::string& path) -> bool {
        if (path.empty()) return false;
        // Filter out RTX internal textures
        if (path.find("rtx/") == 0 || path.find("rtx\\") == 0) return false;
        // Filter out render targets and internal textures
        if (path.find("_rt_") != std::string::npos) return false;
        if (path.find("__") == 0) return false;  // Internal textures like __error
        // Filter out procedural textures
        if (path.find("env_cubemap") != std::string::npos) return false;
        // Filter out undefined/invalid texture markers (with or without angle brackets)
        if (path == "UNDEFINED" || path == "<UNDEFINED>") return false;
        if (path.find("UNDEFINED") != std::string::npos) return false;
        // Filter out error textures that Source Engine returns for missing/invalid textures
        if (path == "error" || path == "Error" || path == "ERROR") return false;
        // Filter out paths that are just numbers or very short
        if (path.length() < 3) return false;
        return true;
    };
    
    // Get $basetexture - prefer VMT-parsed value, fallback to FindVar
    if (hasVMTParsed && vmtParsed.hasBaseTexture && IsValidTexturePath(vmtParsed.baseTexture)) {
        outProps.baseTexturePath = vmtParsed.baseTexture;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $basetexture (from VMT) = %s\n", materialName.c_str(), outProps.baseTexturePath.c_str());
        }
    } else {
        // Fallback to FindVar
        bool found = false;
        IMaterialVar* pVar = pMaterial->FindVar("$basetexture", &found, false);
        if (found && pVar) {
            std::string texPath;
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                texPath = strVal;
            }
            if (!IsValidTexturePath(texPath)) {
                ITexture* pTex = pVar->GetTextureValue();
                if (pTex) {
                    texPath = pTex->GetName();
                }
            }
            if (IsValidTexturePath(texPath)) {
                outProps.baseTexturePath = texPath;
            }
            if (m_debugOutput && !outProps.baseTexturePath.empty()) {
                Msg("[MaterialPipeline::ToPBR] %s: $basetexture (from FindVar) = %s\n", materialName.c_str(), outProps.baseTexturePath.c_str());
            } else if (m_debugOutput && strVal) {
                Msg("[MaterialPipeline::ToPBR] %s: $basetexture filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
            }
        }
    }
    
    // Get $bumpmap - prefer VMT-parsed value, fallback to FindVar.
    // Water shader: $bumpmap holds the legacy DX8 du/dv distortion map (UV88
    // format, not convertible to a normal map). The real tangent-space normal
    // map lives in $normalmap, handled below - so skip $bumpmap for water.
    bool found = false;
    std::string shaderLower = outProps.shaderName;
    std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool isWaterShader = shaderLower.find("water") != std::string::npos;
    outProps.isWater = isWaterShader;
    if (isWaterShader) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: Water shader - ignoring $bumpmap (du/dv map), using $normalmap\n",
                materialName.c_str());
        }
    } else if (hasVMTParsed && vmtParsed.hasBumpMap && IsValidTexturePath(vmtParsed.bumpMap)) {
        outProps.bumpMapPath = vmtParsed.bumpMap;
        outProps.hasBumpMap = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $bumpmap (from VMT) = %s\n", materialName.c_str(), outProps.bumpMapPath.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$bumpmap", &found, false);
        if (found && pVar) {
            std::string texPath;
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                texPath = strVal;
            }
            if (!IsValidTexturePath(texPath)) {
                ITexture* pTex = pVar->GetTextureValue();
                if (pTex) {
                    texPath = pTex->GetName();
                }
            }
            if (IsValidTexturePath(texPath)) {
                outProps.bumpMapPath = texPath;
                outProps.hasBumpMap = true;
            }
            if (m_debugOutput && outProps.hasBumpMap) {
                Msg("[MaterialPipeline::ToPBR] %s: $bumpmap (from FindVar) = %s\n", materialName.c_str(), outProps.bumpMapPath.c_str());
            } else if (m_debugOutput && strVal) {
                Msg("[MaterialPipeline::ToPBR] %s: $bumpmap filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
            }
        }
    }
    
    // Check for $ssbump flag - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasSSBump && vmtParsed.ssbump != 0) {
        outProps.isSSBump = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $ssbump = 1 (from VMT, will convert to normal map)\n", materialName.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$ssbump", &found, false);
        if (found && pVar) {
            int ssbumpValue = pVar->GetIntValue();
            if (ssbumpValue != 0) {
                outProps.isSSBump = true;
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] %s: $ssbump = 1 (will convert to normal map)\n", materialName.c_str());
                }
            }
        }
    }
    
    // Get $normalmap - prefer VMT-parsed value, only if we don't have $bumpmap
    if (!outProps.hasBumpMap) {
        if (hasVMTParsed && vmtParsed.hasNormalMap && IsValidTexturePath(vmtParsed.normalMap)) {
            outProps.bumpMapPath = vmtParsed.normalMap;
            outProps.hasBumpMap = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: $normalmap (from VMT) = %s\n", materialName.c_str(), outProps.bumpMapPath.c_str());
            }
        } else {
            IMaterialVar* pVar = pMaterial->FindVar("$normalmap", &found, false);
            if (found && pVar) {
                std::string texPath;
                const char* strVal = pVar->GetStringValue();
                if (strVal && strVal[0] != '\0') {
                    texPath = strVal;
                }
                if (!IsValidTexturePath(texPath)) {
                    ITexture* pTex = pVar->GetTextureValue();
                    if (pTex) {
                        texPath = pTex->GetName();
                    }
                }
                if (IsValidTexturePath(texPath)) {
                    outProps.bumpMapPath = texPath;
                    outProps.hasBumpMap = true;
                }
                if (m_debugOutput && outProps.hasBumpMap) {
                    Msg("[MaterialPipeline::ToPBR] %s: $normalmap = %s\n", materialName.c_str(), outProps.bumpMapPath.c_str());
                } else if (m_debugOutput && strVal) {
                    Msg("[MaterialPipeline::ToPBR] %s: $normalmap filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
                }
            }
        }
    }
    
    // Get $envmapmask - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasEnvMapMask && IsValidTexturePath(vmtParsed.envMapMask)) {
        outProps.envMapMaskPath = vmtParsed.envMapMask;
        outProps.hasEnvMapMask = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $envmapmask (from VMT) = %s\n", materialName.c_str(), outProps.envMapMaskPath.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$envmapmask", &found, false);
        if (found && pVar) {
            std::string texPath;
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                texPath = strVal;
            }
            if (!IsValidTexturePath(texPath)) {
                ITexture* pTex = pVar->GetTextureValue();
                if (pTex) {
                    texPath = pTex->GetName();
                }
            }
            if (IsValidTexturePath(texPath)) {
                outProps.envMapMaskPath = texPath;
                outProps.hasEnvMapMask = true;
            }
            if (m_debugOutput && outProps.hasEnvMapMask) {
                Msg("[MaterialPipeline::ToPBR] %s: $envmapmask = %s\n", materialName.c_str(), outProps.envMapMaskPath.c_str());
            } else if (m_debugOutput && strVal) {
                Msg("[MaterialPipeline::ToPBR] %s: $envmapmask filtered (invalid path: '%s')\n", materialName.c_str(), strVal);
            }
        } else if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $envmapmask not found\n", materialName.c_str());
        }
    }
    
    // Get $envmap - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasEnvMap) {
        outProps.hasEnvMap = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $envmap (from VMT) = %s\n", materialName.c_str(), vmtParsed.envMap.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$envmap", &found, false);
        if (found && pVar) {
            const char* strVal = pVar->GetStringValue();
            bool isUndefined = false;
            if (strVal) {
                if (strcmp(strVal, "UNDEFINED") == 0 || strcmp(strVal, "<UNDEFINED>") == 0) {
                    isUndefined = true;
                } else if (strstr(strVal, "UNDEFINED") != nullptr) {
                    isUndefined = true;
                }
            }
            if (strVal && strVal[0] != '\0' && !isUndefined) {
                outProps.hasEnvMap = true;
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] %s: $envmap = %s\n", materialName.c_str(), strVal);
                }
            } else if (m_debugOutput && strVal && isUndefined) {
                Msg("[MaterialPipeline::ToPBR] %s: $envmap = %s (ignored)\n", materialName.c_str(), strVal);
            }
        }
    }
    
    // $envmapsphere + $envmapmode together mean a fully reflective chrome
    // surface with no envmap mask - force full metal, mirror-like roughness.
    {
        bool hasSphere = false, sphere = false, hasMode = false, mode = false;
        if (hasVMTParsed) {
            hasSphere = vmtParsed.hasEnvMapSphere;
            sphere = vmtParsed.envMapSphere;
            hasMode = vmtParsed.hasEnvMapMode;
            mode = vmtParsed.envMapMode;
        }
        if (!hasSphere) {
            IMaterialVar* pVar = pMaterial->FindVar("$envmapsphere", &found, false);
            if (found && pVar) {
                hasSphere = true;
                sphere = pVar->GetIntValue() != 0;
            }
        }
        if (!hasMode) {
            IMaterialVar* pVar = pMaterial->FindVar("$envmapmode", &found, false);
            if (found && pVar) {
                hasMode = true;
                mode = pVar->GetIntValue() != 0;
            }
        }
        outProps.isEnvmapSphereChrome = hasSphere && sphere && hasMode && mode;
        if (m_debugOutput && outProps.isEnvmapSphereChrome) {
            Msg("[MaterialPipeline::ToPBR] %s: $envmapsphere+$envmapmode detected -> forcing chrome\n", materialName.c_str());
        }
    }
    
    // Get $normalmapalphaenvmapmask - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasNormalMapAlphaEnvMapMask) {
        outProps.normalMapAlphaEnvMapMask = (vmtParsed.normalMapAlphaEnvMapMask != 0);
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $normalmapalphaenvmapmask (from VMT) = %d\n", materialName.c_str(), vmtParsed.normalMapAlphaEnvMapMask);
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$normalmapalphaenvmapmask", &found, false);
        if (found && pVar) {
            int intVal = pVar->GetIntValue();
            float floatVal = pVar->GetFloatValue();
            const char* strVal = pVar->GetStringValue();
            
            bool isTruthy = (intVal != 0) || (floatVal != 0.0f);
            if (!isTruthy && strVal && strVal[0] != '\0') {
                isTruthy = (atoi(strVal) != 0) || (atof(strVal) != 0.0);
            }
            
            outProps.normalMapAlphaEnvMapMask = isTruthy;
            if (m_debugOutput) {
                if (outProps.normalMapAlphaEnvMapMask) {
                    Msg("[MaterialPipeline::ToPBR] %s: $normalmapalphaenvmapmask = 1 (will use normal alpha for roughness)\n", materialName.c_str());
                } else {
                    Msg("[MaterialPipeline::ToPBR] %s: $normalmapalphaenvmapmask found but value = 0\n", materialName.c_str());
                }
            }
        } else if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $normalmapalphaenvmapmask NOT FOUND\n", materialName.c_str());
        }
    }
    
    // Get $phong - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhong) {
        outProps.hasPhong = (vmtParsed.phong != 0);
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phong", &found, false);
        if (found && pVar) {
            outProps.hasPhong = (pVar->GetIntValue() == 1);
        }
    }
    
    // Get $phongexponent - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhongExponent) {
        outProps.phongExponent = vmtParsed.phongExponent;
        if (!outProps.hasPhong) outProps.hasPhong = true;
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phongexponent", &found, false);
        if (found && pVar) {
            outProps.phongExponent = pVar->GetFloatValue();
            if (!outProps.hasPhong) outProps.hasPhong = true;
        }
    }
    
    // Get $phongboost - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhongBoost) {
        outProps.phongBoost = vmtParsed.phongBoost;
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phongboost", &found, false);
        if (found && pVar) {
            outProps.phongBoost = pVar->GetFloatValue();
        }
    }
    
    // Get $phongexponenttexture - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhongExponentTexture && IsValidTexturePath(vmtParsed.phongExponentTexture)) {
        outProps.phongExponentTexturePath = vmtParsed.phongExponentTexture;
        outProps.hasPhongExponentTexture = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $phongexponenttexture (from VMT) = %s\n", materialName.c_str(), outProps.phongExponentTexturePath.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phongexponenttexture", &found, false);
        if (found && pVar) {
            std::string texPath;
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                texPath = strVal;
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] %s: $phongexponenttexture raw string = '%s'\n", materialName.c_str(), strVal);
                }
            }
            if (!IsValidTexturePath(texPath)) {
                ITexture* pTex = pVar->GetTextureValue();
                if (pTex) {
                    texPath = pTex->GetName();
                    if (m_debugOutput) {
                        Msg("[MaterialPipeline::ToPBR] %s: $phongexponenttexture from texture = '%s'\n", materialName.c_str(), texPath.c_str());
                    }
                }
            }
            if (IsValidTexturePath(texPath)) {
                outProps.phongExponentTexturePath = texPath;
                outProps.hasPhongExponentTexture = true;
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] %s: $phongexponenttexture = %s\n", materialName.c_str(), texPath.c_str());
                }
            } else if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: $phongexponenttexture FILTERED as invalid path: '%s'\n", materialName.c_str(), texPath.c_str());
            }
        } else if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $phongexponenttexture not found\n", materialName.c_str());
        }
    }
    
    // Get $basemapalphaphongmask - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasBaseMapAlphaPhongMask) {
        outProps.hasBaseMapAlphaPhongMask = (vmtParsed.baseMapAlphaPhongMask != 0);
        if (m_debugOutput && outProps.hasBaseMapAlphaPhongMask) {
            Msg("[MaterialPipeline::ToPBR] %s: $basemapalphaphongmask (from VMT) = 1\n", materialName.c_str());
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$basemapalphaphongmask", &found, false);
        if (found && pVar) {
            outProps.hasBaseMapAlphaPhongMask = (pVar->GetIntValue() == 1);
            if (m_debugOutput && outProps.hasBaseMapAlphaPhongMask) {
                Msg("[MaterialPipeline::ToPBR] %s: $basemapalphaphongmask = 1\n", materialName.c_str());
            }
        }
    }
    
    // Get $basealphaenvmapmask - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasBaseAlphaEnvMapMask) {
        outProps.hasBaseAlphaEnvMapMask = (vmtParsed.baseAlphaEnvMapMask != 0);
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $basealphaenvmapmask (from VMT) = %d\n", materialName.c_str(), vmtParsed.baseAlphaEnvMapMask);
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$basealphaenvmapmask", &found, false);
        if (found && pVar) {
            int intVal = pVar->GetIntValue();
            float floatVal = pVar->GetFloatValue();
            const char* strVal = pVar->GetStringValue();
            
            bool isTruthy = (intVal != 0) || (floatVal != 0.0f);
            if (!isTruthy && strVal && strVal[0] != '\0') {
                isTruthy = (atoi(strVal) != 0) || (atof(strVal) != 0.0);
            }
            
            outProps.hasBaseAlphaEnvMapMask = isTruthy;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: $basealphaenvmapmask found (int=%d, float=%.2f, str='%s') -> %d\n", 
                    materialName.c_str(), intVal, floatVal, strVal ? strVal : "null", 
                    outProps.hasBaseAlphaEnvMapMask ? 1 : 0);
            }
        } else if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $basealphaenvmapmask NOT FOUND\n", materialName.c_str());
        }
    }
    
    // Get $phongfresnelranges - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasPhongFresnelRanges) {
        outProps.phongFresnelRanges[0] = vmtParsed.phongFresnelRanges[0];
        outProps.phongFresnelRanges[1] = vmtParsed.phongFresnelRanges[1];
        outProps.phongFresnelRanges[2] = vmtParsed.phongFresnelRanges[2];
        outProps.hasPhongFresnelRanges = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $phongfresnelranges (from VMT) = [%.2f %.2f %.2f]\n", 
                materialName.c_str(), outProps.phongFresnelRanges[0], outProps.phongFresnelRanges[1], outProps.phongFresnelRanges[2]);
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$phongfresnelranges", &found, false);
        if (found && pVar) {
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                float x = 0, y = 0, z = 0;
                if (sscanf(strVal, "[%f %f %f]", &x, &y, &z) == 3 ||
                    sscanf(strVal, "%f %f %f", &x, &y, &z) == 3) {
                    outProps.phongFresnelRanges[0] = x;
                    outProps.phongFresnelRanges[1] = y;
                    outProps.phongFresnelRanges[2] = z;
                    outProps.hasPhongFresnelRanges = true;
                    if (m_debugOutput) {
                        Msg("[MaterialPipeline::ToPBR] %s: $phongfresnelranges = [%.2f %.2f %.2f]\n", 
                            materialName.c_str(), x, y, z);
                    }
                }
            }
        }
    }
    
    // Get $envmaptint - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasEnvMapTint) {
        outProps.envMapTint[0] = vmtParsed.envMapTint[0];
        outProps.envMapTint[1] = vmtParsed.envMapTint[1];
        outProps.envMapTint[2] = vmtParsed.envMapTint[2];
        
        // Check if non-default
        bool isDefaultTint = (fabs(vmtParsed.envMapTint[0] - 1.0f) < 0.01f && 
                             fabs(vmtParsed.envMapTint[1] - 1.0f) < 0.01f && 
                             fabs(vmtParsed.envMapTint[2] - 1.0f) < 0.01f);
        if (!isDefaultTint) {
            outProps.hasEnvMapTint = true;
            if (!outProps.hasEnvMap) {
                outProps.hasEnvMap = true;
            }
        }
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $envmaptint (from VMT) = [%.2f %.2f %.2f]%s\n", 
                materialName.c_str(), vmtParsed.envMapTint[0], vmtParsed.envMapTint[1], vmtParsed.envMapTint[2],
                isDefaultTint ? " (default)" : "");
        }
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$envmaptint", &found, false);
        if (found && pVar) {
            const char* strVal = pVar->GetStringValue();
            if (strVal && strVal[0] != '\0') {
                float r = 1, g = 1, b = 1;
                bool parsed = false;
                
                if (sscanf(strVal, "[%f %f %f]", &r, &g, &b) == 3 ||
                    sscanf(strVal, "%f %f %f", &r, &g, &b) == 3) {
                    parsed = true;
                } else if (sscanf(strVal, "%f", &r) == 1) {
                    g = r;
                    b = r;
                    parsed = true;
                }
                
                if (parsed) {
                    outProps.envMapTint[0] = r;
                    outProps.envMapTint[1] = g;
                    outProps.envMapTint[2] = b;
                    
                    bool isDefaultTint = (fabs(r - 1.0f) < 0.01f && fabs(g - 1.0f) < 0.01f && fabs(b - 1.0f) < 0.01f);
                    
                    if (!isDefaultTint) {
                        outProps.hasEnvMapTint = true;
                        if (!outProps.hasEnvMap) {
                            outProps.hasEnvMap = true;
                            if (m_debugOutput) {
                                Msg("[MaterialPipeline::ToPBR] %s: Setting hasEnvMap=true based on non-default $envmaptint\n", materialName.c_str());
                            }
                        }
                    }
                    
                    if (m_debugOutput) {
                        Msg("[MaterialPipeline::ToPBR] %s: $envmaptint = [%.2f %.2f %.2f]%s\n", 
                            materialName.c_str(), r, g, b, isDefaultTint ? " (default, ignoring)" : "");
                    }
                } else if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] %s: $envmaptint found but parse failed: '%s'\n", materialName.c_str(), strVal);
                }
            } else if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: $envmaptint found but empty value\n", materialName.c_str());
            }
        } else if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $envmaptint not found by FindVar\n", materialName.c_str());
        }
    }
    
    // Get $selfillum - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasSelfIllum) {
        outProps.isSelfIllum = (vmtParsed.selfIllum != 0);
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$selfillum", &found, false);
        if (found && pVar) {
            outProps.isSelfIllum = (pVar->GetIntValue() == 1);
        }
    }
    
    // =========================================================================
    // NEW: Copy additional VMT-parsed properties for comprehensive PBR extraction
    // =========================================================================
    
    // $selfillummask - separate emissive mask texture
    if (hasVMTParsed && vmtParsed.hasSelfIllumMask && IsValidTexturePath(vmtParsed.selfIllumMask)) {
        outProps.selfIllumMaskPath = vmtParsed.selfIllumMask;
        outProps.hasSelfIllumMask = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $selfillummask (from VMT) = %s\n", materialName.c_str(), outProps.selfIllumMaskPath.c_str());
        }
    }
    
    // $selfillumtint - tint color for self-illumination
    if (hasVMTParsed && vmtParsed.hasSelfIllumTint) {
        outProps.selfIllumTint[0] = vmtParsed.selfIllumTint[0];
        outProps.selfIllumTint[1] = vmtParsed.selfIllumTint[1];
        outProps.selfIllumTint[2] = vmtParsed.selfIllumTint[2];
        outProps.hasSelfIllumTint = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $selfillumtint (from VMT) = [%.2f %.2f %.2f]\n", 
                materialName.c_str(), outProps.selfIllumTint[0], outProps.selfIllumTint[1], outProps.selfIllumTint[2]);
        }
    }
    
    // Rim lighting properties
    if (hasVMTParsed && vmtParsed.hasRimLight) {
        outProps.hasRimLight = (vmtParsed.rimLight != 0);
        if (m_debugOutput && outProps.hasRimLight) {
            Msg("[MaterialPipeline::ToPBR] %s: $rimlight=1 (from VMT)\n", materialName.c_str());
        }
    }
    if (hasVMTParsed && vmtParsed.hasRimLightExponent) {
        outProps.rimLightExponent = vmtParsed.rimLightExponent;
        outProps.hasRimLightExponent = true;
    }
    if (hasVMTParsed && vmtParsed.hasRimLightBoost) {
        outProps.rimLightBoost = vmtParsed.rimLightBoost;
        outProps.hasRimLightBoost = true;
    }
    
    // Additional phong properties
    if (hasVMTParsed && vmtParsed.hasPhongAlbedoTint) {
        outProps.phongAlbedoTint = (vmtParsed.phongAlbedoTint != 0);
    }
    if (hasVMTParsed && vmtParsed.hasPhongAlbedoBoost) {
        outProps.phongAlbedoBoost = vmtParsed.phongAlbedoBoost;
        outProps.hasPhongAlbedoBoost = true;
    }
    if (hasVMTParsed && vmtParsed.hasPhongTint) {
        outProps.phongTint[0] = vmtParsed.phongTint[0];
        outProps.phongTint[1] = vmtParsed.phongTint[1];
        outProps.phongTint[2] = vmtParsed.phongTint[2];
        outProps.hasPhongTint = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $phongtint (from VMT) = [%.2f %.2f %.2f]\n", 
                materialName.c_str(), outProps.phongTint[0], outProps.phongTint[1], outProps.phongTint[2]);
        }
    }
    
    // Parallax/heightmap
    if (hasVMTParsed && vmtParsed.hasParallaxMap && IsValidTexturePath(vmtParsed.parallaxMap)) {
        outProps.parallaxMapPath = vmtParsed.parallaxMap;
        outProps.hasParallaxMap = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $parallaxmap (from VMT) = %s\n", materialName.c_str(), outProps.parallaxMapPath.c_str());
        }
    }
    if (hasVMTParsed && vmtParsed.hasParallaxMapScale) {
        outProps.parallaxMapScale = vmtParsed.parallaxMapScale;
        outProps.hasParallaxMapScale = true;
    }
    
    // Additional envmap properties
    if (hasVMTParsed && vmtParsed.hasEnvMapContrast) {
        outProps.envMapContrast = vmtParsed.envMapContrast;
        outProps.hasEnvMapContrast = true;
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: $envmapcontrast (from VMT) = %.2f\n", materialName.c_str(), outProps.envMapContrast);
        }
    }
    if (hasVMTParsed && vmtParsed.hasEnvMapSaturation) {
        outProps.envMapSaturation = vmtParsed.envMapSaturation;
        outProps.hasEnvMapSaturation = true;
    }
    
    // =========================================================================
    // ExoPBR community PBR format detection
    // ExoPBR provides direct PBR textures - ARM map, normal, emission
    // =========================================================================
    if (hasVMTParsed && vmtParsed.isExoPBR) {
        outProps.isExoPBR = true;
        
        if (vmtParsed.hasTexture1 && IsValidTexturePath(vmtParsed.texture1)) {
            outProps.armTexturePath = vmtParsed.texture1;
            outProps.hasARMTexture = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [ExoPBR] ARM texture = %s\n", materialName.c_str(), vmtParsed.texture1.c_str());
            }
        }
        
        if (vmtParsed.hasTexture2 && IsValidTexturePath(vmtParsed.texture2)) {
            outProps.exoNormalPath = vmtParsed.texture2;
            outProps.hasExoNormal = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [ExoPBR] Normal texture = %s\n", materialName.c_str(), vmtParsed.texture2.c_str());
            }
        }
        
        if (vmtParsed.hasTexture3 && IsValidTexturePath(vmtParsed.texture3)) {
            outProps.emissionTexturePath = vmtParsed.texture3;
            outProps.hasEmissionTexture = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [ExoPBR] Emission texture = %s\n", materialName.c_str(), vmtParsed.texture3.c_str());
            }
        }
        
        if (vmtParsed.hasEmissionScale) {
            outProps.emissionScale = vmtParsed.emissionScale;
            outProps.hasEmissionScale = true;
        }
        
        if (vmtParsed.hasEmissionTint) {
            outProps.emissionTint[0] = vmtParsed.emissionTint[0];
            outProps.emissionTint[1] = vmtParsed.emissionTint[1];
            outProps.emissionTint[2] = vmtParsed.emissionTint[2];
            outProps.hasEmissionTint = true;
        }
        
        // ExoPBR materials have direct PBR data - set roughness and metallic to use textures
        // The ARM map will be split in CreatePBRMaterial
        outProps.roughness = 0.5f;  // Default, will be overridden by ARM map
        outProps.metallic = 0.0f;   // Default, will be overridden by ARM map
        
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: [ExoPBR] Material detected - using direct PBR path\n", materialName.c_str());
        }
    }
    
    // =========================================================================
    // GPBR (Strata Source) community PBR format detection
    // GPBR provides direct PBR textures - MRAO map, normal, emission
    // =========================================================================
    if (hasVMTParsed && vmtParsed.isGPBR) {
        outProps.isGPBR = true;
        
        // Copy MRAO texture path
        if (vmtParsed.hasMRAOTexture && IsValidTexturePath(vmtParsed.mraoTexture)) {
            outProps.mraoTexturePath = vmtParsed.mraoTexture;
            outProps.hasMRAOTexture = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [GPBR] MRAO texture = %s\n", materialName.c_str(), vmtParsed.mraoTexture.c_str());
            }
        }
        
        // MRAO scale
        if (vmtParsed.hasMRAOScale) {
            outProps.mraoScale = vmtParsed.mraoScale;
            outProps.hasMRAOScale = true;
        }
        
        // Emission texture
        if (vmtParsed.hasGPBREmissionTexture && IsValidTexturePath(vmtParsed.gpbrEmissionTexture)) {
            outProps.gpbrEmissionPath = vmtParsed.gpbrEmissionTexture;
            outProps.hasGPBREmission = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [GPBR] Emission texture = %s\n", materialName.c_str(), vmtParsed.gpbrEmissionTexture.c_str());
            }
        }
        
        // Emission scale
        if (vmtParsed.hasGPBREmissionScale) {
            outProps.gpbrEmissionScale = vmtParsed.gpbrEmissionScale;
            outProps.hasGPBREmissionScale = true;
        }
        
        // Parallax settings
        if (vmtParsed.hasGPBRParallax) {
            outProps.gpbrParallax = vmtParsed.gpbrParallax;
        }
        if (vmtParsed.hasGPBRParallaxDepth) {
            outProps.gpbrParallaxDepth = vmtParsed.gpbrParallaxDepth;
        }
        
        // Alpha transparency
        if (vmtParsed.hasGPBRAlpha) {
            outProps.gpbrAlpha = vmtParsed.gpbrAlpha;
            outProps.hasGPBRAlpha = true;
        }
        
        // GPBR materials have direct PBR data
        outProps.roughness = 0.5f;  // Default, will be overridden by MRAO map
        outProps.metallic = 0.0f;   // Default, will be overridden by MRAO map
        
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: [GPBR] Material detected - using direct PBR path\n", materialName.c_str());
        }
    }
    
    // =========================================================================
    // MWB PBR Gen format detection (must check before BFT - more specific)
    // Uses $phongexponenttexture with special encoding for PBR data
    // =========================================================================
    if (hasVMTParsed && vmtParsed.isMWBPBR) {
        outProps.isMWBPBR = true;
        
        // MWB uses $phongexponenttexture for PBR data
        // Red channel: pow(gloss, 4.0) -> roughness via pow^0.25
        // Green channel: direct metallic value
        if (vmtParsed.hasPhongExponentTexture && IsValidTexturePath(vmtParsed.phongExponentTexture)) {
            outProps.bftExponentTexturePath = vmtParsed.phongExponentTexture;  // Reuse BFT field
            outProps.hasBFTExponentTexture = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [MWB-PBR] Exponent texture = %s\n", 
                    materialName.c_str(), vmtParsed.phongExponentTexture.c_str());
            }
        }
        
        // MWB materials have default mid-range values (textures provide actual data)
        outProps.roughness = 0.5f;  // Will be decoded from exponent texture red channel
        outProps.metallic = 0.0f;   // Will be extracted from exponent green or base alpha
        
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: [MWB-PBR] Material detected - using MWB handler\n", 
                materialName.c_str());
        }
    }
    // =========================================================================
    // BlueFlyTrap PseudoPBR format detection
    // Uses $phongexponenttexture for roughness encoding
    // =========================================================================
    else if (hasVMTParsed && vmtParsed.isBFTPseudoPBR) {
        outProps.isBFTPseudoPBR = true;
        outProps.isBFTMetallicLayer = vmtParsed.isBFTMetallicLayer;
        
        // The exponent texture contains the roughness info (inverted)
        if (vmtParsed.hasPhongExponentTexture && IsValidTexturePath(vmtParsed.phongExponentTexture)) {
            outProps.bftExponentTexturePath = vmtParsed.phongExponentTexture;
            outProps.hasBFTExponentTexture = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: [BFT] Exponent texture = %s\n", 
                    materialName.c_str(), vmtParsed.phongExponentTexture.c_str());
            }
        }
        
        // Set metallic based on layer type
        if (outProps.isBFTMetallicLayer) {
            outProps.metallic = 0.9f;  // High metallic for metallic layers
        } else {
            outProps.metallic = 0.0f;  // Non-metallic for base layers
        }
        
        outProps.roughness = 0.5f;  // Will be overridden by exponent texture
        
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: [BFT] Material detected - %s layer\n", 
                materialName.c_str(), outProps.isBFTMetallicLayer ? "metallic" : "base");
        }
    }
    
    // =========================================================================
    // END: Additional VMT-parsed properties
    // =========================================================================
    
    // Get $translucent - prefer VMT-parsed value
    if (hasVMTParsed && vmtParsed.hasTranslucent) {
        outProps.isTranslucent = vmtParsed.translucent;
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$translucent", &found, false);
        if (found && pVar) {
            outProps.isTranslucent = (pVar->GetIntValue() == 1);
        }
    }
    
    // Get $surfaceprop - prefer VMT-parsed value
    if (hasVMTParsed && !vmtParsed.surfaceProp.empty()) {
        outProps.surfaceProp = vmtParsed.surfaceProp;
    } else {
        IMaterialVar* pVar = pMaterial->FindVar("$surfaceprop", &found, false);
        if (found && pVar) {
            const char* surfaceVal = pVar->GetStringValue();
            if (surfaceVal && surfaceVal[0] != '\0') {
                outProps.surfaceProp = surfaceVal;
            }
        }
    }
    
    // Glass detection: Material is considered glass if:
    // 1. Shader is "Refract" (always glass), OR
    // 2. VMT has $refractamount (indicates Refract shader even if FindVar says otherwise), OR
    // 3. $surfaceprop = "glass" AND $translucent = 1 (surfaceprop alone is for physics, needs translucent)
    // NOTE: $surfaceprop "glass" alone is NOT enough - materials like nukwindowa have it for physics sounds
    //       but are not meant to be transparent. They need $translucent=1 to actually be glass.
    {
        // Check if shader name is exactly "Refract" (case-insensitive).
        // Substring matching is intentionally avoided so shaders like "EyeRefract"
        // do not get treated as refractive glass.
        bool isRefractShader = false;
        if (!outProps.shaderName.empty()) {
            std::string shaderLower = outProps.shaderName;
            std::transform(shaderLower.begin(), shaderLower.end(), shaderLower.begin(), ::tolower);
            isRefractShader = (shaderLower == "refract");
        }
        
        // Check if surfaceprop is glass
        bool isSurfaceGlass = false;
        if (!outProps.surfaceProp.empty()) {
            std::string surfaceLower = outProps.surfaceProp;
            std::transform(surfaceLower.begin(), surfaceLower.end(), surfaceLower.begin(), ::tolower);
            isSurfaceGlass = (surfaceLower == "glass" || surfaceLower.find("glass") != std::string::npos);
        }
        
        // We already parsed the VMT file at the start - reuse that data
        // Check VMT shader name and $refractamount
        bool vmtIsRefract = false;
        bool vmtHasRefractAmount = false;
        if (hasVMTParsed) {
            if (!vmtParsed.shaderName.empty()) {
                std::string vmtShaderLower = vmtParsed.shaderName;
                std::transform(vmtShaderLower.begin(), vmtShaderLower.end(), vmtShaderLower.begin(), ::tolower);
                vmtIsRefract = (vmtShaderLower == "refract");
            }
            vmtHasRefractAmount = vmtParsed.hasRefractAmount;
            
            // Get $refracttinttexture if present
            if (vmtParsed.hasRefractTintTexture && !vmtParsed.refractTintTexture.empty()) {
                outProps.refractTintTexturePath = vmtParsed.refractTintTexture;
            }
        }
        
        // Track if this is a Refract shader (important for transmittance texture handling)
        if (isRefractShader || vmtIsRefract) {
            outProps.isRefractShader = true;
        }
        
        // Determine if this is a glass material
        // Be CONSERVATIVE - only mark as glass if we're SURE it's meant to be refractive glass
        // NOT just any translucent material with reflections (like doors with glass cutouts)
        if (isRefractShader || vmtIsRefract) {
            outProps.isGlass = true;  // Refract shader is always glass
        } else if (vmtHasRefractAmount) {
            outProps.isGlass = true;  // Has $refractamount means it's a refractive material
        } else if (isSurfaceGlass && outProps.isTranslucent) {
            // surfaceprop=glass ONLY triggers glass shader if ALSO translucent
            // Materials like nukwindowa have $surfaceprop "glass" but no $translucent
            // They're just textures with glass surface properties for physics/sounds
            outProps.isGlass = true;
        }
        // NOTE: We REMOVED the "translucent + envmap = glass" heuristic because it catches
        // too many materials that aren't glass (like doors with glass cutouts, windows with frames, etc.)
        // Those materials have $translucent for alpha blending, not for glass refraction.
        // NOTE 2: $surfaceprop alone is NOT enough - it's just for physics. Need $translucent too.
        
        if (m_debugOutput) {
            if (outProps.isGlass) {
                Msg("[MaterialPipeline::ToPBR] %s: DETECTED AS GLASS (shader=%s, vmtShader=%s, vmtRefract=%d, translucent=%d, surfaceprop=%s, hasEnvMap=%d)\n",
                    materialName.c_str(), outProps.shaderName.c_str(), 
                    hasVMTParsed ? vmtParsed.shaderName.c_str() : "N/A",
                    vmtHasRefractAmount, outProps.isTranslucent, outProps.surfaceProp.c_str(), outProps.hasEnvMap);
            } else if (isSurfaceGlass || isRefractShader || vmtIsRefract || vmtHasRefractAmount) {
                // This shouldn't happen, but log it for debugging
                Msg("[MaterialPipeline::ToPBR] %s: GLASS DETECTION FAILED - shader=%s, isRefract=%d, vmtIsRefract=%d, vmtRefract=%d, isSurfaceGlass=%d, translucent=%d, hasEnvMap=%d\n",
                    materialName.c_str(), outProps.shaderName.c_str(), isRefractShader, vmtIsRefract, vmtHasRefractAmount, isSurfaceGlass, outProps.isTranslucent, outProps.hasEnvMap);
            }
        }
    }
    
    // Analyze base texture brightness for metallic detection (only if material has envmap)
    // Black textures + envmap = metallic materials (like chrome)
    // Grey/colored textures + envmap = non-metallic with reflections
    if (outProps.hasEnvMap && !outProps.baseTexturePath.empty() && !outProps.isGlass) {
        float brightness = 0.5f;
        if (AnalyzeBaseTextureBrightness(outProps.baseTexturePath, brightness)) {
            outProps.baseTextureBrightness = brightness;
            outProps.hasBaseTextureBrightness = true;
            
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Base texture brightness=%.3f (for metallic detection)\n",
                    materialName.c_str(), brightness);
            }
        }
    }
    
    // Auto-discover companion textures that aren't explicitly referenced in the VMT
    // This helps find textures that follow naming conventions (e.g., _normal, _mask, _spec)
    if (m_autoDiscoverEnabled && !outProps.baseTexturePath.empty()) {
        DiscoverCompanionTextures(outProps.baseTexturePath, outProps);
        
        // If we discovered a normal map and don't have one yet, use it
        if (outProps.hasDiscoveredNormal && !outProps.hasBumpMap) {
            outProps.bumpMapPath = outProps.discoveredNormalPath;
            outProps.hasBumpMap = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Using auto-discovered normal map: %s\n", 
                    materialName.c_str(), outProps.discoveredNormalPath.c_str());
            }
        }
        
        // If we discovered a height map and don't have one yet, use it
        if (outProps.hasDiscoveredHeight && !outProps.hasParallaxMap) {
            outProps.parallaxMapPath = outProps.discoveredHeightPath;
            outProps.hasParallaxMap = true;
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: Using auto-discovered height map: %s\n", 
                    materialName.c_str(), outProps.discoveredHeightPath.c_str());
            }
        }
    }
    
    // Calculate PBR values with enhanced logic
    // Skip Source Engine heuristics for direct-PBR formats (ExoPBR, GPBR) - they already
    // set their own appropriate defaults (roughness=0.5, metallic=0.0) and rely on
    // texture maps from their ARM/MRAO textures rather than phong/envmap heuristics.
    // Without this guard, CalculateRoughness() returns 1.0 (fully matte) for these formats
    // because they have no $phong or $envmap, overwriting the correct 0.5 default.
    if (!outProps.isExoPBR && !outProps.isGPBR) {
        outProps.roughness = CalculateRoughness(outProps);
        outProps.metallic = EstimateMetallic(outProps);
    }
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] %s extracted: hasPhong=%d, phongExp=%.0f, hasBump=%d, hasEnvMask=%d, hasPhongExpTex=%d, hasEnvMapTint=%d, hasEnvMap=%d, normMapAlpha=%d, isGlass=%d\n",
            materialName.c_str(), outProps.hasPhong, outProps.phongExponent, outProps.hasBumpMap, 
            outProps.hasEnvMapMask, outProps.hasPhongExponentTexture, outProps.hasEnvMapTint, outProps.hasEnvMap, outProps.normalMapAlphaEnvMapMask, outProps.isGlass);
    }
    
    return true;
}


// Helper to create ProcessingContext for modular format handlers
ProcessingContext TextureProcessor::CreateProcessingContext() {
    ProcessingContext ctx;
    
    // Bind member functions to the context
    ctx.readVTFFile = [this](const std::string& path, std::vector<uint8_t>& data) {
        return ReadVTFFile(path, data);
    };
    ctx.parseVTFHeader = [this](const std::vector<uint8_t>& data, VTFFileHeader& header) {
        return ParseVTFHeader(data, header);
    };
    ctx.extractPixelData = [this](const std::vector<uint8_t>& data, const VTFFileHeader& header, 
                                   ConvertedTexture& tex, bool convertNormal) {
        return ExtractVTFPixelData(data, header, tex, convertNormal);
    };
    ctx.writeDDS = [this](const ConvertedTexture& tex, const std::string& path) {
        return WriteTextureToDDS(tex, path);
    };
    ctx.convertToOctahedral = [this](ConvertedTexture& tex) {
        ConvertNormalMapToOctahedral(tex);
    };
    ctx.convertSSBumpToNormal = [this](ConvertedTexture& tex) {
        ConvertSSBumpToNormal(tex);
    };
    ctx.generateOutputPath = [this](const std::string& sourceName, const std::string& suffix) {
        return GenerateOutputPath(sourceName, suffix);
    };
    ctx.fileExists = [this](const std::string& path) {
        return FileExists(path);
    };
    
    ctx.debugOutput = m_debugOutput;
    ctx.metallicGenerationEnabled = m_metallicGenerationEnabled;  // Pass metallic generation flag
    ctx.materialsWithNormals = &m_stats.materialsWithNormals;
    ctx.materialsWithRoughness = &m_stats.materialsWithRoughness;
    
    return ctx;
}

// Helper to copy ProcessedMaterial results to ProcessedMaterialInfo
static void CopyProcessedMaterial(const ProcessedMaterial& src, TextureProcessor::ProcessedMaterialInfo& dst) {
    if (!src.normalPath.empty()) dst.normalPath = src.normalPath;
    if (!src.roughnessPath.empty()) dst.roughnessPath = src.roughnessPath;
    if (!src.metallicPath.empty()) dst.metallicPath = src.metallicPath;
    if (!src.heightPath.empty()) {
        dst.heightPath = src.heightPath;
        dst.heightScale = src.heightScale;
    }
    if (!src.emissivePath.empty()) {
        dst.emissivePath = src.emissivePath;
        dst.emissionIntensity = src.emissionIntensity;
    }
    if (!src.transmittancePath.empty()) dst.transmittancePath = src.transmittancePath;
    dst.enableTransmissionMask = src.enableTransmissionMask;
    if (!src.albedoPath.empty()) dst.albedoPath = src.albedoPath;
    if (src.roughnessConstant != 0.5f) dst.roughnessConstant = src.roughnessConstant;
    if (src.metallicConstant != 0.0f) dst.metallicConstant = src.metallicConstant;
    if (src.isGlass) {
        dst.isGlass = src.isGlass;
        dst.ior = src.ior;
    }
}

// Preserve the Source color texture when replacing glass with Remix's
// translucent material. Refract shaders use $refracttinttexture for color;
// their $basetexture may be a normal/distortion map and is deliberately not
// used as a fallback. Other glass materials retain their original
// $basetexture as the transmittance texture. Water is explicitly excluded:
// its texture inputs describe animated surface normals/distortion, not a
// color or opacity mask for light transmission.
static void AddGlassTransmittance(const MaterialPBRProperties& props,
                                  const ProcessingContext& ctx,
                                  ProcessedMaterial& result) {
    if (!props.isGlass || props.isWater || !result.transmittancePath.empty()) {
        return;
    }

    result.isGlass = true;
    result.ior = 1.52f;

    const std::string& sourcePath = props.isRefractShader
        ? props.refractTintTexturePath
        : props.baseTexturePath;
    if (sourcePath.empty()) {
        if (ctx.debugOutput) {
            Msg("[MaterialPipeline::ToPBR] %s: glass has no color texture for transmittance\n",
                props.materialName.c_str());
        }
        return;
    }

    const std::string outputPath =
        ctx.generateOutputPath(sourcePath, "_transmittance");
    const bool outputExists = ctx.fileExists(outputPath);

    std::vector<uint8_t> fileData;
    VTFFileHeader header;
    ConvertedTexture texture;
    texture.isNormalMap = false;
    const bool textureDecoded =
        ctx.readVTFFile(sourcePath, fileData) &&
        ctx.parseVTFHeader(fileData, header) &&
        ctx.extractPixelData(fileData, header, texture, false);

    if (!textureDecoded) {
        if (outputExists) {
            // Preserve the existing successful conversion, but leave the new mask
            // disabled because its alpha semantics could not be classified safely.
            result.transmittancePath = outputPath;
            result.skippedCount++;
            if (ctx.debugOutput) {
                Msg("[MaterialPipeline::ToPBR] %s: reusing transmittance texture without shadow mask; source alpha could not be inspected\n",
                    props.materialName.c_str());
            }
            return;
        }

        Warning("[MaterialPipeline::ToPBR] %s: failed to preserve glass color texture '%s'\n",
            props.materialName.c_str(), sourcePath.c_str());
        return;
    }

    // VTF decoders fill textures without meaningful alpha with 255. Enable the
    // shadow mask only when at least one pixel transmits some light, which also
    // supports uniform partial alpha while protecting ordinary opaque RGB maps.
    for (size_t i = 3; i < texture.pixelData.size(); i += 4) {
        if (texture.pixelData[i] < 255) {
            result.enableTransmissionMask = true;
            break;
        }
    }

    if (!outputExists && !ctx.writeDDS(texture, outputPath)) {
        Warning("[MaterialPipeline::ToPBR] %s: failed to write glass transmittance texture '%s'\n",
            props.materialName.c_str(), outputPath.c_str());
        return;
    }

    result.transmittancePath = outputPath;
    if (ctx.debugOutput) {
        Msg("[MaterialPipeline::ToPBR] %s: %s transmittance texture %s%s\n",
            props.materialName.c_str(), outputExists ? "reused" : "wrote", outputPath.c_str(),
            result.enableTransmissionMask ? " [transmission mask]" : "");
    }
    if (outputExists) {
        result.skippedCount++;
    }
}

// Animated materials render one D3D texture per frame, each with its own
// Remix hash. Collect them all so every frame receives the PBR override.
static std::vector<uint64_t> CollectMaterialFrameHashes(const std::string& materialName) {
    std::vector<uint64_t> hashes;
    auto variants =
        D3D9TextureTracker::Instance().GetTextureVariantsForMaterial(materialName.c_str());
    for (IDirect3DTexture9* tex : variants) {
        if (!tex) continue;
        auto result = g_remix->dxvk_GetTextureHash(tex);
        if (result && result.value() != 0) {
            uint64_t hash = result.value();
            if (std::find(hashes.begin(), hashes.end(), hash) == hashes.end()) {
                hashes.push_back(hash);
            }
        }
    }
    return hashes;
}

void TextureProcessor::RegisterProcessedMaterial(const std::vector<uint64_t>& hashes,
                                                 ProcessedMaterialInfo info) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (uint64_t hash : hashes) {
        info.textureHash = hash;
        m_processedMaterialInfo[hash] = info;
    }
    m_stats.materialsProcessed++;
}

bool TextureProcessor::RegisterChromeMaterial(uint64_t hash) {
    if (!m_initialized || hash == 0) {
        return false;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_processedMaterialInfo.count(hash) ||
            m_materialsWrittenToUSDA.count(hash)) {
            return false;
        }
    }

    ProcessedMaterialInfo info{};
    info.textureHash = hash;
    info.roughnessConstant = 0.05f;
    info.metallicConstant = 1.0f;
    info.heightScale = 0.025f;
    info.ior = 1.0f;
    info.emissionIntensity = 1.0f;
    RegisterProcessedMaterial({ hash }, info);

    m_needsUSDAUpdate = true;
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Registered chrome override for hash 0x%llX\n",
            (unsigned long long)hash);
    }
    return true;
}

bool TextureProcessor::CreatePBRMaterial(const MaterialPBRProperties& props, const std::vector<uint64_t>& textureHashes) {
    // Partition into hashes that still need a USDA entry. If a sibling frame
    // was already processed this session, reuse its maps for the new frames.
    std::vector<uint64_t> newHashes;
    ProcessedMaterialInfo siblingInfo;
    bool haveSiblingInfo = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (uint64_t hash : textureHashes) {
            if (hash == 0) continue;
            auto it = m_processedMaterialInfo.find(hash);
            if (it != m_processedMaterialInfo.end()) {
                siblingInfo = it->second;
                haveSiblingInfo = true;
                continue;
            }
            if (m_materialsWrittenToUSDA.find(hash) != m_materialsWrittenToUSDA.end()) {
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] Skipping hash 0x%llX - already in USDA from previous session\n", 
                        (unsigned long long)hash);
                }
                continue;
            }
            newHashes.push_back(hash);
        }
    }
    
    if (newHashes.empty()) {
        return !textureHashes.empty();
    }
    
    if (haveSiblingInfo) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (uint64_t hash : newHashes) {
            siblingInfo.textureHash = hash;
            m_processedMaterialInfo[hash] = siblingInfo;
        }
        return true;
    }
    
    uint64_t textureHash = newHashes[0];
    
    // Ensure output directory exists for texture files
    if (!EnsureOutputDirectory()) {
        Warning("[MaterialPipeline::ToPBR] Cannot create output directory, skipping material\n");
        return false;
    }
    
    // Track material info for USDA generation
    ProcessedMaterialInfo matInfo{};
    matInfo.textureHash = textureHash;
    matInfo.roughnessConstant = props.roughness;
    matInfo.metallicConstant = props.metallic;
    matInfo.heightScale = 0.025f;  // Default height scale
    matInfo.isGlass = props.isGlass;
    matInfo.thinWalled = props.isGlass && !props.isWater;
    matInfo.isRefractShader = props.isRefractShader;
    matInfo.hasVolumeTransmittance = props.isWater;
    matInfo.transmittanceColor[0] = 0.3f;
    matInfo.transmittanceColor[1] = 0.65f;
    matInfo.transmittanceColor[2] = 0.86f;
    matInfo.transmittanceMeasurementDistance = 800.0f;
    // Match the hand-authored base_mod water replacements. Glass uses the
    // crown/window-glass IOR while water uses its physical IOR.
    matInfo.ior = props.isWater ? 1.33f : (props.isGlass ? 1.52f : 1.0f);
    matInfo.emissionIntensity = props.hasEmissionScale ? props.emissionScale : 1.0f;
    
    // Create processing context for modular handlers
    ProcessingContext ctx = CreateProcessingContext();
    
    // =========================================================================
    // Delegate to format-specific handlers (defined in separate files)
    // Priority: ExoPBR -> GPBR -> BFT -> SourceEngine (fallback)
    // =========================================================================
    
    // ExoPBR format
    if (props.isExoPBR) {
        ProcessedMaterial result = ExoPBR::ProcessTextures(props, textureHash, ctx);
        if (result.success) {
            AddGlassTransmittance(props, ctx, result);
            CopyProcessedMaterial(result, matInfo);
            RegisterProcessedMaterial(newHashes, matInfo);
            return true;
        }
    }
    
    // GPBR format
    if (props.isGPBR) {
        ProcessedMaterial result = GPBR::ProcessTextures(props, textureHash, ctx);
        if (result.success) {
            AddGlassTransmittance(props, ctx, result);
            CopyProcessedMaterial(result, matInfo);
            RegisterProcessedMaterial(newHashes, matInfo);
            return true;
        }
    }
    
    // MWB PBR Gen format (must check before BFT - it has more specific patterns)
    if (props.isMWBPBR) {
        ProcessedMaterial result = MWBPBR::ProcessTextures(props, textureHash, ctx);
        if (result.success) {
            AddGlassTransmittance(props, ctx, result);
            CopyProcessedMaterial(result, matInfo);
            RegisterProcessedMaterial(newHashes, matInfo);
            return true;
        }
    }
    
    // BlueFlyTrap PseudoPBR format
    if (props.isBFTPseudoPBR) {
        ProcessedMaterial result = BFTPseudoPBR::ProcessTextures(props, textureHash, ctx);
        if (result.success) {
            AddGlassTransmittance(props, ctx, result);
            CopyProcessedMaterial(result, matInfo);
            RegisterProcessedMaterial(newHashes, matInfo);
            return true;
        }
    }
    
    // =========================================================================
    // Standard Source Engine material processing (fallback)
    // For non-PBR format materials, use the SourceEngine handler
    // =========================================================================
    ProcessedMaterial result = SourceEngine::ProcessTextures(props, textureHash, ctx);
    if (result.success) {
        AddGlassTransmittance(props, ctx, result);
        CopyProcessedMaterial(result, matInfo);
        
        // Store for USDA generation (thread-safe)
        RegisterProcessedMaterial(newHashes, matInfo);
        
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Processed material '%s' (hash 0x%llX, %zu frames): roughness=%.2f, metallic=%.2f%s%s%s%s\n",
                props.materialName.c_str(), textureHash, newHashes.size(), matInfo.roughnessConstant, matInfo.metallicConstant,
                !matInfo.normalPath.empty() ? " [normal]" : "",
                !matInfo.roughnessPath.empty() ? " [roughness]" : "",
                !matInfo.metallicPath.empty() ? " [metallic]" : "",
                !matInfo.heightPath.empty() ? " [height]" : "");
        }
        
        return true;
    }
    
    return false;
}

int TextureProcessor::ProcessAllTrackedMaterials() {
    // Process all materials without limit (legacy behavior)
    return ProcessTrackedMaterialsBatch(0);
}

int TextureProcessor::ProcessTrackedMaterialsBatch(int maxBatch) {
    if (!m_enabled.load(std::memory_order_relaxed)) {
        return 0;
    }
    
    // LOCK-FREE FAST PATH: Check if we know everything is processed without taking any locks
    // This atomic load has zero contention and returns instantly
    if (m_allMaterialsProcessed.load(std::memory_order_relaxed)) {
        return 0;
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (!m_initialized) {
        Warning("[MaterialPipeline::ToPBR] Not initialized\n");
        return 0;
    }
    
    auto afterLock = std::chrono::high_resolution_clock::now();
    
    // Check if material count changed before expensive GetCachedMaterials() call
    size_t currentCount = D3D9TextureTracker::Instance().GetCacheSize();
    
    // Early exit: if count hasn't changed and we processed everything, nothing new to do
    if (currentCount == m_lastKnownMaterialCount && m_processedMaterials.size() >= currentCount) {
        // Set the lock-free flag so next time we don't even take the lock
        m_allMaterialsProcessed.store(true, std::memory_order_relaxed);
        
        auto endTime = std::chrono::high_resolution_clock::now();
        if (m_debugOutput) {
            auto lockTime = std::chrono::duration_cast<std::chrono::microseconds>(afterLock - startTime).count();
            auto totalTime = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
            Msg("[MaterialPipeline::ToPBR] Batch: early exit, set fast-path flag (lock: %lld us, total: %lld us)\n", lockTime, totalTime);
        }
        return 0;
    }
    
    // Material count changed - clear the flag so we check again next time
    m_allMaterialsProcessed.store(false, std::memory_order_relaxed);
    
    auto afterCheck = std::chrono::high_resolution_clock::now();
    
    // Count changed or there might be unprocessed materials - do the expensive work
    std::vector<std::string> cachedMaterials = D3D9TextureTracker::Instance().GetCachedMaterials();
    m_lastKnownMaterialCount = cachedMaterials.size();
    
    auto afterGetMaterials = std::chrono::high_resolution_clock::now();
    
    if (m_debugOutput) {
        auto lockTime = std::chrono::duration_cast<std::chrono::microseconds>(afterLock - startTime).count();
        auto checkTime = std::chrono::duration_cast<std::chrono::microseconds>(afterCheck - afterLock).count();
        auto getMaterialsTime = std::chrono::duration_cast<std::chrono::microseconds>(afterGetMaterials - afterCheck).count();
        Msg("[MaterialPipeline::ToPBR] Batch timings: lock=%lld us, check=%lld us, getMaterials=%lld us\n",
            lockTime, checkTime, getMaterialsTime);
    }
    
    int processedCount = 0;
    int skippedCount = 0;
    
    for (const std::string& matName : cachedMaterials) {
        // Check batch limit (0 = no limit)
        if (maxBatch > 0 && processedCount >= maxBatch) {
            break;
        }
        
        // Skip already processed
        if (m_processedMaterials.find(matName) != m_processedMaterials.end()) {
            skippedCount++;
            // Early exit optimization: if we've skipped many materials in a row, likely nothing new
            if (skippedCount > 100 && processedCount == 0) {
                // We've checked 100 materials and found nothing new - probably nothing left
                break;
            }
            continue;
        }
        
        // Reset skip counter when we find something unprocessed
        skippedCount = 0;
        
        // Skip engine-generated and 2D interface materials.
        if (MaterialFilter::IsNonSceneMaterialName(matName)) {
            m_processedMaterials.insert(matName);
            continue;
        }
        
        // Extract PBR properties
        MaterialPBRProperties props;
        if (!ExtractMaterialPBR(matName, props)) {
            // Mark as processed even if it failed - don't check it again
            m_processedMaterials.insert(matName);
            continue;
        }
        
        // Only process materials with PBR-relevant data
        // Include materials with roughness texture sources OR envmap/envmaptint (which implies reflectivity)
        if (!props.hasBumpMap && !props.hasPhong && !props.hasEnvMapMask && 
            !props.normalMapAlphaEnvMapMask && !props.hasPhongExponentTexture && 
            !props.hasBaseMapAlphaPhongMask && !props.hasBaseAlphaEnvMapMask &&
            !props.hasEnvMap && !props.hasEnvMapTint && !props.isGlass) {
            m_processedMaterials.insert(matName);
            continue;
        }
        
        // Get all frame hashes from the tracker (animated textures have one per frame)
        std::vector<uint64_t> frameHashes = CollectMaterialFrameHashes(matName);
        
        if (frameHashes.empty()) {
            continue;
        }
        
        props.baseTextureHash = frameHashes[0];
        
        // Create PBR material (generates textures and tracks info for USDA)
        if (CreatePBRMaterial(props, frameHashes)) {
            processedCount++;
        }
        
        m_processedMaterials.insert(matName);
        m_processedFrameCounts[matName] = frameHashes.size();
    }
    
    if (processedCount > 0) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] Processed %d materials with PBR properties\n", processedCount);
            Msg("[MaterialPipeline::ToPBR] Stats: %d with normals, %d with roughness textures\n", 
                m_stats.materialsWithNormals, m_stats.materialsWithRoughness);
        }
        
        // Flag that USDA needs updating - caller must call WriteUSDAIfNeeded() to actually write
        m_needsUSDAUpdate = true;
    }
    
    return processedCount;
}

bool TextureProcessor::IsMaterialProcessed(const std::string& materialName) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_processedMaterials.find(materialName) != m_processedMaterials.end();
}

TextureProcessor::Stats TextureProcessor::GetStats() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_stats;
}

void TextureProcessor::ClearCache() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // Clear all tracking data for reprocessing.
    // Note: USDA files remain on disk and need game restart to reload.
    m_processedMaterials.clear();
    m_processedFrameCounts.clear();
    m_uploadedTextures.clear();
    m_processedMaterialInfo.clear();
    m_needsUSDAUpdate = false;
    m_stats = {};
    
    // Clear persistent ineligible cache and delete the file
    m_ineligibleCache.clear();
    m_ineligibleCacheDirty = false;
    std::string cachePath = GetIneligibleCachePath();
    if (!cachePath.empty()) {
        DeleteFileA(cachePath.c_str());
    }
    
    Msg("[MaterialPipeline::ToPBR] Cache cleared (including persistent ineligible cache)\n");
}

bool TextureProcessor::ProcessSingleMaterial(const std::string& materialName) {
    if (!m_enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    if (!m_initialized) {
        return false;
    }
    
    // Skip already processed
    if (m_processedMaterials.find(materialName) != m_processedMaterials.end()) {
        return true; // Already done
    }
    
    // Skip engine-generated and 2D interface materials.
    if (MaterialFilter::IsNonSceneMaterialName(materialName)) {
        return false;
    }
    
    // Extract PBR properties
    MaterialPBRProperties props;
    if (!ExtractMaterialPBR(materialName, props)) {
        m_processedMaterials.insert(materialName);
        m_ineligibleCache.insert(materialName);
        m_ineligibleCacheDirty = true;
        return false;
    }
    
    // Only process materials with PBR-relevant data
    // Include materials with roughness texture sources OR envmap/envmaptint (which implies reflectivity)
    // OR glass materials (which need special shader)
    if (!props.hasBumpMap && !props.hasPhong && !props.hasEnvMapMask && 
        !props.normalMapAlphaEnvMapMask && !props.hasPhongExponentTexture &&
        !props.hasBaseMapAlphaPhongMask && !props.hasBaseAlphaEnvMapMask &&
        !props.hasEnvMap && !props.hasEnvMapTint && !props.isGlass) {
        m_processedMaterials.insert(materialName);
        m_ineligibleCache.insert(materialName);
        m_ineligibleCacheDirty = true;
        return false;
    }
    
    // Get all frame hashes from the tracker (animated textures have one per frame)
    std::vector<uint64_t> frameHashes = CollectMaterialFrameHashes(materialName);
    
    if (frameHashes.empty()) {
        return false;
    }
    
    props.baseTextureHash = frameHashes[0];
    
    // Create PBR material (generates textures and tracks info for USDA)
    bool success = CreatePBRMaterial(props, frameHashes);
    
    m_processedMaterials.insert(materialName);
    m_processedFrameCounts[materialName] = frameHashes.size();
    
    if (success) {
        m_needsUSDAUpdate = true;
    }
    
    return success;
}

void TextureProcessor::OnNewMaterialDetected(const std::string& materialName, uint64_t textureHash) {
    if (!m_initialized || !m_autoProcessing || !m_enabled.load(std::memory_order_relaxed)) {
        return;
    }

    if (MaterialFilter::IsNonSceneMaterialName(materialName)) {
        return;
    }
    
    // Skip already processed - unless this is a new frame hash of an animated
    // material (e.g. water) that finished processing before all frames rendered.
    // Re-queue it: CreatePBRMaterial reuses the sibling frame's maps, so this
    // only registers the new hash for the USDA override.
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_processedMaterials.find(materialName) != m_processedMaterials.end()) {
            const bool isNewFrameHash = textureHash != 0 &&
                m_ineligibleCache.find(materialName) == m_ineligibleCache.end() &&
                m_processedMaterialInfo.find(textureHash) == m_processedMaterialInfo.end() &&
                m_materialsWrittenToUSDA.find(textureHash) == m_materialsWrittenToUSDA.end();
            if (!isNewFrameHash) {
                return;
            }
            m_processedMaterials.erase(materialName);
            if (m_debugOutput) {
                Msg("[MaterialPipeline::ToPBR] New frame hash 0x%llX for processed material '%s' - re-queueing\n",
                    (unsigned long long)textureHash, materialName.c_str());
            }
        }
    }
    
    // Clear the lock-free flag so batch processing will check for new materials
    m_allMaterialsProcessed.store(false, std::memory_order_relaxed);
    
    // Process in background (don't hold up rendering)
    // For now, just mark as needing processing - we'll batch process later
    // The actual processing happens in ProcessAllTrackedMaterials or ProcessSingleMaterial
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] New material detected for auto-processing: %s (hash 0x%llX)\n", 
            materialName.c_str(), textureHash);
    }
}

void TextureProcessor::WriteUSDAIfNeeded() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    
    // Flush ineligible cache to disk if dirty (piggyback on periodic Think hook call)
    SaveIneligibleCache();
    
    if (!m_needsUSDAUpdate || m_processedMaterialInfo.empty()) {
        return;
    }
    
    // Debounce: don't write if we wrote recently and background processing is still active.
    // This prevents Remix from constantly reloading the mod layer mid-processing,
    // which causes driver lockups on large maps.
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastUSDAWriteTime).count();
    if (elapsed < USDA_WRITE_DEBOUNCE_SECONDS && m_backgroundProcessing.load(std::memory_order_relaxed)) {
        return;
    }
    
    if (WriteModUSDA()) {
        m_needsUSDAUpdate = false;
        m_lastUSDAWriteTime = now;
        Msg("[MaterialPipeline::ToPBR] USDA updated with %d materials.\n",
            (int)m_processedMaterialInfo.size());
    }
}

// Helper to convert absolute path to relative path from mod directory
// Delegate to USDA module for relative path calculation
static std::string GetRelativeTexturePath(const std::string& absolutePath, const std::string& outputDir) {
    return USDA::GetRelativeTexturePath(absolutePath, outputDir);
}

bool TextureProcessor::WriteModUSDA() {
    if (m_outputDirectory.empty() || m_processedMaterialInfo.empty()) {
        return false;
    }
    
    // Get the mod directory using USDA module utility
    std::string modDir = USDA::GetModDirectory(m_outputDirectory);
    
    // Check existing materials and count new ones
    std::unordered_set<uint64_t> existingHashes;
    int newMaterialCount = 0;
    std::string materialsUsdaPath = modDir + "/materials.usda";
    USDA::CheckExistingMaterials(materialsUsdaPath, m_processedMaterialInfo, 
                                  existingHashes, newMaterialCount, m_debugOutput);
    
    // If no new materials to add and file exists, skip writing
    if (newMaterialCount == 0 && !existingHashes.empty()) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] No new materials to add to USDA\n");
        }
        return true;
    }
    
    // Write mod.usda using USDA module (this file rarely changes, no staging needed)
    if (!USDA::WriteModUSDAFile(modDir)) {
        return false;
    }
    
    // Write materials.usda via staging file + atomic rename to avoid
    // triggering Remix reloads with a partially-written file
    std::string stagingPath = modDir + "/materials.usda.tmp";
    if (!USDA::WriteMaterialsUSDAFile(modDir, m_outputDirectory, m_processedMaterialInfo, m_debugOutput, stagingPath)) {
        return false;
    }
    
    // Atomic rename: staging -> final
    if (std::rename(stagingPath.c_str(), materialsUsdaPath.c_str()) != 0) {
        std::remove(materialsUsdaPath.c_str());
        if (std::rename(stagingPath.c_str(), materialsUsdaPath.c_str()) != 0) {
            Warning("[MaterialPipeline::ToPBR] Failed to rename staging file to materials.usda\n");
            return false;
        }
    }
    
    m_lastUSDAWriteTime = std::chrono::steady_clock::now();
    
    Msg("[MaterialPipeline::ToPBR] Wrote mod.usda and materials.usda with %d materials (%d new) to %s\n", 
        (int)m_processedMaterialInfo.size(), newMaterialCount, modDir.c_str());
    
    return true;
}

//=============================================================================
// Background Processing Implementation
//=============================================================================

void TextureProcessor::StartWorkerThread() {
    if (m_workerRunning.load(std::memory_order_relaxed)) {
        return; // Already running
    }
    
    m_shutdownRequested.store(false, std::memory_order_relaxed);
    m_workerRunning.store(true, std::memory_order_relaxed);
    
    m_workerThread = std::thread(&TextureProcessor::WorkerThreadFunc, this);
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Background worker thread started\n");
    }
}

void TextureProcessor::StopWorkerThread() {
    if (!m_workerRunning.load(std::memory_order_relaxed)) {
        return; // Not running
    }
    
    // Signal shutdown
    m_shutdownRequested.store(true, std::memory_order_relaxed);
    
    // Wake up the worker thread
    m_queueCondition.notify_all();
    
    // Wait for thread to finish
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    
    m_workerRunning.store(false, std::memory_order_relaxed);
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Background worker thread stopped\n");
    }
}

void TextureProcessor::WorkerThreadFunc() {
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Worker thread running\n");
    }
    
    // Track whether we have pending USDA data that needs flushing
    bool hasPendingUSDA = false;
    auto lastProcessedTime = std::chrono::steady_clock::now();
    
    while (!m_shutdownRequested.load(std::memory_order_relaxed)) {
        std::string materialName;
        bool gotWork = false;
        
        // Get next material from queue, with a timeout so we can
        // flush USDA after a quiet period even if no new work arrives
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            
            if (m_materialQueue.empty()) {
                if (hasPendingUSDA) {
                    // We have pending USDA data - wait with timeout for the debounce period.
                    // If new materials arrive before the timeout, we'll process them first.
                    // If the timeout expires with no new work, we flush to disk.
                    auto waitResult = m_queueCondition.wait_for(lock, 
                        std::chrono::seconds(USDA_WRITE_DEBOUNCE_SECONDS),
                        [this] {
                            return !m_materialQueue.empty() || m_shutdownRequested.load(std::memory_order_relaxed);
                        });
                    
                    if (m_shutdownRequested.load(std::memory_order_relaxed)) {
                        break;
                    }
                    
                    if (!waitResult) {
                        // Timeout expired with no new work - flush USDA now
                        lock.unlock();
                        Msg("[MaterialPipeline::ToPBR] Debounce period elapsed, flushing USDA to disk\n");
                        AppendMaterialsToUSDA();
                        hasPendingUSDA = false;
                        m_backgroundProcessing.store(false, std::memory_order_relaxed);
                        continue;
                    }
                    // Otherwise new work arrived - fall through to process it
                } else {
                    // No pending USDA data - block indefinitely until work arrives
                    m_queueCondition.wait(lock, [this] {
                        return !m_materialQueue.empty() || m_shutdownRequested.load(std::memory_order_relaxed);
                    });
                    
                    if (m_shutdownRequested.load(std::memory_order_relaxed)) {
                        break;
                    }
                }
            }
            
            if (!m_materialQueue.empty()) {
                materialName = m_materialQueue.front();
                m_materialQueue.pop();
                gotWork = true;
            }
        }
        
        if (!gotWork) {
            continue;
        }
        
        // Skip processing when globally disabled; remove from dedup set
        // (but not from processedMaterials) so it can be re-queued when re-enabled
        if (!m_enabled.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queuedMaterials.erase(materialName);
            continue;
        }
        
        // Process the material (outside of queue lock)
        m_backgroundProcessing.store(true, std::memory_order_relaxed);
        
        bool success = ProcessMaterialOnWorker(materialName);
        
        // Remove from queued set after processing
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queuedMaterials.erase(materialName);
        }
        
        if (success) {
            m_lastProcessedCount.fetch_add(1, std::memory_order_relaxed);
            hasPendingUSDA = true;
            lastProcessedTime = std::chrono::steady_clock::now();
        }
    }
    
    // Final USDA write before exiting
    AppendMaterialsToUSDA();
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Worker thread exiting\n");
    }
}

bool TextureProcessor::ProcessMaterialOnWorker(const std::string& materialName) {
    if (!m_enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    
    // Check if already processed (lock-free check first, then verify under lock)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_processedMaterials.find(materialName) != m_processedMaterials.end()) {
            return false; // Already processed
        }
    }
    
    // Skip engine-generated and 2D interface materials.
    if (MaterialFilter::IsNonSceneMaterialName(materialName)) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        return false;
    }
    
    // Extract PBR properties (this reads from Source Engine - thread safe for reading)
    MaterialPBRProperties props;
    if (!ExtractMaterialPBR(materialName, props)) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        m_ineligibleCache.insert(materialName);
        m_ineligibleCacheDirty = true;
        return false;
    }
    
    // Check if material has PBR-relevant data
    if (!props.hasBumpMap && !props.hasPhong && !props.hasEnvMapMask && 
        !props.normalMapAlphaEnvMapMask && !props.hasPhongExponentTexture &&
        !props.hasBaseMapAlphaPhongMask && !props.hasBaseAlphaEnvMapMask &&
        !props.hasEnvMap && !props.hasEnvMapTint && !props.isGlass) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        m_ineligibleCache.insert(materialName);
        m_ineligibleCacheDirty = true;
        return false;
    }
    
    // Get all frame hashes from the tracker (animated textures have one per frame)
    std::vector<uint64_t> frameHashes = CollectMaterialFrameHashes(materialName);
    
    if (frameHashes.empty()) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        return false;
    }
    
    props.baseTextureHash = frameHashes[0];
    
    // Create PBR material (generates textures - file I/O is thread safe)
    bool success = CreatePBRMaterial(props, frameHashes);
    
    // Mark as processed
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_processedMaterials.insert(materialName);
        m_processedFrameCounts[materialName] = frameHashes.size();
        
        if (success) {
            m_stats.materialsProcessed++;
        }
    }
    
    // Always log when a material is processed (not just in debug mode) so user knows it's working
    if (success) {
        Msg("[MaterialPipeline::ToPBR] Converted: %s\n", materialName.c_str());
    } else if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] [BG] Skipped: %s (hash 0x%llX)\n", 
            materialName.c_str(), frameHashes[0]);
    }
    
    return success;
}

int TextureProcessor::QueueMaterialsForProcessing() {
    if (!m_enabled.load(std::memory_order_relaxed)) {
        return 0;
    }
    
    if (!m_initialized) {
        Msg("[MaterialPipeline::ToPBR] QueueMaterialsForProcessing: not initialized\n");
        return 0;
    }
    
    // Start worker thread if not running
    StartWorkerThread();
    
    // Reset processed count for this batch
    m_lastProcessedCount.store(0, std::memory_order_relaxed);
    
    // Get all cached materials from tracker (this is the only main-thread access)
    std::vector<std::string> cachedMaterials = D3D9TextureTracker::Instance().GetCachedMaterials();
    
    if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] Found %d cached materials in tracker\n", (int)cachedMaterials.size());
    }
    
    int queuedCount = 0;
    
    // Query tracker variant counts before taking m_mutex (avoids nesting the
    // tracker lock inside ours - the tracker's hook path can call back into us)
    std::vector<size_t> variantCounts;
    variantCounts.reserve(cachedMaterials.size());
    for (const std::string& matName : cachedMaterials) {
        variantCounts.push_back(D3D9TextureTracker::Instance().GetTextureVariantCount(matName.c_str()));
    }
    
    // Quick check which materials need processing
    std::vector<std::string> materialsToQueue;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (size_t i = 0; i < cachedMaterials.size(); i++) {
            const std::string& matName = cachedMaterials[i];
            if (MaterialFilter::IsNonSceneMaterialName(matName)) {
                m_processedMaterials.insert(matName);
                continue;
            }

            auto processedIt = m_processedMaterials.find(matName);
            if (processedIt != m_processedMaterials.end()) {
                // Animated textures reveal frames over time. If the tracker now
                // has more variants than when we processed, re-queue so the new
                // frame hashes get registered (cheap sibling-reuse path).
                auto countIt = m_processedFrameCounts.find(matName);
                if (countIt == m_processedFrameCounts.end() || variantCounts[i] <= countIt->second) {
                    continue;
                }
                m_processedMaterials.erase(processedIt);
                if (m_debugOutput) {
                    Msg("[MaterialPipeline::ToPBR] '%s' has new frames (%zu -> %zu) - re-queueing\n",
                        matName.c_str(), countIt->second, variantCounts[i]);
                }
            }
            materialsToQueue.push_back(matName);
        }
    }
    
    // Add to queue (separate lock)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (const std::string& matName : materialsToQueue) {
            // Skip if already in queue
            if (m_queuedMaterials.find(matName) != m_queuedMaterials.end()) {
                continue;
            }
            
            m_materialQueue.push(matName);
            m_queuedMaterials.insert(matName);
            queuedCount++;
        }
    }
    
    // Wake up worker thread
    if (queuedCount > 0) {
        m_queueCondition.notify_one();
        Msg("[MaterialPipeline::ToPBR] Queued %d materials for background processing\n", queuedCount);
    } else if (m_debugOutput) {
        Msg("[MaterialPipeline::ToPBR] No new materials to queue (all %d already processed)\n", 
            (int)cachedMaterials.size());
    }
    
    return queuedCount;
}

size_t TextureProcessor::GetQueuedMaterialCount() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_materialQueue.size();
}

bool TextureProcessor::AppendMaterialsToUSDA() {
    // Collect pending materials
    std::vector<std::pair<uint64_t, ProcessedMaterialInfo>> pendingMaterials;
    size_t totalProcessed = 0;
    size_t alreadyWritten = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        totalProcessed = m_processedMaterialInfo.size();
        alreadyWritten = m_materialsWrittenToUSDA.size();
        
        // Find materials that have been processed but not written to USDA
        for (const auto& pair : m_processedMaterialInfo) {
            if (m_materialsWrittenToUSDA.find(pair.first) == m_materialsWrittenToUSDA.end()) {
                pendingMaterials.push_back(pair);
            }
        }
    }
    
    if (pendingMaterials.empty()) {
        if (m_debugOutput) {
            Msg("[MaterialPipeline::ToPBR] AppendMaterialsToUSDA: no pending materials (total=%d, written=%d)\n",
                (int)totalProcessed, (int)alreadyWritten);
        }
        return true; // Nothing to write
    }
    
    Msg("[MaterialPipeline::ToPBR] AppendMaterialsToUSDA: %d pending materials to write\n", (int)pendingMaterials.size());
    
    // Get mod directory
    std::string modDir = USDA::GetModDirectory(m_outputDirectory);
    std::string materialsUsdaPath = modDir + "/materials.usda";
    std::string stagingPath = modDir + "/materials.usda.tmp";
    
    // Write mod.usda (in case it's missing - this file rarely changes so no staging needed)
    USDA::WriteModUSDAFile(modDir);
    
    // Write to a staging file first, then atomic rename.
    // This prevents Remix's checkForChanges() from seeing a partially-written file
    // and avoids triggering multiple reloads during continuous processing.
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!USDA::WriteMaterialsUSDAFile(modDir, m_outputDirectory, m_processedMaterialInfo, m_debugOutput, stagingPath)) {
            Warning("[MaterialPipeline::ToPBR] Failed to write staging materials.usda.tmp\n");
            return false;
        }
        
        // Atomic rename: staging file -> final file
        // On Windows, MoveFileExA with MOVEFILE_REPLACE_EXISTING is atomic on NTFS
        // On failure, fall back to remove + rename
        if (std::rename(stagingPath.c_str(), materialsUsdaPath.c_str()) != 0) {
            // rename() can fail if destination exists on some platforms - try remove first
            std::remove(materialsUsdaPath.c_str());
            if (std::rename(stagingPath.c_str(), materialsUsdaPath.c_str()) != 0) {
                Warning("[MaterialPipeline::ToPBR] Failed to rename staging file to materials.usda\n");
                return false;
            }
        }
        
        // Mark all as written
        for (const auto& pair : m_processedMaterialInfo) {
            m_materialsWrittenToUSDA.insert(pair.first);
        }
    }
    
    m_lastUSDAWriteTime = std::chrono::steady_clock::now();
    
    Msg("[MaterialPipeline::ToPBR] Updated materials.usda with %d total materials (%d new)\n", 
        (int)m_processedMaterialInfo.size(), (int)pendingMaterials.size());
    
    return true;
}

void TextureProcessor::AppendToUSDAAsync() {
    // If worker thread is running, it will handle USDA writes
    // Otherwise, do it synchronously
    if (m_workerRunning.load(std::memory_order_relaxed)) {
        // Worker will write when queue is empty
        return;
    }
    
    // No worker running - write synchronously
    AppendMaterialsToUSDA();
}

} // namespace ToPBR
} // namespace MaterialPipeline

//=============================================================================
// Lua Bindings - Must be at global scope
//=============================================================================

LUA_FUNCTION(ToPBR_Initialize) {
    // If already initialized (by C++ during RemixAPI init), return success
    if (MaterialPipeline::ToPBR::TextureProcessor::Instance().IsInitialized()) {
        LUA->PushBool(true);
        return 1;
    }
    
    // Not initialized yet - need g_remix to initialize
    if (!g_remix) {
        LUA->PushBool(false);
        return 1;
    }
    
    bool result = MaterialPipeline::ToPBR::TextureProcessor::Instance().Initialize(g_remix);
    LUA->PushBool(result);
    return 1;
}

LUA_FUNCTION(ToPBR_IsInitialized) {
    LUA->PushBool(MaterialPipeline::ToPBR::TextureProcessor::Instance().IsInitialized());
    return 1;
}

LUA_FUNCTION(ToPBR_ProcessAllMaterials) {
    int count = MaterialPipeline::ToPBR::TextureProcessor::Instance().ProcessAllTrackedMaterials();
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(ToPBR_ProcessMaterialsBatch) {
    int maxBatch = 5; // Default batch size
    if (LUA->IsType(1, GarrysMod::Lua::Type::Number)) {
        maxBatch = (int)LUA->GetNumber(1);
    }
    
    int count = MaterialPipeline::ToPBR::TextureProcessor::Instance().ProcessTrackedMaterialsBatch(maxBatch);
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(ToPBR_SetEnabled) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
        LUA->ThrowError("Expected boolean for enabled");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    MaterialPipeline::ToPBR::TextureProcessor::Instance().SetEnabled(enabled);
    Msg("[MaterialPipeline::ToPBR] ToPBR conversion %s\n", enabled ? "ENABLED" : "DISABLED");
    return 0;
}

LUA_FUNCTION(ToPBR_IsEnabled) {
    LUA->PushBool(MaterialPipeline::ToPBR::TextureProcessor::Instance().IsEnabled());
    return 1;
}

LUA_FUNCTION(ToPBR_SetAutoProcessing) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
        LUA->ThrowError("Expected boolean for auto processing");
        return 0;
    }
    
    MaterialPipeline::ToPBR::TextureProcessor::Instance().SetAutoProcessing(LUA->GetBool(1));
    return 0;
}

LUA_FUNCTION(ToPBR_SetDebugOutput) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
        LUA->ThrowError("Expected boolean for debug output");
        return 0;
    }
    
    MaterialPipeline::ToPBR::TextureProcessor::Instance().SetDebugOutput(LUA->GetBool(1));
    return 0;
}

LUA_FUNCTION(ToPBR_RegisterChromeMaterial) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        LUA->ThrowError("Expected hash string for chrome material");
        return 0;
    }

    const char* hashStr = LUA->GetString(1);
    const uint64_t hash = std::strtoull(
        (hashStr[0] == '0' && (hashStr[1] == 'x' || hashStr[1] == 'X')) ? hashStr + 2 : hashStr,
        nullptr, 16);

    LUA->PushBool(
        MaterialPipeline::ToPBR::TextureProcessor::Instance().RegisterChromeMaterial(hash));
    return 1;
}

LUA_FUNCTION(ToPBR_SetMetallicGeneration) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
        LUA->ThrowError("Expected boolean for metallic generation");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    MaterialPipeline::ToPBR::TextureProcessor::Instance().SetMetallicGeneration(enabled);
    
    if (enabled) {
        Msg("[MaterialPipeline::ToPBR] Experimental metallic generation ENABLED\n");
        Msg("[MaterialPipeline::ToPBR] WARNING: This may cause dark envmap materials to appear black.\n");
        Msg("[MaterialPipeline::ToPBR] In PBR, metallic surfaces reflect their base color - black base = no reflections.\n");
    } else {
        Msg("[MaterialPipeline::ToPBR] Metallic generation DISABLED (default)\n");
        Msg("[MaterialPipeline::ToPBR] Dark envmap materials will use low roughness for reflections instead.\n");
    }
    return 0;
}

LUA_FUNCTION(ToPBR_IsMetallicGenerationEnabled) {
    LUA->PushBool(MaterialPipeline::ToPBR::TextureProcessor::Instance().IsMetallicGenerationEnabled());
    return 1;
}

LUA_FUNCTION(ToPBR_SetAutoDiscover) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
        LUA->ThrowError("Expected boolean for auto-discover");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    MaterialPipeline::ToPBR::TextureProcessor::Instance().SetAutoDiscover(enabled);
    
    if (enabled) {
        Msg("[MaterialPipeline::ToPBR] Texture auto-discovery ENABLED (default)\n");
        Msg("[MaterialPipeline::ToPBR] Will search for companion textures like _normal, _mask, _spec\n");
    } else {
        Msg("[MaterialPipeline::ToPBR] Texture auto-discovery DISABLED\n");
        Msg("[MaterialPipeline::ToPBR] Only explicitly referenced textures will be used\n");
    }
    return 0;
}

LUA_FUNCTION(ToPBR_IsAutoDiscoverEnabled) {
    LUA->PushBool(MaterialPipeline::ToPBR::TextureProcessor::Instance().IsAutoDiscoverEnabled());
    return 1;
}

LUA_FUNCTION(ToPBR_SetParseCommentedProperties) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::Bool)) {
        LUA->ThrowError("Expected boolean for parse commented properties");
        return 0;
    }
    
    bool enabled = LUA->GetBool(1);
    MaterialPipeline::ToPBR::TextureProcessor::Instance().SetParseCommentedProperties(enabled);
    
    if (enabled) {
        Msg("[MaterialPipeline::ToPBR] Parsing commented-out VMT properties ENABLED\n");
        Msg("[MaterialPipeline::ToPBR] Will parse // commented properties like $envmap, $normalmapalphaenvmapmask\n");
        Msg("[MaterialPipeline::ToPBR] Useful for maps where these were disabled for vanilla Source performance\n");
    } else {
        Msg("[MaterialPipeline::ToPBR] Parsing commented-out VMT properties DISABLED (default)\n");
        Msg("[MaterialPipeline::ToPBR] Respects author intent - commented properties will be ignored\n");
    }
    return 0;
}

LUA_FUNCTION(ToPBR_IsParseCommentedPropertiesEnabled) {
    LUA->PushBool(MaterialPipeline::ToPBR::TextureProcessor::Instance().IsParseCommentedPropertiesEnabled());
    return 1;
}

LUA_FUNCTION(ToPBR_GetStats) {
    auto stats = MaterialPipeline::ToPBR::TextureProcessor::Instance().GetStats();
    
    LUA->CreateTable();
    
    LUA->PushNumber(stats.materialsProcessed);
    LUA->SetField(-2, "materialsProcessed");
    
    LUA->PushNumber(stats.texturesUploaded);
    LUA->SetField(-2, "texturesUploaded");
    
    LUA->PushNumber(stats.materialsWithNormals);
    LUA->SetField(-2, "materialsWithNormals");
    
    LUA->PushNumber(stats.materialsWithRoughness);
    LUA->SetField(-2, "materialsWithRoughness");
    
    LUA->PushNumber(stats.failedConversions);
    LUA->SetField(-2, "failedConversions");
    
    return 1;
}

LUA_FUNCTION(ToPBR_ClearCache) {
    MaterialPipeline::ToPBR::TextureProcessor::Instance().ClearCache();
    return 0;
}

LUA_FUNCTION(ToPBR_ConvertTexture) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        LUA->ThrowError("Expected string for texture path");
        return 0;
    }
    
    const char* path = LUA->GetString(1);
    bool isNormalMap = LUA->IsType(2, GarrysMod::Lua::Type::Bool) ? LUA->GetBool(2) : false;
    
    uint64_t hash = MaterialPipeline::ToPBR::TextureProcessor::Instance().ConvertAndUploadTexture(path, isNormalMap);
    
    // Return hash as string to preserve precision
    char hashStr[32];
    sprintf_s(hashStr, "0x%llX", hash);
    LUA->PushString(hashStr);
    
    return 1;
}

LUA_FUNCTION(ToPBR_InspectMaterial) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* matName = LUA->GetString(1);
    
    MaterialPipeline::ToPBR::MaterialPBRProperties props;
    if (!MaterialPipeline::ToPBR::TextureProcessor::Instance().ExtractMaterialPBR(matName, props)) {
        LUA->PushNil();
        return 1;
    }
    
    LUA->CreateTable();
    
    // Basic info
    LUA->PushString(props.materialName.c_str());
    LUA->SetField(-2, "name");
    
    LUA->PushString(props.baseTexturePath.c_str());
    LUA->SetField(-2, "baseTexture");
    
    LUA->PushString(props.bumpMapPath.c_str());
    LUA->SetField(-2, "bumpMap");
    
    LUA->PushString(props.envMapMaskPath.c_str());
    LUA->SetField(-2, "envMapMask");
    
    LUA->PushNumber(props.phongExponent);
    LUA->SetField(-2, "phongExponent");
    
    LUA->PushNumber(props.phongBoost);
    LUA->SetField(-2, "phongBoost");
    
    LUA->PushNumber(props.roughness);
    LUA->SetField(-2, "roughness");
    
    LUA->PushNumber(props.metallic);
    LUA->SetField(-2, "metallic");
    
    LUA->PushBool(props.hasBumpMap);
    LUA->SetField(-2, "hasBumpMap");
    
    LUA->PushBool(props.isSSBump);
    LUA->SetField(-2, "isSSBump");
    
    LUA->PushBool(props.hasPhong);
    LUA->SetField(-2, "hasPhong");
    
    LUA->PushBool(props.hasEnvMapMask);
    LUA->SetField(-2, "hasEnvMapMask");
    
    LUA->PushBool(props.hasPhongExponentTexture);
    LUA->SetField(-2, "hasPhongExponentTexture");
    
    LUA->PushString(props.phongExponentTexturePath.c_str());
    LUA->SetField(-2, "phongExponentTexture");
    
    LUA->PushBool(props.hasBaseMapAlphaPhongMask);
    LUA->SetField(-2, "hasBaseMapAlphaPhongMask");
    
    LUA->PushBool(props.normalMapAlphaEnvMapMask);
    LUA->SetField(-2, "normalMapAlphaEnvMapMask");
    
    LUA->PushBool(props.hasBaseAlphaEnvMapMask);
    LUA->SetField(-2, "hasBaseAlphaEnvMapMask");
    
    LUA->PushBool(props.isSelfIllum);
    LUA->SetField(-2, "isSelfIllum");
    
    LUA->PushBool(props.isTranslucent);
    LUA->SetField(-2, "isTranslucent");
    
    LUA->PushBool(props.isGlass);
    LUA->SetField(-2, "isGlass");
    
    LUA->PushString(props.shaderName.c_str());
    LUA->SetField(-2, "shaderName");
    
    LUA->PushString(props.surfaceProp.c_str());
    LUA->SetField(-2, "surfaceProp");
    
    // Metallic detection info
    LUA->PushNumber(props.baseTextureBrightness);
    LUA->SetField(-2, "baseTextureBrightness");
    
    LUA->PushBool(props.hasBaseTextureBrightness);
    LUA->SetField(-2, "hasBaseTextureBrightness");
    
    // =========================================================================
    // PBR Format Detection
    // =========================================================================
    
    // Determine detected format
    std::string detectedFormat = "Source Engine (Standard)";
    if (props.isExoPBR) detectedFormat = "ExoPBR";
    else if (props.isGPBR) detectedFormat = "GPBR (Strata)";
    else if (props.isMWBPBR) detectedFormat = "MWB PBR Gen";
    else if (props.isBFTPseudoPBR) detectedFormat = "PseudoPBR (BlueFlyTrap)";
    
    LUA->PushString(detectedFormat.c_str());
    LUA->SetField(-2, "detectedFormat");
    
    // ExoPBR
    LUA->PushBool(props.isExoPBR);
    LUA->SetField(-2, "isExoPBR");
    
    LUA->PushString(props.armTexturePath.c_str());
    LUA->SetField(-2, "armTexture");
    
    LUA->PushBool(props.hasARMTexture);
    LUA->SetField(-2, "hasARMTexture");
    
    LUA->PushString(props.exoNormalPath.c_str());
    LUA->SetField(-2, "exoNormal");
    
    LUA->PushBool(props.hasExoNormal);
    LUA->SetField(-2, "hasExoNormal");
    
    LUA->PushString(props.emissionTexturePath.c_str());
    LUA->SetField(-2, "emissionTexture");
    
    LUA->PushBool(props.hasEmissionTexture);
    LUA->SetField(-2, "hasEmissionTexture");
    
    // GPBR (Strata)
    LUA->PushBool(props.isGPBR);
    LUA->SetField(-2, "isGPBR");
    
    LUA->PushString(props.mraoTexturePath.c_str());
    LUA->SetField(-2, "mraoTexture");
    
    LUA->PushBool(props.hasMRAOTexture);
    LUA->SetField(-2, "hasMRAOTexture");
    
    LUA->PushString(props.gpbrEmissionPath.c_str());
    LUA->SetField(-2, "gpbrEmission");
    
    LUA->PushBool(props.hasGPBREmission);
    LUA->SetField(-2, "hasGPBREmission");
    
    // BlueFlyTrap PseudoPBR
    LUA->PushBool(props.isBFTPseudoPBR);
    LUA->SetField(-2, "isBFTPseudoPBR");
    
    // MWB PBR Gen
    LUA->PushBool(props.isMWBPBR);
    LUA->SetField(-2, "isMWBPBR");
    
    LUA->PushBool(props.isBFTMetallicLayer);
    LUA->SetField(-2, "isBFTMetallicLayer");
    
    LUA->PushBool(props.isBFTDiffuseLayer);
    LUA->SetField(-2, "isBFTDiffuseLayer");
    
    LUA->PushString(props.bftExponentTexturePath.c_str());
    LUA->SetField(-2, "bftExponentTexture");
    
    LUA->PushBool(props.hasBFTExponentTexture);
    LUA->SetField(-2, "hasBFTExponentTexture");
    
    LUA->PushBool(props.hasBFTBlendTintByBaseAlpha);
    LUA->SetField(-2, "hasBFTBlendTintByBaseAlpha");
    
    LUA->PushBool(props.hasBFTColor2);
    LUA->SetField(-2, "hasBFTColor2");
    
    if (props.hasBFTColor2) {
        LUA->CreateTable();
        LUA->PushNumber(props.bftColor2[0]);
        LUA->SetField(-2, "r");
        LUA->PushNumber(props.bftColor2[1]);
        LUA->SetField(-2, "g");
        LUA->PushNumber(props.bftColor2[2]);
        LUA->SetField(-2, "b");
        LUA->SetField(-2, "bftColor2");
    }
    
    // =========================================================================
    // Auto-discovered textures
    // =========================================================================
    LUA->PushString(props.discoveredNormalPath.c_str());
    LUA->SetField(-2, "discoveredNormal");
    
    LUA->PushBool(props.hasDiscoveredNormal);
    LUA->SetField(-2, "hasDiscoveredNormal");
    
    LUA->PushString(props.discoveredHeightPath.c_str());
    LUA->SetField(-2, "discoveredHeight");
    
    LUA->PushBool(props.hasDiscoveredHeight);
    LUA->SetField(-2, "hasDiscoveredHeight");
    
    LUA->PushString(props.discoveredMaskPath.c_str());
    LUA->SetField(-2, "discoveredMask");
    
    LUA->PushBool(props.hasDiscoveredMask);
    LUA->SetField(-2, "hasDiscoveredMask");
    
    LUA->PushString(props.discoveredAOPath.c_str());
    LUA->SetField(-2, "discoveredAO");
    
    LUA->PushBool(props.hasDiscoveredAO);
    LUA->SetField(-2, "hasDiscoveredAO");
    
    // =========================================================================
    // Envmap properties
    // =========================================================================
    LUA->PushBool(props.hasEnvMap);
    LUA->SetField(-2, "hasEnvMap");
    
    LUA->PushBool(props.hasEnvMapTint);
    LUA->SetField(-2, "hasEnvMapTint");
    
    if (props.hasEnvMapTint) {
        LUA->CreateTable();
        LUA->PushNumber(props.envMapTint[0]);
        LUA->SetField(-2, "r");
        LUA->PushNumber(props.envMapTint[1]);
        LUA->SetField(-2, "g");
        LUA->PushNumber(props.envMapTint[2]);
        LUA->SetField(-2, "b");
        LUA->SetField(-2, "envMapTint");
    }
    
    LUA->PushNumber(props.envMapContrast);
    LUA->SetField(-2, "envMapContrast");
    
    LUA->PushBool(props.hasEnvMapContrast);
    LUA->SetField(-2, "hasEnvMapContrast");
    
    LUA->PushNumber(props.envMapSaturation);
    LUA->SetField(-2, "envMapSaturation");
    
    LUA->PushBool(props.hasEnvMapSaturation);
    LUA->SetField(-2, "hasEnvMapSaturation");
    
    // =========================================================================
    // Phong fresnel
    // =========================================================================
    LUA->PushBool(props.hasPhongFresnelRanges);
    LUA->SetField(-2, "hasPhongFresnelRanges");
    
    if (props.hasPhongFresnelRanges) {
        LUA->CreateTable();
        LUA->PushNumber(1);
        LUA->PushNumber(props.phongFresnelRanges[0]);
        LUA->SetTable(-3);
        LUA->PushNumber(2);
        LUA->PushNumber(props.phongFresnelRanges[1]);
        LUA->SetTable(-3);
        LUA->PushNumber(3);
        LUA->PushNumber(props.phongFresnelRanges[2]);
        LUA->SetTable(-3);
        LUA->SetField(-2, "phongFresnelRanges");
    }
    
    // =========================================================================
    // Rim lighting
    // =========================================================================
    LUA->PushBool(props.hasRimLight);
    LUA->SetField(-2, "hasRimLight");
    
    LUA->PushNumber(props.rimLightExponent);
    LUA->SetField(-2, "rimLightExponent");
    
    LUA->PushNumber(props.rimLightBoost);
    LUA->SetField(-2, "rimLightBoost");
    
    // =========================================================================
    // Self-illum / Emissive
    // =========================================================================
    LUA->PushString(props.selfIllumMaskPath.c_str());
    LUA->SetField(-2, "selfIllumMask");
    
    LUA->PushBool(props.hasSelfIllumMask);
    LUA->SetField(-2, "hasSelfIllumMask");
    
    // =========================================================================
    // Parallax
    // =========================================================================
    LUA->PushString(props.parallaxMapPath.c_str());
    LUA->SetField(-2, "parallaxMap");
    
    LUA->PushBool(props.hasParallaxMap);
    LUA->SetField(-2, "hasParallaxMap");
    
    return 1;
}

LUA_FUNCTION(ToPBR_SetOutputDirectory) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        LUA->ThrowError("Expected string for output directory path");
        return 0;
    }
    
    const char* path = LUA->GetString(1);
    MaterialPipeline::ToPBR::TextureProcessor::Instance().SetOutputDirectory(path);
    return 0;
}

LUA_FUNCTION(ToPBR_GetOutputDirectory) {
    LUA->PushString(MaterialPipeline::ToPBR::TextureProcessor::Instance().GetOutputDirectory().c_str());
    return 1;
}

LUA_FUNCTION(ToPBR_ProcessSingleMaterial) {
    if (!LUA->IsType(1, GarrysMod::Lua::Type::String)) {
        LUA->ThrowError("Expected string for material name");
        return 0;
    }
    
    const char* matName = LUA->GetString(1);
    bool result = MaterialPipeline::ToPBR::TextureProcessor::Instance().ProcessSingleMaterial(matName);
    LUA->PushBool(result);
    return 1;
}

LUA_FUNCTION(ToPBR_WriteUSDAIfNeeded) {
    MaterialPipeline::ToPBR::TextureProcessor::Instance().WriteUSDAIfNeeded();
    return 0;
}

LUA_FUNCTION(ToPBR_NeedsUSDAUpdate) {
    LUA->PushBool(MaterialPipeline::ToPBR::TextureProcessor::Instance().NeedsUSDAUpdate());
    return 1;
}

// =========================================================================
// Background Processing Lua Bindings
// =========================================================================

LUA_FUNCTION(ToPBR_QueueMaterialsForProcessing) {
    int count = MaterialPipeline::ToPBR::TextureProcessor::Instance().QueueMaterialsForProcessing();
    LUA->PushNumber(count);
    return 1;
}

LUA_FUNCTION(ToPBR_IsProcessingInBackground) {
    LUA->PushBool(MaterialPipeline::ToPBR::TextureProcessor::Instance().IsProcessingInBackground());
    return 1;
}

LUA_FUNCTION(ToPBR_GetQueuedMaterialCount) {
    LUA->PushNumber(static_cast<double>(MaterialPipeline::ToPBR::TextureProcessor::Instance().GetQueuedMaterialCount()));
    return 1;
}

LUA_FUNCTION(ToPBR_GetLastProcessedCount) {
    LUA->PushNumber(MaterialPipeline::ToPBR::TextureProcessor::Instance().GetLastProcessedCount());
    return 1;
}

LUA_FUNCTION(ToPBR_AppendToUSDAAsync) {
    MaterialPipeline::ToPBR::TextureProcessor::Instance().AppendToUSDAAsync();
    return 0;
}

namespace MaterialPipeline {
namespace ToPBR {

void InitializeToPBRLuaBindings(GarrysMod::Lua::ILuaBase* LUA) {
    // Register ToPBR functions under MaterialPipeline.ToPBR table
    // First get or create MaterialPipeline table
    LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
    LUA->GetField(-1, "MaterialPipeline");
    if (LUA->IsType(-1, GarrysMod::Lua::Type::Nil)) {
        LUA->Pop(); // pop nil
        LUA->CreateTable();
        LUA->SetField(-2, "MaterialPipeline");
        LUA->GetField(-1, "MaterialPipeline");
    }
    
    // Create ToPBR subtable
    LUA->CreateTable();
    
    LUA->PushCFunction(ToPBR_Initialize);
    LUA->SetField(-2, "Initialize");
    
    LUA->PushCFunction(ToPBR_IsInitialized);
    LUA->SetField(-2, "IsInitialized");
    
    LUA->PushCFunction(ToPBR_ProcessAllMaterials);
    LUA->SetField(-2, "ProcessAllMaterials");
    
    LUA->PushCFunction(ToPBR_ProcessMaterialsBatch);
    LUA->SetField(-2, "ProcessMaterialsBatch");
    
    LUA->PushCFunction(ToPBR_ProcessSingleMaterial);
    LUA->SetField(-2, "ProcessSingleMaterial");
    
    LUA->PushCFunction(ToPBR_SetEnabled);
    LUA->SetField(-2, "SetEnabled");
    
    LUA->PushCFunction(ToPBR_IsEnabled);
    LUA->SetField(-2, "IsEnabled");
    
    LUA->PushCFunction(ToPBR_SetAutoProcessing);
    LUA->SetField(-2, "SetAutoProcessing");
    
    LUA->PushCFunction(ToPBR_SetDebugOutput);
    LUA->SetField(-2, "SetDebugOutput");
    
    LUA->PushCFunction(ToPBR_RegisterChromeMaterial);
    LUA->SetField(-2, "RegisterChromeMaterial");
    
    LUA->PushCFunction(ToPBR_SetMetallicGeneration);
    LUA->SetField(-2, "SetMetallicGeneration");
    
    LUA->PushCFunction(ToPBR_IsMetallicGenerationEnabled);
    LUA->SetField(-2, "IsMetallicGenerationEnabled");
    
    LUA->PushCFunction(ToPBR_SetAutoDiscover);
    LUA->SetField(-2, "SetAutoDiscover");
    
    LUA->PushCFunction(ToPBR_IsAutoDiscoverEnabled);
    LUA->SetField(-2, "IsAutoDiscoverEnabled");
    
    LUA->PushCFunction(ToPBR_SetParseCommentedProperties);
    LUA->SetField(-2, "SetParseCommentedProperties");
    
    LUA->PushCFunction(ToPBR_IsParseCommentedPropertiesEnabled);
    LUA->SetField(-2, "IsParseCommentedPropertiesEnabled");
    
    LUA->PushCFunction(ToPBR_GetStats);
    LUA->SetField(-2, "GetStats");
    
    LUA->PushCFunction(ToPBR_ClearCache);
    LUA->SetField(-2, "ClearCache");
    
    LUA->PushCFunction(ToPBR_ConvertTexture);
    LUA->SetField(-2, "ConvertTexture");
    
    LUA->PushCFunction(ToPBR_InspectMaterial);
    LUA->SetField(-2, "InspectMaterial");
    
    LUA->PushCFunction(ToPBR_SetOutputDirectory);
    LUA->SetField(-2, "SetOutputDirectory");
    
    LUA->PushCFunction(ToPBR_GetOutputDirectory);
    LUA->SetField(-2, "GetOutputDirectory");
    
    LUA->PushCFunction(ToPBR_WriteUSDAIfNeeded);
    LUA->SetField(-2, "WriteUSDAIfNeeded");
    
    LUA->PushCFunction(ToPBR_NeedsUSDAUpdate);
    LUA->SetField(-2, "NeedsUSDAUpdate");
    
    // Background processing
    LUA->PushCFunction(ToPBR_QueueMaterialsForProcessing);
    LUA->SetField(-2, "QueueMaterialsForProcessing");
    
    LUA->PushCFunction(ToPBR_IsProcessingInBackground);
    LUA->SetField(-2, "IsProcessingInBackground");
    
    LUA->PushCFunction(ToPBR_GetQueuedMaterialCount);
    LUA->SetField(-2, "GetQueuedMaterialCount");
    
    LUA->PushCFunction(ToPBR_GetLastProcessedCount);
    LUA->SetField(-2, "GetLastProcessedCount");
    
    LUA->PushCFunction(ToPBR_AppendToUSDAAsync);
    LUA->SetField(-2, "AppendToUSDAAsync");
    
    // Set ToPBR table under MaterialPipeline
    LUA->SetField(-2, "ToPBR");
    LUA->Pop(2); // pop MaterialPipeline and GLOB
    
    Msg("[MaterialPipeline::ToPBR] Lua bindings initialized\n");
}

} // namespace ToPBR
} // namespace MaterialPipeline

#endif // _WIN64
