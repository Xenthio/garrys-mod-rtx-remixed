

// Hook the file opening function instead
//Define_method_Hook(FILE*, fopen, void*, const char* filename, const char* mode)
//{
//    // Check if the file being opened is a .dx90.vtx file
//    const char* vtx_ext = ".dx90.vtx";
//    size_t filename_len = strlen(filename);
//    size_t ext_len = strlen(vtx_ext);
//
//    if (filename_len > ext_len &&
//        _stricmp(filename + filename_len - ext_len, vtx_ext) == 0) {
//
//        // Create a new filename with .sw.vtx instead
//        char* sw_filename = (char*)malloc(filename_len + 1);
//        strcpy(sw_filename, filename);
//        strcpy(sw_filename + filename_len - ext_len, ".sw.vtx");
//
//        // Try to open the .sw.vtx file
//        FILE* file = fopen_trampoline()(sw_filename, mode);
//        free(sw_filename);
//
//        // If successful, return the file; otherwise fall back to original
//        if (file) {
//            Msg("[Model Load Fixes] Redirected %s to .sw.vtx version\n", filename);
//            return file;
//        }
//    }
//
//    // Fall back to original behavior
//    return fopen_trampoline()(_this, filename, mode);
//}


//Define_method_Hook(bool, CMDLCache_LoadHardwareData, void*, MDLHandle_t handle)
//{
//    // Key insight: We only need to modify the part where the VTX file is loaded
//    // Let's identify where in the function it's accessing the VTX file
//
//    // First, let the function run normally until just before it loads the VTX file
//    // We can detour it at that point by setting a breakpoint or using a strategic hook
//
//    // For demonstration purposes, here's a general approach:
//
//    // 1. First, temporarily hook the function that gets the VTX extension
//    // This is likely GetVTXExtension() in the original code
//
//    static bool trying_sw_vtx = false;
//    static void* original_GetVTXExtension = nullptr;
//
//    // Only try .sw.vtx file if we're not already trying it
//    if (!trying_sw_vtx) {
//        // Find the GetVTXExtension function
//        if (!original_GetVTXExtension) {
//            // You'll need to find this function - it might be nearby or referenced
//            // original_GetVTXExtension = FindPattern(module, "GetVTXExtension signature");
//        }
//
//        if (original_GetVTXExtension) {
//            // Hook or patch GetVTXExtension to return ".sw.vtx"
//            // This is pseudo-code - you'll need your actual hooking implementation
//            // HookFunction(original_GetVTXExtension, MyGetVTXExtension);
//
//            trying_sw_vtx = true;
//
//            // Call the original function with our hook in place
//            bool result = CMDLCache_LoadHardwareData_trampoline()(_this, handle);
//
//            // Restore the original GetVTXExtension function
//            // UnhookFunction(original_GetVTXExtension);
//
//            trying_sw_vtx = false;
//
//            // If loading with .sw.vtx succeeded, return the result
//            if (result) {
//                Msg("[Model Load Fixes] Successfully loaded .sw.vtx for handle %d\n", handle);
//                return result;
//            }
//
//            // If loading with .sw.vtx failed, we'll fall through to try .dx90.vtx
//            Msg("[Model Load Fixes] Failed to load .sw.vtx, falling back to .dx90.vtx\n");
//        }
//    }
//
//    // If we didn't succeed with .sw.vtx or couldn't hook GetVTXExtension,
//    // fall back to original behavior (which will try .dx90.vtx)
//    return CMDLCache_LoadHardwareData_trampoline()(_this, handle);
//}

// Mock implementation of our custom GetVTXExtension
//const char* MyGetVTXExtension()
//{
//    return ".sw.vtx";
//}
#include "GarrysMod/Lua/Interface.h"
#include "modelload_fixes.h"
#include "globalconvars.h"
#include "cdll_client_int.h"
#include "filesystem.h"  // Include for IFileSystem definitions
#ifdef QUEUEDLOADER_INTERFACE_VERSION
#undef QUEUEDLOADER_INTERFACE_VERSION
#endif
#include "filesystem/IQueuedLoader.h"
#include "datacache/imdlcache.h"
#include <stdint.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <thread>
#include <mutex>
#include <cctype>

using namespace GarrysMod::Lua;

// Global filesystem interface pointer
IFileSystem* g_pFileSystem = nullptr;

// VTX file structure for checksum
#pragma pack(push, 1)
struct OptimizedModelFileHeader_t
{
    int version;
    int vertCacheSize;
    short maxBonesPerStrip;
    short maxBonesPerTri;
    int maxBonesPerVert;
    int checkSum;
    int numLODs;
    int numBodyParts;
    int bodyPartOffset;
};
#pragma pack(pop)

struct ResolvedFileLocation
{
    std::string filename;
    const char* pathID = nullptr;
};

typedef FileHandle_t(*OpenExFunc)(void*, const char*, const char*, unsigned, const char*, char**);

// Forward declare helper functions, will implement after the hook
int GetVtxFileChecksum(void* fs, const char* filename, const char* pathID, void* openFunc);
int GetMdlFileChecksum(void* fs, const char* filename, const char* pathID, void* openFunc);
int GetMdlBoneCount(void* fs, const char* vtxFilename, const char* pathID, void* openFunc);
bool ResolveMatchingSoftwareVtxPath(void* fs, const char* hardwareFilename,
    const char* pathID, void* openFunc, struct ResolvedFileLocation& softwareFile);

namespace {
    constexpr const char* kSoftwareVtxExtension = ".sw.vtx";

    std::vector<std::string> g_gameSearchPaths;
    std::string g_hl2MiscCompatibilityRoot;
    std::string g_lamarrCompatibilityRoot;
    std::string g_handsCompatibilityRoot;

    bool ResolveCompatibilityFileLocation(
        const char* filename, ResolvedFileLocation& compatibilityFile);

    bool CanOpenFileLocation(void* fs, const ResolvedFileLocation& file,
        void* openFunc)
    {
        if (!fs || file.filename.empty() || !openFunc) {
            return false;
        }

        FileHandle_t handle = reinterpret_cast<OpenExFunc>(openFunc)(
            fs, file.filename.c_str(), "rb", 0, file.pathID, nullptr);
        if (!handle) {
            return false;
        }
        reinterpret_cast<IFileSystem*>(fs)->Close(handle);
        return true;
    }

#ifdef _WIN64
    using IFileSystemGetSearchPathFn = int(__fastcall*)(
        void*, const char*, bool, char*, int);
    IFileSystemGetSearchPathFn g_pFileSystemGetSearchPath = nullptr;
#endif

    bool ShouldKeepLegacyHardwareVtx()
    {
        return GlobalConvars::r_forcehwskin && GlobalConvars::r_forcehwskin->GetBool() &&
            GlobalConvars::r_hwskin_vtx_hw && GlobalConvars::r_hwskin_vtx_hw->GetBool();
    }

    bool HasHardwareVtxExtension(const char* filename)
    {
        if (!filename) {
            return false;
        }

        static const char* hardwareExtensions[] = {
            ".dx90.vtx", ".dx80.vtx", ".dx70.vtx"
        };
        const size_t filenameLength = strlen(filename);
        for (const char* extension : hardwareExtensions) {
            const size_t extensionLength = strlen(extension);
            if (filenameLength > extensionLength &&
                _stricmp(filename + filenameLength - extensionLength,
                    extension) == 0) {
                return true;
            }
        }
        return false;
    }

    bool ResolvePreferredCompatibilityFileLocation(
        void* fs, const char* filename, const char* pathID, void* openFunc,
        ResolvedFileLocation& compatibilityFile, bool& usesSoftwareVtx)
    {
        usesSoftwareVtx = false;
        if (!ResolveCompatibilityFileLocation(filename, compatibilityFile)) {
            return false;
        }
        if (!CanOpenFileLocation(fs, compatibilityFile, openFunc)) {
            compatibilityFile = {};
            return false;
        }

        // HL2 RTX captures its skinned replacement hashes and joint indices
        // from SW-VTX data. Resolve the software companion from the same
        // launcher-managed mount path so search priority cannot mix files from
        // different model builds. Keep DX90 only when r_hwskin_vtx_hw
        // explicitly requests legacy palettes.
        if (HasHardwareVtxExtension(filename) &&
            !ShouldKeepLegacyHardwareVtx()) {
            ResolvedFileLocation softwareFile;
            if (ResolveMatchingSoftwareVtxPath(
                    fs, compatibilityFile.filename.c_str(),
                    compatibilityFile.pathID, openFunc, softwareFile)) {
                compatibilityFile = std::move(softwareFile);
                usesSoftwareVtx = true;
            }
        }
        return true;
    }
}

// Modified hook with recursion prevention and checksum verification
Define_method_Hook(FileHandle_t, IFileSystem_OpenEx, void*, const char* pFileName,
    const char* pOptions, unsigned flags, const char* pathID, char** ppszResolvedFilename)
{
    // Prevent recursion
    static bool inHook = false;
    if (inHook || !pFileName || !pOptions) {
        return IFileSystem_OpenEx_trampoline()(_this, pFileName, pOptions, flags, pathID, ppszResolvedFilename);
    }

    inHook = true;
    FileHandle_t result = NULL;

    try {
        // Define the VTX extensions we want to check for
        const char* vtx_exts[] = { ".dx90.vtx", ".dx80.vtx", ".dx70.vtx" };
        const int ext_count = sizeof(vtx_exts) / sizeof(vtx_exts[0]);

        // Check if it's a model file (handle both forward and backslashes)
        const char* model_indicators[] = { "models/", "models\\" };
        bool is_model = false;
        for (int i = 0; i < 2; i++) {
            if (strstr(pFileName, model_indicators[i])) {
                is_model = true;
                break;
            }
        }

        // RTXLauncher exposes the authoritative HL2/HL2 RTX model sets through
        // named mount.cfg paths. Prefer the selected source before ordinary
        // GAME search paths while honoring the SW/global versus DX90/local
        // bone-index mode for optimized vertex data.
        ResolvedFileLocation compatibilityFile;
        bool compatibilityUsesSoftwareVtx = false;
        if (is_model &&
            ResolvePreferredCompatibilityFileLocation(
                _this, pFileName, pathID,
                (void*)IFileSystem_OpenEx_trampoline(), compatibilityFile,
                compatibilityUsesSoftwareVtx)) {
            result = IFileSystem_OpenEx_trampoline()(
                _this, compatibilityFile.filename.c_str(), pOptions, flags,
                compatibilityFile.pathID, ppszResolvedFilename);
            if (result) {
                static int compatibilityRedirectLogCount = 0;
                if (GlobalConvars::r_hwskin_debug &&
                    GlobalConvars::r_hwskin_debug->GetBool() &&
                    compatibilityRedirectLogCount < 50) {
                    Msg("[gmRTX - Model Load] USING mounted compatibility %s model file: %s -> %s (%s)\n",
                        compatibilityUsesSoftwareVtx ? "SW-VTX" : "exact",
                        pFileName, compatibilityFile.filename.c_str(),
                        compatibilityFile.pathID
                            ? compatibilityFile.pathID
                            : "<default>");
                    compatibilityRedirectLogCount++;
                }
                inHook = false;
                return result;
            }
        }

        if (is_model) {
            size_t filename_len = strlen(pFileName);

            // Check for each possible extension
            for (int i = 0; i < ext_count; i++) {
                const char* vtx_ext = vtx_exts[i];
                size_t ext_len = strlen(vtx_ext);

                // If filename ends with this extension (case insensitive)
                if (filename_len > ext_len &&
                    _stricmp(pFileName + filename_len - ext_len, vtx_ext) == 0) {

                    // Check if this is a skinned model (has >1 bone)
                    // Skinned models need HW vertex data for proper bone animation in RTX Remix
                    int boneCount = GetMdlBoneCount(_this, pFileName, pathID, (void*)IFileSystem_OpenEx_trampoline());
                    
                    static int vtxLogCount = 0;
                    if (GlobalConvars::r_hwskin_debug &&
                        GlobalConvars::r_hwskin_debug->GetBool() &&
                        vtxLogCount < 50) {
                        Msg("[gmRTX - VTX Load] %s -> boneCount=%d\n", pFileName, boneCount);
                        vtxLogCount++;
                    }
                    
                    if (boneCount > 1 && ShouldKeepLegacyHardwareVtx()) {
                        // Compatibility escape hatch: retain Source's DX9-optimized
                        // topology. This does not match the SW VTX ordering used by
                        // Half-Life 2 RTX replacement captures.
                        if (GlobalConvars::r_hwskin_debug &&
                            GlobalConvars::r_hwskin_debug->GetBool()) {
                            Msg("[gmRTX - Model Load] KEEPING legacy HW VTX for skinned model (%d bones): %s\n", boneCount, pFileName);
                        }
                        break; // Skip SW redirection, use original HW file
                    }

                    ResolvedFileLocation softwareFile;
                    if (ResolveMatchingSoftwareVtxPath(
                            _this, pFileName, pathID,
                            (void*)IFileSystem_OpenEx_trampoline(),
                            softwareFile)) {
                        result = IFileSystem_OpenEx_trampoline()(
                            _this, softwareFile.filename.c_str(), pOptions,
                            flags, softwareFile.pathID,
                            ppszResolvedFilename);
                        if (result) {
                            static int swRedirectLogCount = 0;
                            if (GlobalConvars::r_hwskin_debug &&
                                GlobalConvars::r_hwskin_debug->GetBool() &&
                                swRedirectLogCount < 50) {
                                Msg("[gmRTX - Model Load] USING HL2 RTX-compatible SW VTX: %s -> %s\n",
                                    pFileName, softwareFile.filename.c_str());
                                swRedirectLogCount++;
                            }
                        }
                    }

                    // If we successfully opened the SW VTX file, return it
                    if (result) {
                        inHook = false;
                        return result;
                    }

                    // If we didn't find a matching SW VTX, break and fall back to original
                    break;
                }
            }
        }

        // Fall back to original behavior
        result = IFileSystem_OpenEx_trampoline()(_this, pFileName, pOptions, flags, pathID, ppszResolvedFilename);
    }
    catch (...) {
        // If an unexpected exception occurs, ensure we fall back to original behavior
        if (!result) {
            result = IFileSystem_OpenEx_trampoline()(_this, pFileName, pOptions, flags, pathID, ppszResolvedFilename);
        }
    }

    // Always reset the recursion flag before returning
    inHook = false;
    return result;
}

// Helper function to read checksum from a VTX file - now using trampoline directly
int GetVtxFileChecksum(void* fs, const char* filename, const char* pathID, void* openFunc)
{
    OpenExFunc openExFn = (OpenExFunc)openFunc;

    ResolvedFileLocation compatibilityFile;
    const bool useCompatibilityFile = ResolveCompatibilityFileLocation(
        filename, compatibilityFile);
    FileHandle_t file = NULL;
    if (useCompatibilityFile) {
        file = openExFn(
            fs, compatibilityFile.filename.c_str(), "rb", 0,
            compatibilityFile.pathID, NULL);
    }
    if (!file) {
        file = openExFn(fs, filename, "rb", 0, pathID, NULL);
    }
    if (!file)
        return 0;

    OptimizedModelFileHeader_t header;
    size_t bytesRead = ((IFileSystem*)fs)->Read(&header, sizeof(header), file);
    ((IFileSystem*)fs)->Close(file);

    if (bytesRead != sizeof(header))
        return 0;

    return header.checkSum;
}

bool ResolveMatchingSoftwareVtxPath(void* fs, const char* hardwareFilename,
    const char* pathID, void* openFunc, ResolvedFileLocation& softwareFile)
{
    if (!fs || !hardwareFilename || !openFunc) {
        return false;
    }

    const char* hardwareExtensions[] = {
        ".dx90.vtx", ".dx80.vtx", ".dx70.vtx"
    };
    const size_t filenameLength = strlen(hardwareFilename);
    size_t matchedExtensionLength = 0;
    for (const char* extension : hardwareExtensions) {
        const size_t extensionLength = strlen(extension);
        if (filenameLength > extensionLength &&
            _stricmp(hardwareFilename + filenameLength - extensionLength,
                extension) == 0) {
            matchedExtensionLength = extensionLength;
            break;
        }
    }
    if (matchedExtensionLength == 0) {
        return false;
    }

    const int hardwareChecksum = GetVtxFileChecksum(
        fs, hardwareFilename, pathID, openFunc);
    if (hardwareChecksum == 0) {
        return false;
    }

    std::string relativeSoftwarePath(hardwareFilename);
    relativeSoftwarePath.replace(
        filenameLength - matchedExtensionLength,
        matchedExtensionLength,
        kSoftwareVtxExtension);

    // Prefer the normal search-path result when it belongs to the same model
    // build. This remains the fast path for nearly all Source models.
    if (GetVtxFileChecksum(fs, relativeSoftwarePath.c_str(), pathID,
            openFunc) == hardwareChecksum) {
        softwareFile.filename = relativeSoftwarePath;
        softwareFile.pathID = pathID;
        return true;
    }

    // GMod can override an MDL and its DX90 VTX without shipping a matching SW
    // VTX. A lower-priority mounted game can still contain the compatible SW
    // file. Probe each encoded absolute search path so lookup does not stop at
    // the first, incompatible file from another Source game build.
    for (const std::string& searchPath : g_gameSearchPaths) {
        if (searchPath.empty()) {
            continue;
        }

        std::string candidate(searchPath);
        const char lastCharacter = candidate[candidate.size() - 1];
        if (lastCharacter != '\\' && lastCharacter != '/') {
            candidate.push_back('\\');
        }
        candidate.append(relativeSoftwarePath);

        if (GetVtxFileChecksum(fs, candidate.c_str(), nullptr,
                openFunc) == hardwareChecksum) {
            softwareFile.filename = candidate;
            softwareFile.pathID = nullptr;
            if (GlobalConvars::r_hwskin_debug &&
                GlobalConvars::r_hwskin_debug->GetBool()) {
                Msg("[gmRTX - Model Load] Found checksum-matched SW VTX in alternate mount: %s\n",
                    candidate.c_str());
            }
            return true;
        }
    }

    return false;
}

#ifdef _WIN64
namespace {
    std::string TrimmedSearchRoot(const std::string& path)
    {
        std::string result(path);
        while (!result.empty() &&
            (result.back() == '\\' || result.back() == '/')) {
            result.pop_back();
        }
        return result;
    }

    std::string NormalizedSearchRoot(const std::string& path)
    {
        std::string result = TrimmedSearchRoot(path);
        for (char& character : result) {
            if (character == '/') {
                character = '\\';
            }
            else {
                character = static_cast<char>(tolower(
                    static_cast<unsigned char>(character)));
            }
        }
        return result;
    }

    bool EndsWith(const std::string& value, const char* suffix)
    {
        const size_t suffixLength = strlen(suffix);
        return value.size() >= suffixLength &&
            value.compare(value.size() - suffixLength,
                suffixLength, suffix) == 0;
    }

    void RefreshCompatibilitySearchRoots()
    {
        g_hl2MiscCompatibilityRoot.clear();
        g_lamarrCompatibilityRoot.clear();
        g_handsCompatibilityRoot.clear();

        constexpr const char* lamarrSuffix =
            "\\hl2rtx\\custom\\lamarr_hack";
        constexpr const char* handsSuffix =
            "\\hl2rtx\\custom\\new_rtx_hands";
        std::string installRoot;

        for (const std::string& searchPath : g_gameSearchPaths) {
            const std::string normalized = NormalizedSearchRoot(searchPath);
            if (EndsWith(normalized, lamarrSuffix)) {
                g_lamarrCompatibilityRoot = TrimmedSearchRoot(searchPath);
                if (installRoot.empty()) {
                    installRoot = normalized.substr(
                        0, normalized.size() - strlen(lamarrSuffix));
                }
            }
            else if (EndsWith(normalized, handsSuffix)) {
                g_handsCompatibilityRoot = TrimmedSearchRoot(searchPath);
                installRoot = normalized.substr(
                    0, normalized.size() - strlen(handsSuffix));
            }
        }

        if (!installRoot.empty()) {
            for (const std::string& searchPath : g_gameSearchPaths) {
                const std::string normalized =
                    NormalizedSearchRoot(searchPath);
                if (normalized.compare(0, installRoot.size(),
                        installRoot) != 0) {
                    continue;
                }
                if (EndsWith(normalized, "\\hl2\\hl2_misc.vpk") ||
                    EndsWith(normalized,
                        "\\hl2\\hl2_misc_dir.vpk")) {
                    g_hl2MiscCompatibilityRoot =
                        TrimmedSearchRoot(searchPath);
                    break;
                }
            }
        }
    }

    bool RefreshGameSearchPaths()
    {
        g_gameSearchPaths.clear();
        if (!g_pFileSystem || !g_pFileSystemGetSearchPath) {
            return false;
        }

        const int requiredLength = g_pFileSystemGetSearchPath(
            g_pFileSystem, "GAME", true, nullptr, 0);
        if (requiredLength <= 1 || requiredLength > 1024 * 1024) {
            return false;
        }

        std::vector<char> searchPathBuffer(requiredLength, '\0');
        const int returnedLength = g_pFileSystemGetSearchPath(
            g_pFileSystem, "GAME", true, searchPathBuffer.data(),
            static_cast<int>(searchPathBuffer.size()));
        if (returnedLength <= 1 || searchPathBuffer[0] == '\0') {
            return false;
        }

        const char* pathStart = searchPathBuffer.data();
        for (const char* cursor = pathStart; ; cursor++) {
            if (*cursor != ';' && *cursor != '\0') {
                continue;
            }
            if (cursor > pathStart) {
                g_gameSearchPaths.emplace_back(pathStart, cursor - pathStart);
            }
            if (*cursor == '\0') {
                break;
            }
            pathStart = cursor + 1;
        }

        RefreshCompatibilitySearchRoots();

        if (GlobalConvars::r_hwskin_debug &&
            GlobalConvars::r_hwskin_debug->GetBool()) {
            Msg("[gmRTX - Model Load Fixes] Enumerated %zu GAME search paths for checksum-matched SW VTX lookup\n",
                g_gameSearchPaths.size());
        }
        return !g_gameSearchPaths.empty();
    }

    bool MatchesModelStem(const std::string& relativePath,
        const char* stem, bool includeRelatedModels)
    {
        const size_t stemLength = strlen(stem);
        if (relativePath.size() <= stemLength ||
            relativePath.compare(0, stemLength, stem) != 0) {
            return false;
        }

        const char suffixCharacter = relativePath[stemLength];
        return suffixCharacter == '.' ||
            (includeRelatedModels && suffixCharacter == '_');
    }

    bool NormalizeModelRelativePath(
        const char* filename, std::string& relativePath)
    {
        relativePath.clear();
        if (!filename || !*filename) {
            return false;
        }

        relativePath.assign(filename);
        for (char& character : relativePath) {
            if (character == '/') {
                character = '\\';
            }
            else {
                character = static_cast<char>(tolower(
                    static_cast<unsigned char>(character)));
            }
        }

        // Queued model loading can reopen a resolved absolute path. Reduce it
        // back to its virtual models/... path before selecting its source.
        const size_t absoluteModels = relativePath.find("\\models\\");
        if (absoluteModels != std::string::npos) {
            relativePath.erase(0, absoluteModels + 1);
        }

        while (relativePath.size() >= 2 && relativePath[0] == '.' &&
            relativePath[1] == '\\') {
            relativePath.erase(0, 2);
        }
        while (!relativePath.empty() && relativePath[0] == '\\') {
            relativePath.erase(0, 1);
        }

        return relativePath.compare(0, 7, "models\\") == 0 &&
            relativePath.find("..") == std::string::npos;
    }

    const std::string* CompatibilitySearchRoot(
        const std::string& relativePath)
    {
        // HL2 RTX ships a corrected classic-headcrab model in lamarr_hack.
        // Check it before the more general headcrab stem.
        if (MatchesModelStem(
                relativePath, "models\\headcrabclassic", true)) {
            return &g_lamarrCompatibilityRoot;
        }

        static const char* handsViewModels[] = {
            "models\\weapons\\v_357",
            "models\\weapons\\v_bugbait",
            "models\\weapons\\v_crossbow",
            "models\\weapons\\v_crowbar",
            "models\\weapons\\v_grenade",
            "models\\weapons\\v_hands",
            "models\\weapons\\v_irifle",
            "models\\weapons\\v_physcannon",
            "models\\weapons\\v_pistol",
            "models\\weapons\\v_rpg",
            "models\\weapons\\v_shotgun",
            "models\\weapons\\v_smg1",
            "models\\weapons\\v_superphyscannon",
        };
        for (const char* stem : handsViewModels) {
            if (MatchesModelStem(relativePath, stem, true)) {
                return &g_handsCompatibilityRoot;
            }
        }

        if (MatchesModelStem(
                relativePath, "models\\weapons\\v_stunbaton", true)) {
            return &g_hl2MiscCompatibilityRoot;
        }

        static const struct CompatibilityPrefix {
            const char* value;
            bool includeRelatedModels;
        } hl2ModelPrefixes[] = {
            { "models\\zombie\\classic", true },
            // fast_torso is Episode-only and has no HL2 RTX replacement.
            { "models\\zombie\\fast", false },
            { "models\\zombie\\poison", true },
            { "models\\headcrabblack", true },
            { "models\\headcrab", true },
        };
        for (const CompatibilityPrefix& prefix : hl2ModelPrefixes) {
            if (MatchesModelStem(relativePath, prefix.value,
                    prefix.includeRelatedModels)) {
                return &g_hl2MiscCompatibilityRoot;
            }
        }

        return nullptr;
    }

    bool ResolveCompatibilityFileLocation(
        const char* filename, ResolvedFileLocation& compatibilityFile)
    {
        compatibilityFile = {};
        std::string relativePath;
        if (!NormalizeModelRelativePath(filename, relativePath)) {
            return false;
        }

        const std::string* searchRoot =
            CompatibilitySearchRoot(relativePath);
        if (!searchRoot || searchRoot->empty()) {
            return false;
        }

        compatibilityFile.filename = *searchRoot;
        if (compatibilityFile.filename.back() != '\\' &&
            compatibilityFile.filename.back() != '/') {
            compatibilityFile.filename.push_back('\\');
        }
        compatibilityFile.filename.append(relativePath);
        compatibilityFile.pathID = nullptr;
        return true;
    }
}
#else
namespace {
    bool ResolveCompatibilityFileLocation(
        const char*, ResolvedFileLocation& compatibilityFile)
    {
        // The launcher-managed compatibility roots are discovered through the
        // x64 filesystem search-path implementation above. Keep the Win32
        // build on its normal GAME lookup instead of leaving an unresolved
        // helper reference in the shared model-loading code.
        compatibilityFile = {};
        return false;
    }
}
#endif

// Helper to get MDL checksum - using trampoline
int GetMdlFileChecksum(void* fs, const char* filename, const char* pathID, void* openFunc)
{
    // Extract base MDL path
    std::string mdlPath = filename;

    // Replace VTX extension with MDL extension
    const char* extensions[] = { ".dx90.vtx", ".dx80.vtx", ".dx70.vtx", ".sw.vtx" };
    for (const char* ext : extensions) {
        size_t pos = mdlPath.rfind(ext);
        if (pos != std::string::npos) {
            mdlPath.replace(pos, strlen(ext), ".mdl");
            break;
        }
    }

    OpenExFunc openExFn = (OpenExFunc)openFunc;

    ResolvedFileLocation compatibilityFile;
    const bool useCompatibilityFile = ResolveCompatibilityFileLocation(
        mdlPath.c_str(), compatibilityFile);
    FileHandle_t file = NULL;
    if (useCompatibilityFile) {
        file = openExFn(
            fs, compatibilityFile.filename.c_str(), "rb", 0,
            compatibilityFile.pathID, NULL);
    }
    if (!file) {
        file = openExFn(fs, mdlPath.c_str(), "rb", 0, pathID, NULL);
    }
    if (!file)
        return 0;

    // studiohdr_t stores the model checksum immediately after id and version.
    ((IFileSystem*)fs)->Seek(file, 0x08, FILESYSTEM_SEEK_HEAD);

    int checksum = 0;
    ((IFileSystem*)fs)->Read(&checksum, sizeof(checksum), file);
    ((IFileSystem*)fs)->Close(file);

    return checksum;
}

// Helper to get bone count from MDL file - used to determine if model is skinned
// Returns -1 on error, otherwise the number of bones
int GetMdlBoneCount(void* fs, const char* vtxFilename, const char* pathID, void* openFunc)
{
    // Extract base MDL path from VTX filename
    std::string mdlPath = vtxFilename;

    // Replace VTX extension with MDL extension
    const char* extensions[] = { ".dx90.vtx", ".dx80.vtx", ".dx70.vtx", ".sw.vtx" };
    bool foundExt = false;
    for (const char* ext : extensions) {
        size_t pos = mdlPath.rfind(ext);
        if (pos != std::string::npos) {
            mdlPath.replace(pos, strlen(ext), ".mdl");
            foundExt = true;
            break;
        }
    }
    
    if (!foundExt) {
        // Couldn't find VTX extension to replace
        return -1;
    }

    OpenExFunc openExFn = (OpenExFunc)openFunc;

    ResolvedFileLocation compatibilityFile;
    const bool useCompatibilityFile = ResolveCompatibilityFileLocation(
        mdlPath.c_str(), compatibilityFile);
    FileHandle_t file = NULL;
    if (useCompatibilityFile) {
        file = openExFn(
            fs, compatibilityFile.filename.c_str(), "rb", 0,
            compatibilityFile.pathID, NULL);
    }
    if (!file) {
        file = openExFn(fs, mdlPath.c_str(), "rb", 0, pathID, NULL);
    }
    if (!file) {
        static int failLogCount = 0;
        if (GlobalConvars::r_hwskin_debug &&
            GlobalConvars::r_hwskin_debug->GetBool() &&
            failLogCount < 10) {
            Msg("[gmRTX - VTX Load] Failed to open MDL: %s\n", mdlPath.c_str());
            failLogCount++;
        }
        return -1;
    }

    // studiohdr_t::numbones is at offset 0x9C (156) in the MDL file
    // This is consistent across Source engine versions
    const int NUMBONES_OFFSET = 0x9C;
    
    ((IFileSystem*)fs)->Seek(file, NUMBONES_OFFSET, FILESYSTEM_SEEK_HEAD);

    int numbones = 0;
    size_t bytesRead = ((IFileSystem*)fs)->Read(&numbones, sizeof(numbones), file);
    ((IFileSystem*)fs)->Close(file);

    if (bytesRead != sizeof(numbones))
        return -1;

    // Sanity check - bone count should be reasonable
    if (numbones < 0 || numbones > 1024)
        return -1;

    return numbones;
}

// CMDLCache loads VTX data through the filesystem async API, not OpenEx.
// Redirect the request before the filesystem job duplicates pszFilename so the
// asynchronous job owns the replacement path for its entire lifetime.
// Hook the CreditAlloc implementation because some GMod modules call it
// directly while the public AsyncReadMultiple wrapper also forwards into it.
Define_method_Hook(FSAsyncStatus_t, IFileSystem_AsyncReadMultipleCreditAlloc, void*,
    const FileAsyncRequest_t* pRequests, int nRequests,
    const char* pszFile, int line, FSAsyncControl_t* phControls)
{
    if (!pRequests || nRequests <= 0 || nRequests > 1024) {
        return IFileSystem_AsyncReadMultipleCreditAlloc_trampoline()(
            _this, pRequests, nRequests, pszFile, line, phControls);
    }

    try {
        std::vector<FileAsyncRequest_t> redirectedRequests(pRequests, pRequests + nRequests);
        std::vector<std::string> redirectedPaths(nRequests);
        std::vector<const char*> redirectedPathIds(nRequests, nullptr);
        bool redirectedAny = false;

        for (int i = 0; i < nRequests; i++) {
            const FileAsyncRequest_t& request = pRequests[i];
            if (!request.pszFilename ||
                request.hSpecificAsyncFile != FS_INVALID_ASYNC_FILE) {
                continue;
            }

            const char* hardwareExtensions[] = { ".dx90.vtx", ".dx80.vtx", ".dx70.vtx" };
            const size_t filenameLength = strlen(request.pszFilename);
            const char* matchedExtension = nullptr;

            for (const char* extension : hardwareExtensions) {
                const size_t extensionLength = strlen(extension);
                if (filenameLength > extensionLength &&
                    _stricmp(request.pszFilename + filenameLength - extensionLength, extension) == 0) {
                    matchedExtension = extension;
                    break;
                }
            }

            bool compatibilityUsesSoftwareVtx = false;
            ResolvedFileLocation replacementFile;
            const bool redirectedToCompatibilityFile =
                ResolvePreferredCompatibilityFileLocation(
                    _this, request.pszFilename, request.pszPathID,
                    (void*)IFileSystem_OpenEx_trampoline(),
                    replacementFile, compatibilityUsesSoftwareVtx);
            bool redirectedToSoftwareVtx = false;
            if (!redirectedToCompatibilityFile && matchedExtension &&
                !ShouldKeepLegacyHardwareVtx()) {
                redirectedToSoftwareVtx = ResolveMatchingSoftwareVtxPath(
                    _this, request.pszFilename, request.pszPathID,
                    (void*)IFileSystem_OpenEx_trampoline(),
                    replacementFile);
            }
            if (!redirectedToCompatibilityFile &&
                !redirectedToSoftwareVtx) {
                continue;
            }

            redirectedPaths[i] = std::move(replacementFile.filename);
            redirectedPathIds[i] = replacementFile.pathID;
            redirectedRequests[i].pszFilename = redirectedPaths[i].c_str();
            redirectedRequests[i].pszPathID = redirectedPathIds[i];
            redirectedAny = true;

            static int compatibilityAsyncRedirectLogCount = 0;
            if (GlobalConvars::r_hwskin_debug &&
                GlobalConvars::r_hwskin_debug->GetBool() &&
                compatibilityAsyncRedirectLogCount < 50) {
                Msg("[gmRTX - Model Load] ASYNC USING %s model file: %s -> %s (%s)\n",
                    redirectedToCompatibilityFile
                        ? (compatibilityUsesSoftwareVtx
                            ? "compatibility SW-VTX"
                            : "exact compatibility")
                        : "SW-VTX",
                    request.pszFilename, redirectedPaths[i].c_str(),
                    redirectedPathIds[i]
                        ? redirectedPathIds[i]
                        : "<default>");
                compatibilityAsyncRedirectLogCount++;
            }
        }

        return IFileSystem_AsyncReadMultipleCreditAlloc_trampoline()(
            _this,
            redirectedAny ? redirectedRequests.data() : pRequests,
            nRequests,
            pszFile,
            line,
            phControls);
    }
    catch (...) {
        Warning("[gmRTX - Model Load Fixes] Exception while redirecting async VTX request\n");
        return IFileSystem_AsyncReadMultipleCreditAlloc_trampoline()(
            _this, pRequests, nRequests, pszFile, line, phControls);
    }
}

#ifdef _WIN64
// CMDLCache::PreloadModel submits its VTX filename directly to the queued
// loader.  That path bypasses both OpenEx and AsyncReadMultipleCreditAlloc, so
// redirect the submitted job only after confirming a matching SW VTX exists.
// CQueuedLoader::AddJob copies the filename before returning, making the local
// replacement string safe for the duration of this call.
Define_method_Hook(bool, IQueuedLoader_AddJob, void*, const LoaderJob_t* pLoaderJob)
{
    if (!pLoaderJob || !pLoaderJob->m_pFilename ||
        !g_pFileSystem || !IFileSystem_OpenEx_trampoline()) {
        return IQueuedLoader_AddJob_trampoline()(_this, pLoaderJob);
    }

    try {
        const char* hardwareExtensions[] = { ".dx90.vtx", ".dx80.vtx", ".dx70.vtx" };
        const size_t filenameLength = strlen(pLoaderJob->m_pFilename);
        const char* matchedExtension = nullptr;

        for (const char* extension : hardwareExtensions) {
            const size_t extensionLength = strlen(extension);
            if (filenameLength > extensionLength &&
                _stricmp(pLoaderJob->m_pFilename + filenameLength - extensionLength,
                    extension) == 0) {
                matchedExtension = extension;
                break;
            }
        }

        ResolvedFileLocation replacementFile;
        bool compatibilityUsesSoftwareVtx = false;
        const bool redirectedToCompatibilityFile =
            ResolvePreferredCompatibilityFileLocation(
                g_pFileSystem, pLoaderJob->m_pFilename,
                pLoaderJob->m_pPathID,
                (void*)IFileSystem_OpenEx_trampoline(), replacementFile,
                compatibilityUsesSoftwareVtx);
        if (!redirectedToCompatibilityFile && matchedExtension &&
            !ShouldKeepLegacyHardwareVtx()) {
            ResolveMatchingSoftwareVtxPath(
                g_pFileSystem, pLoaderJob->m_pFilename,
                pLoaderJob->m_pPathID,
                (void*)IFileSystem_OpenEx_trampoline(), replacementFile);
        }
        if (replacementFile.filename.empty()) {
            return IQueuedLoader_AddJob_trampoline()(_this, pLoaderJob);
        }

        LoaderJob_t redirectedJob = *pLoaderJob;
        redirectedJob.m_pFilename = replacementFile.filename.c_str();
        redirectedJob.m_pPathID = replacementFile.pathID;

        if (GlobalConvars::r_hwskin_debug &&
            GlobalConvars::r_hwskin_debug->GetBool()) {
            Msg("[gmRTX - Model Load] QUEUED USING %s model file: %s -> %s (%s)\n",
                redirectedToCompatibilityFile
                    ? (compatibilityUsesSoftwareVtx
                        ? "compatibility SW-VTX"
                        : "exact compatibility")
                    : "SW-VTX",
                pLoaderJob->m_pFilename, replacementFile.filename.c_str(),
                replacementFile.pathID
                    ? replacementFile.pathID
                    : "<default>");
        }

        return IQueuedLoader_AddJob_trampoline()(_this, &redirectedJob);
    }
    catch (...) {
        Warning("[gmRTX - Model Load Fixes] Exception while redirecting queued VTX job\n");
        return IQueuedLoader_AddJob_trampoline()(_this, pLoaderJob);
    }
}
#endif


#ifdef _WIN64
using CMDLCacheFlushAllFn = void(__fastcall*)(void*, MDLCacheFlush_t);
static void* g_pReloadMDLCache;
static CMDLCacheFlushAllFn g_pMDLCacheFlushAll;
#else
static IMDLCache* g_pReloadMDLCache;
#endif
static IVEngineClient* engineClient;

void ForceModelReload() { 

    Msg("[gmRTX - Model Load Fixes] Forcing model reload...\n");
#ifdef _WIN64
    if (g_pReloadMDLCache && g_pMDLCacheFlushAll) {
        // Only discard the VTX-derived hardware meshes. Flushing the complete
        // cache here can invalidate model data which is already in use while
        // the client Lua state is starting up.
        __try {
            g_pMDLCacheFlushAll(g_pReloadMDLCache, MDLCACHE_FLUSH_STUDIOHWDATA);
            Msg("[gmRTX - Model Load Fixes] Successfully flushed model hardware cache\n");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Warning("[gmRTX - Model Load Fixes] Exception while flushing model hardware cache\n");
        }
    }
    else {
        Warning("[gmRTX - Model Load Fixes] Couldn't access the validated MDL cache flush function\n");
    }
#else
    if (g_pReloadMDLCache) {
        g_pReloadMDLCache->Flush(MDLCACHE_FLUSH_STUDIOHWDATA);
        Msg("[gmRTX - Model Load Fixes] Successfully flushed model hardware cache\n");
    }
    else {
        Warning("[gmRTX - Model Load Fixes] Couldn't access MDL cache to force reload\n");
    }
#endif
}
void ForceModelReloadViaEngine() {
    // Get the engine client interface
    if (engineClient) {
        // Use safer commands that won't crash (r_flushlod crashes)
        engineClient->ClientCmd_Unrestricted("mat_reloadallmaterials");

        Msg("[gmRTX - Model Load Fixes] Executed engine reload commands\n");
    }
    else {
        Warning("[gmRTX - Model Load Fixes] Couldn't access engine client, early loaded map models will not be reloaded in their RTX Remix friendly .sw.vtx form!\n");
    }
}

void ModelLoadHooks::ReloadModelsAfterSettings() {
    Msg("[gmRTX - Model Load Fixes] Reloading models after Lua restored hardware-skinning settings\n");
    ForceModelReload();
}

void ModelLoadHooks::Initialize() {
    try {

        //Sys_LoadInterface CRASHES for some reason?????
#ifdef _WIN32
        using CreateInterfaceFn = void* (*)(const char* pName, int* pReturnCode);

#ifdef _WIN64
        // The active GMod x64 MDLCache004 vtable has additional methods which
        // are absent from the public SDK interface. Resolve the concrete
        // Flush(MDLCacheFlush_t) implementation instead of making an unsafe
        // SDK-layout virtual call.
        HMODULE datacacheModule = GetModuleHandleA("datacache.dll");
        if (!datacacheModule) {
            Warning("[gmRTX - Model Load Fixes] - datacache.dll is not loaded; early model hardware data cannot be flushed\n");
        }
        else {
            CreateInterfaceFn datacacheCreateInterface =
                (CreateInterfaceFn)GetProcAddress(datacacheModule, "CreateInterface");
            int mdlCacheReturnCode = 0;
            g_pReloadMDLCache = datacacheCreateInterface
                ? datacacheCreateInterface(MDLCACHE_INTERFACE_VERSION, &mdlCacheReturnCode)
                : nullptr;

            static const char mdlCacheFlushAllSignature[] =
                "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 "
                "48 89 7C 24 20 41 56 48 83 EC 20 0F B7 59 50 "
                "8B F2 48 8B F9 41 BE FF FF 00 00";
            g_pMDLCacheFlushAll = reinterpret_cast<CMDLCacheFlushAllFn>(
                ScanSign(datacacheModule, mdlCacheFlushAllSignature,
                    sizeof(mdlCacheFlushAllSignature) - 1));

            if (!datacacheCreateInterface || !g_pReloadMDLCache || !g_pMDLCacheFlushAll) {
                Warning("[gmRTX - Model Load Fixes] - Could not resolve validated %s hardware-cache flush (CreateInterface=%p, cache=%p, flush=%p, return code=%d)\n",
                    MDLCACHE_INTERFACE_VERSION, datacacheCreateInterface,
                    g_pReloadMDLCache, g_pMDLCacheFlushAll, mdlCacheReturnCode);
                g_pReloadMDLCache = nullptr;
                g_pMDLCacheFlushAll = nullptr;
            }
            else {
                Msg("[gmRTX - Model Load Fixes] Resolved validated %s hardware-cache flush at %p\n",
                    MDLCACHE_INTERFACE_VERSION, g_pMDLCacheFlushAll);
            }
        }
#endif

        // Similarly for engine
        Msg("[gmRTX - Model Load Fixes] - Loading clientengine\n");
        HMODULE engineModule = LoadLibraryA("engine.dll");
        if (!engineModule) {
            Warning("[gmRTX] - Failed to load engine.dll: error code %d\n", GetLastError());
            return;
        }

        CreateInterfaceFn createInterface = (CreateInterfaceFn)GetProcAddress(engineModule, "CreateInterface");
        if (!createInterface) {
            Warning("[gmRTX] - Failed to get CreateInterface from engine.dll\n");
            FreeLibrary(engineModule);
            return;
        }

        engineClient = (IVEngineClient*)createInterface(VENGINE_CLIENT_INTERFACE_VERSION, nullptr);
        if (!engineClient) {
            Warning("[gmRTX] - Failed to get engine client interface\n");
        }
        else {
            Msg("[gmRTX] - Successfully loaded engine client interface\n");
        }
#else
        Msg("[gmRTX - Model Load Fixes] - Loading datacache\n");
        if (!Sys_LoadInterface("datacache", MDLCACHE_INTERFACE_VERSION, NULL, (void**)&g_pReloadMDLCache))
            Warning("[gmRTX - Model Load Fixes] - Could not load studiorender interface");

        Msg("[gmRTX - Model Load Fixes] - Loading clientengine\n");
        if (!Sys_LoadInterface("engine", VENGINE_CLIENT_INTERFACE_VERSION, NULL, (void**)&engineClient))
            Warning("[gmRTX - Model Load Fixes] - Could not load clientengine interface");

#endif // _WIN32

        // Find the filesystem module
        HMODULE fsModule = GetModuleHandle("filesystem_stdio.dll");
        if (!fsModule) {
            fsModule = GetModuleHandle("filesystem.dll");
        }

        if (!fsModule) {
            Warning("[gmRTX - Model Load Fixes] - Could not find filesystem module");
            return;
        }

        // Resolve the methods through the versioned filesystem interface. The
        // GMod x64 runtime has four additional IAppSystem ABI slots which are
        // absent from the public Source SDK header, so its concrete indices are
        // four higher than the SDK-derived VFileSystem022 indices.
        CreateInterfaceFn fsCreateInterface =
            (CreateInterfaceFn)GetProcAddress(fsModule, "CreateInterface");
        if (!fsCreateInterface) {
            Warning("[gmRTX - Model Load Fixes] - filesystem CreateInterface export not found\n");
            return;
        }

        int fsReturnCode = 0;
        g_pFileSystem = (IFileSystem*)fsCreateInterface(FILESYSTEM_INTERFACE_VERSION, &fsReturnCode);
        if (!g_pFileSystem) {
            Warning("[gmRTX - Model Load Fixes] - Could not acquire %s (return code %d)\n",
                FILESYSTEM_INTERFACE_VERSION, fsReturnCode);
            return;
        }

        static const size_t GMOD_IAPPSYSTEM_EXTRA_VTABLE_SLOTS = 4;
        static const size_t GET_SEARCH_PATH_VTABLE_INDEX =
            13 + GMOD_IAPPSYSTEM_EXTRA_VTABLE_SLOTS;
        static const size_t OPENEX_VTABLE_INDEX =
            69 + GMOD_IAPPSYSTEM_EXTRA_VTABLE_SLOTS;
        // GMod modules may bypass the public wrapper (SDK slot 37) and invoke
        // AsyncReadMultipleCreditAlloc directly. The runtime wrapper itself
        // dispatches to concrete vtable slot 85, so hook that common endpoint.
        static const size_t ASYNC_READ_MULTIPLE_CREDIT_ALLOC_VTABLE_INDEX = 85;
        void** fsVtable = *(void***)g_pFileSystem;
        void* getSearchPathFunc = fsVtable
            ? fsVtable[GET_SEARCH_PATH_VTABLE_INDEX]
            : nullptr;
        void* openFunc = fsVtable ? fsVtable[OPENEX_VTABLE_INDEX] : nullptr;
        void* asyncReadMultipleCreditAllocFunc = fsVtable
            ? fsVtable[ASYNC_READ_MULTIPLE_CREDIT_ALLOC_VTABLE_INDEX]
            : nullptr;

#ifdef _WIN64
        // CQueuedLoader::AddJob in the active GMod x64 filesystem build. The
        // signature includes its stack frame and active/batching state checks.
        static const char queuedLoaderAddJobSignature[] =
            "40 56 57 48 81 EC A8 02 00 00 48 8B 05 ? ? ? ? "
            "48 33 C4 48 89 84 24 80 02 00 00 80 79 09 00 "
            "48 8B FA 48 8B F1";
        void* queuedLoaderAddJobFunc = ScanSign(
            fsModule,
            queuedLoaderAddJobSignature,
            sizeof(queuedLoaderAddJobSignature) - 1);
#endif

        if (!openFunc || !asyncReadMultipleCreditAllocFunc) {
            Warning("[gmRTX - Model Load Fixes] - Could not resolve filesystem methods from %s vtable (OpenEx=%p, AsyncReadMultipleCreditAlloc=%p)\n",
                FILESYSTEM_INTERFACE_VERSION, openFunc,
                asyncReadMultipleCreditAllocFunc);
            return;
        }

#ifdef _WIN64
        // Fail safely if a future filesystem build changes the ABI again.
        // OpenEx starts with its nonvolatile-register save sequence; the async
        // wrapper reserves shadow space, loads the vtable, preserves controls,
        // and clears the allocation-credit filename argument.
        static const unsigned char expectedOpenExPrologue[] = {
            0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41
        };
        static const unsigned char expectedAsyncReadMultipleCreditAllocPrologue[] = {
            0x40, 0x53, 0x56, 0x41, 0x55, 0x48, 0x83, 0xEC,
            0x40, 0x48, 0x8B, 0xF1, 0x41, 0x8B, 0xD8
        };
        static const unsigned char expectedGetSearchPathPrologue[] = {
            0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
            0x55, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x00
        };
        if (memcmp(openFunc, expectedOpenExPrologue,
                   sizeof(expectedOpenExPrologue)) != 0 ||
            memcmp(asyncReadMultipleCreditAllocFunc,
                   expectedAsyncReadMultipleCreditAllocPrologue,
                   sizeof(expectedAsyncReadMultipleCreditAllocPrologue)) != 0) {
            Warning("[gmRTX - Model Load Fixes] - Filesystem ABI validation failed; hooks disabled (OpenEx slot %zu, AsyncReadMultipleCreditAlloc slot %zu)\n",
                OPENEX_VTABLE_INDEX,
                ASYNC_READ_MULTIPLE_CREDIT_ALLOC_VTABLE_INDEX);
            return;
        }

        if (getSearchPathFunc &&
            memcmp(getSearchPathFunc, expectedGetSearchPathPrologue,
                sizeof(expectedGetSearchPathPrologue)) == 0) {
            g_pFileSystemGetSearchPath =
                reinterpret_cast<IFileSystemGetSearchPathFn>(
                    getSearchPathFunc);
            if (!RefreshGameSearchPaths()) {
                Warning("[gmRTX - Model Load Fixes] - GAME search paths were unavailable; alternate mounted SW VTX lookup disabled\n");
            }
        }
        else {
            Warning("[gmRTX - Model Load Fixes] - GetSearchPath ABI validation failed; alternate mounted SW VTX lookup disabled (slot %zu, function=%p)\n",
                GET_SEARCH_PATH_VTABLE_INDEX, getSearchPathFunc);
        }

        const bool hl2ModelsAvailable =
            !g_hl2MiscCompatibilityRoot.empty();
        const bool lamarrAvailable =
            !g_lamarrCompatibilityRoot.empty();
        const bool handsAvailable =
            !g_handsCompatibilityRoot.empty();
        if (hl2ModelsAvailable || lamarrAvailable || handsAvailable) {
            if (!hl2ModelsAvailable || !lamarrAvailable || !handsAvailable) {
                Warning("[gmRTX - Model Load Fixes] - HL2 RTX mount is incomplete (hl2=%d, lamarr=%d, hands=%d); affected model-family redirects will fall back to normal GAME lookup\n",
                    hl2ModelsAvailable, lamarrAvailable, handsAvailable);
            }
            else if (GlobalConvars::r_hwskin_debug &&
                GlobalConvars::r_hwskin_debug->GetBool()) {
                Msg("[gmRTX - Model Load Fixes] Launcher-managed HL2 RTX model sources are available (hl2=%s, lamarr=%s, hands=%s)\n",
                    g_hl2MiscCompatibilityRoot.c_str(),
                    g_lamarrCompatibilityRoot.c_str(),
                    g_handsCompatibilityRoot.c_str());
            }
        }
#endif

        Msg("[gmRTX - Model Load Fixes] Resolved %s::OpenEx at %p (vtable slot %zu)\n",
            FILESYSTEM_INTERFACE_VERSION, openFunc, OPENEX_VTABLE_INDEX);
        Msg("[gmRTX - Model Load Fixes] Resolved %s::AsyncReadMultipleCreditAlloc at %p (vtable slot %zu)\n",
            FILESYSTEM_INTERFACE_VERSION, asyncReadMultipleCreditAllocFunc,
            ASYNC_READ_MULTIPLE_CREDIT_ALLOC_VTABLE_INDEX);

        // OpenEx remains hooked for synchronous VTX users and provides the
        // original trampoline used by the async checksum validation above.
        Setup_Hook(IFileSystem_OpenEx, openFunc);
        Msg("[gmRTX - Model Load Fixes] Successfully hooked IFileSystem::OpenEx\n");

        Setup_Hook(IFileSystem_AsyncReadMultipleCreditAlloc,
            asyncReadMultipleCreditAllocFunc);
        Msg("[gmRTX - Model Load Fixes] Successfully hooked IFileSystem::AsyncReadMultipleCreditAlloc\n");

#ifdef _WIN64
        if (!queuedLoaderAddJobFunc) {
            Warning("[gmRTX - Model Load Fixes] - Could not resolve CQueuedLoader::AddJob; queued model preloads may retain DX90 topology\n");
        }
        else {
            Setup_Hook(IQueuedLoader_AddJob, queuedLoaderAddJobFunc);
            if (!IQueuedLoader_AddJob_hook.IsEnabled() ||
                !IQueuedLoader_AddJob_trampoline()) {
                Warning("[gmRTX - Model Load Fixes] - Failed to hook CQueuedLoader::AddJob; queued model preloads may retain DX90 topology\n");
                if (IQueuedLoader_AddJob_hook.IsEnabled())
                    IQueuedLoader_AddJob_hook.Disable();
            }
            else {
                Msg("[gmRTX - Model Load Fixes] Successfully hooked CQueuedLoader::AddJob at %p\n",
                    queuedLoaderAddJobFunc);
            }
        }
#endif

        if (!IFileSystem_OpenEx_hook.IsEnabled() ||
            !IFileSystem_AsyncReadMultipleCreditAlloc_hook.IsEnabled()) {
            Warning("[gmRTX - Model Load Fixes] - One or more filesystem hooks failed to enable\n");
            if (IFileSystem_AsyncReadMultipleCreditAlloc_hook.IsEnabled())
                IFileSystem_AsyncReadMultipleCreditAlloc_hook.Disable();
            if (IFileSystem_OpenEx_hook.IsEnabled())
                IFileSystem_OpenEx_hook.Disable();
            return;
        }

        if (!IFileSystem_OpenEx_trampoline() ||
            !IFileSystem_AsyncReadMultipleCreditAlloc_trampoline()) {
            Warning("[gmRTX - Model Load Fixes] - Filesystem hook trampoline creation failed\n");
            IFileSystem_AsyncReadMultipleCreditAlloc_hook.Disable();
            IFileSystem_OpenEx_hook.Disable();
            return;
        }

        /*
         * VFileSystem022 is required here. If its ABI ever changes, fail
         * during initialization rather than silently keeping DX90 topology.
         */
        if (strcmp(FILESYSTEM_INTERFACE_VERSION, "VFileSystem022") != 0) {
            Warning("[gmRTX - Model Load Fixes] - Unsupported filesystem ABI: %s\n",
                FILESYSTEM_INTERFACE_VERSION);
            IFileSystem_AsyncReadMultipleCreditAlloc_hook.Disable();
            IFileSystem_OpenEx_hook.Disable();
            return;
        }

        // cl_rtx.lua owns persistence for r_forcehwskin. Do not rebuild model
        // meshes here while require("RTXFixesBinary") is still running: Lua
        // has not restored the saved value yet. The autorun script requests a
        // reload immediately after applying that setting.
        Msg("[gmRTX - Model Load Fixes] Hooks ready; deferring model reload until Lua restores hardware-skinning settings\n");
    }
    catch (...) {
        Msg("[gmRTX - Model Load Fixes] Exception in ModelLoadHooks::Initialize\n");
    }
}


void ModelLoadHooks::Shutdown() {
    try {
        // Safely disable hooks
#ifdef _WIN64
        if (IQueuedLoader_AddJob_hook.IsEnabled())
            IQueuedLoader_AddJob_hook.Disable();
#endif
        if (IFileSystem_AsyncReadMultipleCreditAlloc_hook.IsEnabled())
            IFileSystem_AsyncReadMultipleCreditAlloc_hook.Disable();
        if (IFileSystem_OpenEx_hook.IsEnabled())
            IFileSystem_OpenEx_hook.Disable();
        
        // Clean up interface pointers
        g_pFileSystem = nullptr;
        g_pReloadMDLCache = nullptr;
#ifdef _WIN64
        g_pMDLCacheFlushAll = nullptr;
        g_pFileSystemGetSearchPath = nullptr;
#endif
        g_gameSearchPaths.clear();
        g_hl2MiscCompatibilityRoot.clear();
        g_lamarrCompatibilityRoot.clear();
        g_handsCompatibilityRoot.clear();
        engineClient = nullptr;
        
        Msg("[gmRTX - Model Load Fixes] Shutdown complete\n");
    }
    catch (...) {
        Error("[gmRTX - Model Load Fixes] Exception during shutdown\n");
    }
}
