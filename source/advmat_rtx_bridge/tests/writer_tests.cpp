#ifdef NDEBUG
#undef NDEBUG
#endif
#include "advmat_rtx_bridge/bridge.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace advmat::rtx_bridge;

namespace {

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void PutU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset + 0] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16u);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24u);
}

void WriteMinimalDds(const std::filesystem::path& path,
                     std::uint32_t width = 1, std::uint32_t height = 1,
                     std::uint32_t mipMapCount = 1,
                     std::optional<std::size_t> payloadBytes = std::nullopt,
                     std::uint32_t pixelFlags = 0x1u | 0x40u) {
    const auto pixelBytes = payloadBytes.value_or(
        static_cast<std::size_t>(width) * height * 4u);
    std::vector<std::uint8_t> bytes(128 + pixelBytes, 0);
    bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
    PutU32(bytes, 4, 124);
    PutU32(bytes, 8, 0x100Fu);
    PutU32(bytes, 12, height);
    PutU32(bytes, 16, width);
    PutU32(bytes, 20, width * 4u);
    PutU32(bytes, 24, 1);
    PutU32(bytes, 28, mipMapCount);
    PutU32(bytes, 76, 32);
    PutU32(bytes, 80, pixelFlags);
    PutU32(bytes, 88, 32);
    PutU32(bytes, 92, 0x00ff0000u);
    PutU32(bytes, 96, 0x0000ff00u);
    PutU32(bytes, 100, 0x000000ffu);
    PutU32(bytes, 104, 0xff000000u);
    PutU32(bytes, 108, 0x1000u);
    constexpr std::uint8_t pixel[]{0x10, 0x20, 0x30, 0xff};
    for (std::size_t offset = 128; offset < bytes.size(); ++offset) {
        bytes[offset] = pixel[(offset - 128) % 4];
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

constexpr std::uint32_t TestFourCc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

void WriteLegacyFourCcDds(const std::filesystem::path& path,
                          std::uint32_t fourCc, std::uint32_t width,
                          std::uint32_t height, std::uint32_t mipMapCount,
                          std::size_t payloadBytes) {
    std::vector<std::uint8_t> bytes(128 + payloadBytes, 0);
    bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
    PutU32(bytes, 4, 124);
    PutU32(bytes, 12, height);
    PutU32(bytes, 16, width);
    PutU32(bytes, 24, 1);
    PutU32(bytes, 28, mipMapCount);
    PutU32(bytes, 76, 32);
    PutU32(bytes, 80, 0x4u);
    PutU32(bytes, 84, fourCc);
    PutU32(bytes, 108, 0x1000u);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void WriteMalformedDx10Dds(const std::filesystem::path& path) {
    std::vector<std::uint8_t> bytes(128, 0);
    bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
    PutU32(bytes, 4, 124);
    PutU32(bytes, 12, 4);
    PutU32(bytes, 16, 4);
    PutU32(bytes, 24, 1);
    PutU32(bytes, 28, 1);
    PutU32(bytes, 76, 32);
    PutU32(bytes, 80, 0x4u);
    PutU32(bytes, 84, static_cast<std::uint32_t>('D') |
        (static_cast<std::uint32_t>('X') << 8u) |
        (static_cast<std::uint32_t>('1') << 16u) |
        (static_cast<std::uint32_t>('0') << 24u));
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void WriteDx10Bc7Dds(const std::filesystem::path& path) {
    std::vector<std::uint8_t> bytes(148 + 16, 0);
    bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
    PutU32(bytes, 4, 124);
    PutU32(bytes, 12, 4);
    PutU32(bytes, 16, 4);
    PutU32(bytes, 24, 1);
    PutU32(bytes, 28, 1);
    PutU32(bytes, 76, 32);
    PutU32(bytes, 80, 0x4u);
    PutU32(bytes, 84, static_cast<std::uint32_t>('D') |
        (static_cast<std::uint32_t>('X') << 8u) |
        (static_cast<std::uint32_t>('1') << 16u) |
        (static_cast<std::uint32_t>('0') << 24u));
    PutU32(bytes, 128, 98); // DXGI_FORMAT_BC7_UNORM
    PutU32(bytes, 132, 3); // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    PutU32(bytes, 140, 1);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

#ifdef _WIN32
void PutU16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset + 0] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
}

void WriteNormalMipFixture(const std::filesystem::path& path) {
    // A 2x2 BGRA BMP containing two +X and two +Z normals. Their raw box
    // average is length ~0.71; the generated 1x1 mip must be renormalized.
    std::vector<std::uint8_t> bytes(70, 0);
    bytes[0] = 'B';
    bytes[1] = 'M';
    PutU32(bytes, 2, static_cast<std::uint32_t>(bytes.size()));
    PutU32(bytes, 10, 54);
    PutU32(bytes, 14, 40);
    PutU32(bytes, 18, 2);
    PutU32(bytes, 22, 2);
    PutU16(bytes, 26, 1);
    PutU16(bytes, 28, 32);
    PutU32(bytes, 34, 16);
    const std::uint8_t pixels[]{
        128, 128, 255, 255, 255, 128, 128, 255,
        128, 128, 255, 255, 255, 128, 128, 255,
    };
    std::copy(std::begin(pixels), std::end(pixels), bytes.begin() + 54);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}
#endif

void WriteText(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

std::string LegacyLayer(const std::vector<std::string>& hashes) {
    std::string output = "#usda 1.0\n(\n    upAxis = \"Z\"\n)\n\n"
                         "over \"RootNode\"\n{\n    over \"Looks\"\n    {\n";
    for (const auto& hash : hashes) {
        output += "        over \"mat_" + hash + "\"\n"
                  "        {\n            over \"Shader\"\n            {\n"
                  "                uniform asset info:mdl:sourceAsset = @AperturePBR_Opacity.mdl@\n"
                  "                float inputs:reflection_roughness_constant = 0.5\n"
                  "            }\n        }\n\n";
    }
    output += "    }\n}\n";
    return output;
}

std::string ModLayer(const std::vector<std::string>& profileIds) {
    std::string output = "#usda 1.0\n(\n"
                         "    customLayerData = {\n"
                         "        string lightspeed_game_name = \"Garry's Mod (x64)\"\n"
                         "        string lightspeed_layer_type = \"replacement\"\n"
                         "    }\n"
                         "    metersPerUnit = 0.01\n"
                         "    subLayers = [\n";
    for (std::size_t index = 0; index < profileIds.size(); ++index) {
        output += "        @./profiles/" + profileIds[index] + ".usda@";
        if (index + 1 < profileIds.size()) output += ',';
        output += '\n';
    }
    output += "    ]\n"
              "    timeCodesPerSecond = 24\n"
              "    upAxis = \"Z\"\n"
              ")\n";
    return output;
}

bool RootReferences(const std::string& rootContents,
                    const std::filesystem::path& layerPath) {
    return rootContents.find("./profiles/" + layerPath.filename().string()) !=
        std::string::npos;
}

bool HasTextureInput(const std::string& layerContents, const std::string& input,
                     const std::string& colorSpace) {
    const auto inputPosition = layerContents.find("asset inputs:" + input + " = @");
    if (inputPosition == std::string::npos) return false;
    const auto inputEnd = layerContents.find("                )", inputPosition);
    if (inputEnd == std::string::npos) return false;
    const auto colorPosition = layerContents.find(
        "colorSpace = \"" + colorSpace + "\"", inputPosition);
    return colorPosition != std::string::npos && colorPosition < inputEnd;
}

bool IsVersionedHashLayer(const std::filesystem::path& path,
                          std::string_view hash) {
    const auto filename = path.filename().string();
    const std::string prefix = "hash_" + std::string(hash) + "_";
    constexpr std::string_view suffix = ".usda";
    return filename.size() == prefix.size() + 16 + suffix.size() &&
        filename.compare(0, prefix.size(), prefix) == 0 &&
        filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::filesystem::path> FindVersionedHashLayers(
    const std::filesystem::path& profiles, std::string_view hash) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(profiles, ec) || ec) return found;
    for (std::filesystem::directory_iterator iterator(profiles, ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        if (iterator->is_regular_file(ec) && !ec &&
            IsVersionedHashLayer(iterator->path(), hash)) {
            found.push_back(iterator->path());
        }
    }
    assert(!ec);
    std::sort(found.begin(), found.end());
    return found;
}

std::size_t CountRegularFiles(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) return 0;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator iterator(directory, ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        if (iterator->is_regular_file(ec) && !ec) ++count;
    }
    assert(!ec);
    return count;
}

std::size_t CountRegularFilesRecursive(const std::filesystem::path& directory) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) return 0;
    std::size_t count = 0;
    for (std::filesystem::recursive_directory_iterator iterator(directory, ec), end;
         !ec && iterator != end; iterator.increment(ec)) {
        if (iterator->is_regular_file(ec) && !ec) ++count;
    }
    assert(!ec);
    return count;
}

std::string HexHash(std::uint64_t value) {
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0') <<
        std::setw(16) << value;
    return output.str();
}

} // namespace

int main() {
    std::string error;
    std::string canonical;
    assert(ValidateProfileId("material_0123", error));
    assert(!ValidateProfileId("../escape", error));
    assert(CanonicalizeHash("0xabc", canonical, error));
    assert(canonical == "0000000000000ABC");
    assert(!CanonicalizeHash("0x0", canonical, error));
    assert(!CanonicalizeHash("xyz", canonical, error));
    BridgeOperation operation = BridgeOperation::Preview;
    assert(ParseBridgeOperation("", operation, error));
    assert(operation == BridgeOperation::Commit);
    assert(ParseBridgeOperation("PREVIEW", operation, error));
    assert(operation == BridgeOperation::Preview);
    assert(ParseBridgeOperation("restore", operation, error));
    assert(operation == BridgeOperation::Restore);
    assert(ParseBridgeOperation("clear", operation, error));
    assert(operation == BridgeOperation::Clear);
    assert(!ParseBridgeOperation("temporary", operation, error));

    Config defaults;
    assert(defaults.maxTextureBytes == 32ull * 1024ull * 1024ull);
    assert(defaults.maxTextureDimension == 2048);
    assert(defaults.maxActiveProfiles == 4096);
    assert(defaults.maxActiveTextureFiles == 8192);
    assert(defaults.maxActiveTextureBytes == 2ull * 1024ull * 1024ull * 1024ull);
    assert(kMaxTextureOperations == 4);

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("advmat_rtx_writer_test_" + std::to_string(nonce));
    std::filesystem::create_directories(root / "garrysmod" / "data");
    std::filesystem::create_directories(root / "rtx-remix" / "mods");
    const auto nestedExecutable = root / "bin" / "win64" / "gmod.exe";
    WriteText(nestedExecutable, "test executable placeholder");
    const auto discoveredRoot = FindGameRootFromExecutable(nestedExecutable);
    assert(discoveredRoot && *discoveredRoot == root.lexically_normal());
    assert(!FindGameRootFromExecutable("bin/win64/gmod.exe"));

    auto tooDeepDirectory = root;
    for (std::size_t index = 0; index < kMaxGameRootAncestorDepth + 1; ++index) {
        tooDeepDirectory /= "nested";
    }
    const auto tooDeepExecutable = tooDeepDirectory / "gmod.exe";
    WriteText(tooDeepExecutable, "test executable placeholder");
    assert(!FindGameRootFromExecutable(tooDeepExecutable));

    const auto source = root / "garrysmod" / "data" / "source.dds";
    WriteMinimalDds(source);

    Config config;
    config.gameRoot = root;
    Bridge bridge(std::move(config));
    const auto capabilities = bridge.GetCapabilities();
    assert(capabilities.available);
    assert(!capabilities.livePreview);
    assert(capabilities.authoritativeReset);
    assert(capabilities.synchronous);
    assert(capabilities.maxTextureBytes == kDefaultMaxTextureBytes);
    assert(capabilities.maxTextureDimension == kDefaultMaxTextureDimension);
    assert(capabilities.maxTextureOperations == kMaxTextureOperations);
    assert(capabilities.maxActiveProfiles == kDefaultMaxActiveProfiles);
    assert(capabilities.maxActiveTextureFiles == kDefaultMaxActiveTextureFiles);
    assert(capabilities.maxActiveTextureBytes == kDefaultMaxActiveTextureBytes);
    ApplyRequest request;
    request.profileId = "material_0123456789ABCDEF";
    request.hashes = {"0x0123456789abcdef", "0123456789ABCDF0"};
    request.material.albedo = Vec3{0.8f, 0.7f, 0.6f};
    request.material.roughness = 0.25f;
    request.material.metallic = 0.75f;
    request.material.spriteSheetRows = 2;
    request.material.spriteSheetColumns = 3;
    request.material.spriteSheetFps = 4;
    request.material.filterMode = 1;
    request.material.wrapModeU = 1;
    request.material.wrapModeV = 2;
    request.material.blendType = 8;
    request.material.alphaTestType = 7;
    request.material.alphaReferenceValue = 0.5f;
    request.material.textures.push_back({TextureRole::Albedo, source, {}});

    const auto expectedMod = root / "rtx-remix" / "mods" / "!advanced_material_editor";

    // Destination containment is physical, not merely lexical. Exercise both
    // a junction/symlink present during construction and components planted
    // after provider discovery. Windows may require Developer Mode for test
    // symlink creation, so each attack case is conditional on host support.
    const auto linkedRoot = root / "linked_game";
    const auto linkedExternal = root / "linked_external";
    std::filesystem::create_directories(linkedRoot / "garrysmod" / "data");
    std::filesystem::create_directories(linkedRoot / "rtx-remix");
    WriteText(linkedExternal / "keep.txt", "outside constructor sentinel");
    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        linkedExternal, linkedRoot / "rtx-remix" / "mods", linkError);
    if (!linkError) {
        Config linkedConfig;
        linkedConfig.gameRoot = linkedRoot;
        Bridge linkedBridge(std::move(linkedConfig));
        assert(!linkedBridge.GetCapabilities().available);
        assert(ReadText(linkedExternal / "keep.txt") == "outside constructor sentinel");
    }

    const auto redirectedRoot = root / "redirected_game";
    const auto redirectedExternal = root / "redirected_external";
    const auto redirectedSource = redirectedRoot / "garrysmod" / "data" / "source.dds";
    std::filesystem::create_directories(redirectedRoot / "garrysmod" / "data");
    std::filesystem::create_directories(redirectedRoot / "rtx-remix" / "mods");
    WriteMinimalDds(redirectedSource);
    WriteText(redirectedExternal / "keep.txt", "outside runtime sentinel");
    Config redirectedConfig;
    redirectedConfig.gameRoot = redirectedRoot;
    Bridge redirectedBridge(std::move(redirectedConfig));
    assert(redirectedBridge.GetCapabilities().available);
    ApplyRequest redirectedRequest = request;
    redirectedRequest.material.textures.front().source = redirectedSource;
    const auto redirectedMods = redirectedRoot / "rtx-remix" / "mods";
    const auto redirectedMod = redirectedMods / "!advanced_material_editor";

    linkError.clear();
    std::filesystem::remove(redirectedMods, linkError);
    assert(!linkError);
    std::filesystem::create_directory_symlink(redirectedExternal, redirectedMods, linkError);
    if (!linkError) {
        const auto redirectedAncestor = redirectedBridge.ApplyLegacyMaterial(redirectedRequest);
        assert(!redirectedAncestor.ok);
        assert(ReadText(redirectedExternal / "keep.txt") == "outside runtime sentinel");
        assert(!std::filesystem::exists(redirectedExternal / "!advanced_material_editor"));
        std::filesystem::remove(redirectedMods, linkError);
        assert(!linkError);
    } else {
        linkError.clear();
    }
    std::filesystem::create_directories(redirectedMod);

    linkError.clear();
    std::filesystem::create_directory_symlink(
        redirectedExternal, redirectedMod / "textures", linkError);
    if (!linkError) {
        const auto redirectedTexture = redirectedBridge.ApplyLegacyMaterial(redirectedRequest);
        assert(!redirectedTexture.ok);
        assert(ReadText(redirectedExternal / "keep.txt") == "outside runtime sentinel");
        std::filesystem::remove(redirectedMod / "textures", linkError);
        assert(!linkError);
    } else {
        linkError.clear();
    }

    const auto redirectedTextureDirectory = redirectedMod / "textures";
    std::filesystem::create_directories(redirectedTextureDirectory);
    const auto redirectedNestedTexture = redirectedTextureDirectory / "nested_reparse";
    linkError.clear();
    std::filesystem::create_directory_symlink(
        redirectedExternal, redirectedNestedTexture, linkError);
    if (!linkError) {
        const auto redirectedNested =
            redirectedBridge.ApplyLegacyMaterial(redirectedRequest);
        assert(!redirectedNested.ok);
        assert(ReadText(redirectedExternal / "keep.txt") == "outside runtime sentinel");
        std::filesystem::remove(redirectedNestedTexture, linkError);
        assert(!linkError);
    } else {
        linkError.clear();
    }
    std::filesystem::remove(redirectedTextureDirectory, linkError);
    assert(!linkError);

    linkError.clear();
    std::filesystem::create_directory_symlink(
        redirectedExternal, redirectedMod / "profiles", linkError);
    if (!linkError) {
        auto redirectedProfileRequest = redirectedRequest;
        redirectedProfileRequest.material.textures.clear();
        const auto redirectedProfile =
            redirectedBridge.ApplyLegacyMaterial(redirectedProfileRequest);
        assert(!redirectedProfile.ok);
        assert(ReadText(redirectedExternal / "keep.txt") == "outside runtime sentinel");
        std::filesystem::remove(redirectedMod / "profiles", linkError);
        assert(!linkError);
    } else {
        linkError.clear();
    }

    const auto redirectedExternalRoot = redirectedExternal / "external_mod.usda";
    WriteText(redirectedExternalRoot, "outside root sentinel");
    linkError.clear();
    std::filesystem::create_symlink(
        redirectedExternalRoot, redirectedMod / "mod.usda", linkError);
    if (!linkError) {
        const auto redirectedRootLayer =
            redirectedBridge.ApplyLegacyMaterial(redirectedRequest);
        assert(!redirectedRootLayer.ok);
        assert(ReadText(redirectedExternalRoot) == "outside root sentinel");
        std::filesystem::remove(redirectedMod / "mod.usda", linkError);
        assert(!linkError);
    }

    request.operation = BridgeOperation::Preview;
    const auto preview = bridge.ApplyLegacyMaterial(request);
    assert(!preview.ok);
    assert(preview.code == "preview_unsupported");
    // A preview request must not create even the addon's dedicated mod folder.
    assert(!std::filesystem::exists(expectedMod));
    request.operation = BridgeOperation::Commit;

    auto invalidSpriteSheet = request;
    invalidSpriteSheet.material.spriteSheetRows = 0;
    const auto invalidRows = bridge.ApplyLegacyMaterial(invalidSpriteSheet);
    assert(!invalidRows.ok && invalidRows.code == "invalid_material");
    invalidSpriteSheet = request;
    invalidSpriteSheet.material.spriteSheetColumns = 0;
    const auto invalidColumns = bridge.ApplyLegacyMaterial(invalidSpriteSheet);
    assert(!invalidColumns.ok && invalidColumns.code == "invalid_material");
    assert(!std::filesystem::exists(expectedMod));

    const auto applied = bridge.ApplyLegacyMaterial(request);
    if (!applied.ok) {
        std::cerr << applied.code << ": " << applied.message << '\n';
        return 1;
    }
    assert(bridge.ModDirectory() == expectedMod);
    assert(std::filesystem::is_regular_file(applied.layerPath));
    assert(applied.layerPaths.size() == 2);
    assert(IsVersionedHashLayer(applied.layerPaths[0], "0123456789ABCDEF"));
    assert(IsVersionedHashLayer(applied.layerPaths[1], "0123456789ABCDF0"));
    assert(applied.importedTextures.size() == 1);
    assert(std::filesystem::is_regular_file(applied.importedTextures.front()));

    const auto firstProfileText = ReadText(applied.layerPaths[0]);
    const auto secondProfileText = ReadText(applied.layerPaths[1]);
    assert(firstProfileText.find("mat_0123456789ABCDEF") != std::string::npos);
    assert(firstProfileText.find("mat_0123456789ABCDF0") == std::string::npos);
    assert(secondProfileText.find("mat_0123456789ABCDF0") != std::string::npos);
    assert(secondProfileText.find("mat_0123456789ABCDEF") == std::string::npos);
    assert(firstProfileText.find("AperturePBR_Opacity.mdl") != std::string::npos);
    assert(firstProfileText.find("subIdentifier = \"AperturePBR_Opacity\"") !=
        std::string::npos);
    assert(firstProfileText.find("reflection_roughness_constant = 0.25") != std::string::npos);
    assert(firstProfileText.find("metallic_constant = 0.75") != std::string::npos);
    assert(firstProfileText.find("diffuse_texture") != std::string::npos);
    assert(firstProfileText.find("int inputs:sprite_sheet_rows = 2") != std::string::npos);
    assert(firstProfileText.find("int inputs:sprite_sheet_cols = 3") != std::string::npos);
    assert(firstProfileText.find("int inputs:sprite_sheet_fps = 4") != std::string::npos);
    assert(firstProfileText.find("int inputs:filter_mode = 1") != std::string::npos);
    assert(firstProfileText.find("int inputs:wrap_mode_u = 1") != std::string::npos);
    assert(firstProfileText.find("int inputs:wrap_mode_v = 2") != std::string::npos);
    assert(firstProfileText.find("bool inputs:blend_enabled = 1") != std::string::npos);
    assert(firstProfileText.find("int inputs:blend_type = 8") != std::string::npos);
    assert(firstProfileText.find("int inputs:alpha_test_type = 7") != std::string::npos);
    assert(firstProfileText.find("float inputs:alpha_test_reference_value = 0.5") !=
        std::string::npos);
    assert(firstProfileText.find("uchar inputs:") == std::string::npos);

    const auto modText = ReadText(expectedMod / "mod.usda");
    assert(RootReferences(modText, applied.layerPaths[0]));
    assert(RootReferences(modText, applied.layerPaths[1]));
    assert(modText.find("./profiles/material_0123456789ABCDEF.usda") == std::string::npos);
    assert(modText.find("~gmod_topbr") == std::string::npos);

    // Reapplying an unchanged profile must reuse the content-addressed DDS and
    // avoid touching either USDA layer. This is important because calls are
    // synchronous on the game's client thread.
    const auto oldTime = std::filesystem::file_time_type::clock::now() -
        std::chrono::hours(24);
    std::filesystem::last_write_time(applied.layerPath, oldTime);
    std::filesystem::last_write_time(applied.layerPaths[1], oldTime);
    std::filesystem::last_write_time(expectedMod / "mod.usda", oldTime);
    std::filesystem::last_write_time(applied.importedTextures.front(), oldTime);
    const auto oldProfileTime = std::filesystem::last_write_time(applied.layerPath);
    const auto oldSecondProfileTime = std::filesystem::last_write_time(applied.layerPaths[1]);
    const auto oldModTime = std::filesystem::last_write_time(expectedMod / "mod.usda");
    const auto oldTextureTime = std::filesystem::last_write_time(applied.importedTextures.front());
    const auto reapplied = bridge.ApplyLegacyMaterial(request);
    assert(reapplied.ok);
    assert(reapplied.layerPaths == applied.layerPaths);
    assert(std::filesystem::last_write_time(applied.layerPath) == oldProfileTime);
    assert(std::filesystem::last_write_time(applied.layerPaths[1]) == oldSecondProfileTime);
    assert(std::filesystem::last_write_time(expectedMod / "mod.usda") == oldModTime);
    assert(std::filesystem::last_write_time(applied.importedTextures.front()) == oldTextureTime);

    const auto previewAfterCommit = [&]() {
        auto previewRequest = request;
        previewRequest.operation = BridgeOperation::Preview;
        return bridge.ApplyLegacyMaterial(previewRequest);
    }();
    assert(!previewAfterCommit.ok && previewAfterCommit.code == "preview_unsupported");
    assert(std::filesystem::last_write_time(applied.layerPath) == oldProfileTime);
    assert(std::filesystem::last_write_time(applied.layerPaths[1]) == oldSecondProfileTime);
    assert(std::filesystem::last_write_time(expectedMod / "mod.usda") == oldModTime);
    assert(std::filesystem::last_write_time(applied.importedTextures.front()) == oldTextureTime);

    // A content-addressed filename alone is not enough to trust a pre-existing
    // cache entry. Reapplying must replace a truncated destination rather than
    // publishing a profile that points Remix at malformed DDS data.
    const auto sourceTextureSize = std::filesystem::file_size(source);
    const auto rootBeforeCacheRepair = ReadText(expectedMod / "mod.usda");
    {
        std::fstream cache(applied.importedTextures.front(),
                           std::ios::binary | std::ios::in | std::ios::out);
        assert(cache);
        cache.seekp(128);
        cache.put(static_cast<char>(0xee));
    }
    assert(std::filesystem::file_size(applied.importedTextures.front()) == sourceTextureSize);
    assert(ReadText(applied.importedTextures.front()) != ReadText(source));
    const auto repairedCache = bridge.ApplyLegacyMaterial(request);
    assert(repairedCache.ok);
    assert(std::filesystem::file_size(applied.importedTextures.front()) == sourceTextureSize);
    assert(ReadText(applied.importedTextures.front()) == ReadText(source));
    assert(ReadText(expectedMod / "mod.usda") == rootBeforeCacheRepair);
    std::filesystem::resize_file(applied.importedTextures.front(), 128);
    const auto repairedTruncatedCache = bridge.ApplyLegacyMaterial(request);
    assert(repairedTruncatedCache.ok);
    assert(ReadText(applied.importedTextures.front()) == ReadText(source));
    assert(ReadText(expectedMod / "mod.usda") == rootBeforeCacheRepair);

    // A multi-hash update is staged into new immutable revisions. Failure after
    // the first staged layer must leave both previously active revisions and
    // the root activation ledger byte-for-byte intact.
    const auto committedRootText = ReadText(expectedMod / "mod.usda");
    int stagedLayerCount = 0;
    Config midStageFailureConfig;
    midStageFailureConfig.gameRoot = root;
    midStageFailureConfig.failureInjector = [&stagedLayerCount](std::string_view point) {
        if (point != "apply_after_stage_layer") return false;
        ++stagedLayerCount;
        return stagedLayerCount == 1;
    };
    Bridge midStageFailureBridge(std::move(midStageFailureConfig));
    ApplyRequest changedRequest = request;
    changedRequest.material.roughness = 0.9f;
    const auto midStageFailure = midStageFailureBridge.ApplyLegacyMaterial(changedRequest);
    assert(!midStageFailure.ok && midStageFailure.code == "injected_failure");
    assert(stagedLayerCount == 1);
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);
    assert(ReadText(applied.layerPaths[0]) == firstProfileText);
    assert(ReadText(applied.layerPaths[1]) == secondProfileText);
    assert(RootReferences(ReadText(expectedMod / "mod.usda"), applied.layerPaths[0]));
    assert(RootReferences(ReadText(expectedMod / "mod.usda"), applied.layerPaths[1]));
    assert(FindVersionedHashLayers(expectedMod / "profiles",
        "0123456789ABCDEF").size() == 1);
    assert(FindVersionedHashLayers(expectedMod / "profiles",
        "0123456789ABCDF0").size() == 1);

    // Failure after every new revision is durable but before publication has
    // the same all-old result; active-ledger cleanup removes the inert staged
    // files before the failure is returned.
    bool reachedPrePublish = false;
    Config prePublishFailureConfig;
    prePublishFailureConfig.gameRoot = root;
    prePublishFailureConfig.failureInjector = [&reachedPrePublish](std::string_view point) {
        if (point != "apply_before_root_publish") return false;
        reachedPrePublish = true;
        return true;
    };
    Bridge prePublishFailureBridge(std::move(prePublishFailureConfig));
    changedRequest.material.roughness = 0.8f;
    const auto prePublishFailure =
        prePublishFailureBridge.ApplyLegacyMaterial(changedRequest);
    assert(!prePublishFailure.ok && prePublishFailure.code == "injected_failure");
    assert(reachedPrePublish);
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);
    assert(ReadText(applied.layerPaths[0]) == firstProfileText);
    assert(ReadText(applied.layerPaths[1]) == secondProfileText);
    assert(FindVersionedHashLayers(expectedMod / "profiles",
        "0123456789ABCDEF").size() == 1);
    assert(FindVersionedHashLayers(expectedMod / "profiles",
        "0123456789ABCDF0").size() == 1);

    // Failure after creating a new map and profile revision removes both
    // transaction-owned orphans while the old root remains active.
    ApplyRequest failedGeneration = request;
    failedGeneration.profileId = "failed_transaction";
    failedGeneration.hashes = {"00000000FA11ED01"};
    failedGeneration.material.textures.clear();
    failedGeneration.material.textures.push_back(
        {TextureRole::Anisotropy, source, {}});
    const auto failedGenerationResult =
        prePublishFailureBridge.ApplyLegacyMaterial(failedGeneration);
    assert(!failedGenerationResult.ok &&
        failedGenerationResult.code == "injected_failure");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);
    assert(!std::filesystem::exists(expectedMod / "textures" / "failed_transaction"));
    assert(FindVersionedHashLayers(expectedMod / "profiles",
        "00000000FA11ED01").empty());

    ApplyRequest tooManyOperations = request;
    tooManyOperations.profileId = "operation_limit";
    tooManyOperations.material.textures.front().operations.assign(
        kMaxTextureOperations + 1, TextureOperation{});
    const auto operationLimit = bridge.ApplyLegacyMaterial(tooManyOperations);
    assert(!operationLimit.ok && operationLimit.code == "invalid_material");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    ApplyRequest invalidBlend = request;
    invalidBlend.material.blendType = 9;
    const auto blendLimit = bridge.ApplyLegacyMaterial(invalidBlend);
    assert(!blendLimit.ok && blendLimit.code == "invalid_material");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    ApplyRequest invalidFilter = request;
    invalidFilter.material.filterMode = 2;
    const auto filterLimit = bridge.ApplyLegacyMaterial(invalidFilter);
    assert(!filterLimit.ok && filterLimit.code == "invalid_material");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    ApplyRequest invalidWrap = request;
    invalidWrap.material.wrapModeU = 4;
    const auto wrapLimit = bridge.ApplyLegacyMaterial(invalidWrap);
    assert(!wrapLimit.ok && wrapLimit.code == "invalid_material");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    for (const int invalidAlphaTest : {-1, 8}) {
        ApplyRequest invalidAlpha = request;
        invalidAlpha.material.alphaTestType = invalidAlphaTest;
        const auto alphaLimit = bridge.ApplyLegacyMaterial(invalidAlpha);
        assert(!alphaLimit.ok && alphaLimit.code == "invalid_material");
        assert(ReadText(expectedMod / "mod.usda") == committedRootText);
    }

    const auto oversizedDimensionSource = root / "garrysmod" / "data" / "too_wide.dds";
    WriteMinimalDds(oversizedDimensionSource, kDefaultMaxTextureDimension + 1, 1);
    ApplyRequest oversizedDimension = request;
    oversizedDimension.profileId = "dimension_limit";
    oversizedDimension.material.textures.front().source = oversizedDimensionSource;
    const auto dimensionLimit = bridge.ApplyLegacyMaterial(oversizedDimension);
    assert(!dimensionLimit.ok && dimensionLimit.code == "invalid_dds");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    const auto truncatedPayloadSource =
        root / "garrysmod" / "data" / "truncated_payload.dds";
    WriteMinimalDds(truncatedPayloadSource, 2048, 2048, 1, 4);
    ApplyRequest truncatedPayload = request;
    truncatedPayload.profileId = "truncated_payload";
    truncatedPayload.material.textures.front().source = truncatedPayloadSource;
    const auto truncatedPayloadResult = bridge.ApplyLegacyMaterial(truncatedPayload);
    assert(!truncatedPayloadResult.ok && truncatedPayloadResult.code == "invalid_dds");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    const auto truncatedMipSource =
        root / "garrysmod" / "data" / "truncated_mips.dds";
    WriteMinimalDds(truncatedMipSource, 4, 4, 3);
    ApplyRequest truncatedMips = request;
    truncatedMips.profileId = "truncated_mips";
    truncatedMips.material.textures.front().source = truncatedMipSource;
    const auto truncatedMipResult = bridge.ApplyLegacyMaterial(truncatedMips);
    assert(!truncatedMipResult.ok && truncatedMipResult.code == "invalid_dds");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    const auto malformedDx10Source =
        root / "garrysmod" / "data" / "missing_dx10_header.dds";
    WriteMalformedDx10Dds(malformedDx10Source);
    ApplyRequest malformedDx10 = request;
    malformedDx10.profileId = "missing_dx10_header";
    malformedDx10.material.textures.front().source = malformedDx10Source;
    const auto malformedDx10Result = bridge.ApplyLegacyMaterial(malformedDx10);
    assert(!malformedDx10Result.ok && malformedDx10Result.code == "invalid_dds");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    ApplyRequest partialTextureFailure = request;
    partialTextureFailure.profileId = "partial_texture_failure";
    partialTextureFailure.material.textures.clear();
    partialTextureFailure.material.textures.push_back(
        {TextureRole::Anisotropy, source, {}});
    partialTextureFailure.material.textures.push_back(
        {TextureRole::Normal, oversizedDimensionSource, {}});
    const auto partialTextureResult = bridge.ApplyLegacyMaterial(partialTextureFailure);
    assert(!partialTextureResult.ok && partialTextureResult.code == "invalid_dds");
    assert(!std::filesystem::exists(
        expectedMod / "textures" / "partial_texture_failure"));
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    const auto protectedSource =
        root / "rtx-remix" / "mods" / "~gmod_topbr" / "protected.dds";
    WriteMinimalDds(protectedSource);
    ApplyRequest protectedInput = request;
    protectedInput.profileId = "protected_input";
    protectedInput.material.textures.front().source = protectedSource;
    const auto protectedResult = bridge.ApplyLegacyMaterial(protectedInput);
    assert(!protectedResult.ok && protectedResult.code == "protected_texture_path");
    assert(ReadText(expectedMod / "mod.usda") == committedRootText);

    assert(!bridge.ClearLegacyMaterial({request.profileId, {}}).ok);
    const auto cleared = bridge.ClearLegacyMaterial(
        {request.profileId, {"0x0123456789ABCDEF"}});
    assert(cleared.ok);
    // The just-disabled revision remains as the single previous generation.
    // Only the atomically published root decides which revisions are active.
    assert(std::filesystem::is_regular_file(applied.layerPaths[0]));
    assert(std::filesystem::is_regular_file(applied.layerPaths[1]));
    assert(std::filesystem::is_regular_file(cleared.layerPath));
    const auto clearedModText = ReadText(expectedMod / "mod.usda");
    assert(!RootReferences(clearedModText, applied.layerPaths[0]));
    assert(RootReferences(clearedModText, applied.layerPaths[1]));
    // An exact-hash clear is deliberately idempotent and cannot touch the
    // surviving hash layer for the same Source material.
    std::filesystem::last_write_time(cleared.layerPath, oldTime);
    std::filesystem::last_write_time(applied.layerPaths[1], oldTime);
    std::filesystem::last_write_time(expectedMod / "mod.usda", oldTime);
    const auto survivingTime = std::filesystem::last_write_time(applied.layerPaths[1]);
    const auto oldClearedModTime = std::filesystem::last_write_time(expectedMod / "mod.usda");
    assert(bridge.ClearLegacyMaterial(
        {request.profileId, {"0x0123456789ABCDEF"}}).ok);
    assert(!std::filesystem::exists(cleared.layerPath));
    assert(std::filesystem::last_write_time(applied.layerPaths[1]) == survivingTime);
    assert(std::filesystem::last_write_time(expectedMod / "mod.usda") == oldClearedModTime);

    assert(bridge.ClearLegacyMaterial(
        {request.profileId, {"0x0123456789ABCDF0"}}).ok);
    assert(std::filesystem::is_regular_file(applied.layerPaths[1]));
    const auto fullyClearedModText = ReadText(expectedMod / "mod.usda");
    assert(fullyClearedModText.find("hash_0123456789ABCDEF") == std::string::npos);
    assert(fullyClearedModText.find("hash_0123456789ABCDF0") == std::string::npos);

    // Migrate an aggregate material_<id> layer from the pre-hash layout, then
    // clear only one hash. The other raw shader block must survive in its own
    // layer, and retrying the retired hash must remain harmless.
    const std::string legacyHash1 = "000000000000AAA1";
    const std::string legacyHash2 = "000000000000AAA2";
    const std::string legacyProfile = "material_legacy_migration";
    const auto legacyPath = expectedMod / "profiles" / (legacyProfile + ".usda");
    const auto legacyContents = LegacyLayer({legacyHash1, legacyHash2});
    WriteText(legacyPath, legacyContents);
    WriteText(expectedMod / "mod.usda", ModLayer({legacyProfile}));
    const auto migratedClear = bridge.ClearLegacyMaterial(
        {legacyProfile, {legacyHash1}});
    assert(migratedClear.ok);
    const auto migratedHash1Layers = FindVersionedHashLayers(
        expectedMod / "profiles", legacyHash1);
    const auto migratedHash2Layers = FindVersionedHashLayers(
        expectedMod / "profiles", legacyHash2);
    assert(migratedHash1Layers.size() == 1);
    assert(migratedHash2Layers.size() == 1);
    const auto& migratedHash1 = migratedHash1Layers.front();
    const auto& migratedHash2 = migratedHash2Layers.front();
    assert(std::filesystem::is_regular_file(migratedHash1));
    assert(std::filesystem::is_regular_file(migratedHash2));
    assert(std::filesystem::is_regular_file(legacyPath));
    assert(ReadText(legacyPath) == legacyContents);
    const auto migratedModText = ReadText(expectedMod / "mod.usda");
    assert(!RootReferences(migratedModText, legacyPath));
    assert(!RootReferences(migratedModText, migratedHash1));
    assert(RootReferences(migratedModText, migratedHash2));
    const auto migratedHash2Text = ReadText(migratedHash2);
    assert(migratedHash2Text.find("mat_" + legacyHash2) != std::string::npos);
    const auto migratedRootBeforeRetry = ReadText(expectedMod / "mod.usda");
    assert(bridge.ClearLegacyMaterial({legacyProfile, {legacyHash1}}).ok);
    assert(!std::filesystem::exists(legacyPath));
    assert(!std::filesystem::exists(migratedHash1));
    assert(std::filesystem::is_regular_file(migratedHash2));
    assert(ReadText(expectedMod / "mod.usda") == migratedRootBeforeRetry);
    assert(bridge.ClearLegacyMaterial({legacyProfile, {legacyHash2}}).ok);
    assert(std::filesystem::is_regular_file(migratedHash2));
    assert(ReadText(expectedMod / "mod.usda").find("hash_") == std::string::npos);

    // Every edit begins by pruning files not referenced by the current root,
    // then leaves at most the just-superseded generation for watcher safety.
    // Repeated successful edits therefore stay bounded within one session.
    const std::string churnHash = "00000000C0FFEE01";
    ApplyRequest churnRequest = request;
    churnRequest.profileId = "bounded_churn";
    churnRequest.hashes = {churnHash};
    for (std::uint32_t revision = 1; revision <= 12; ++revision) {
        const auto churnSource = root / "garrysmod" / "data" /
            ("churn_" + std::to_string(revision) + ".dds");
        WriteMinimalDds(churnSource, revision, 1);
        churnRequest.material.roughness = revision / 16.0f;
        churnRequest.material.textures.clear();
        churnRequest.material.textures.push_back(
            {TextureRole::Albedo, churnSource, {}});
        const auto churnApply = bridge.ApplyLegacyMaterial(churnRequest);
        assert(churnApply.ok && churnApply.layerPaths.size() == 1);
    }
    const auto churnLayers = FindVersionedHashLayers(
        expectedMod / "profiles", churnHash);
    assert(churnLayers.size() == 2);
    assert(CountRegularFiles(expectedMod / "textures" / "bounded_churn") == 2);
    assert(bridge.ClearLegacyMaterial({"bounded_churn", {churnHash}}).ok);

    // Populate a larger active generation. ClearAllOwned must deactivate the
    // entire set with one root publication, then retire the profile and texture
    // generations with fixed directory renames independent of layer count.
    ApplyRequest scaleRequest = request;
    scaleRequest.profileId = "material_authoritative_reset_scale";
    scaleRequest.hashes.clear();
    for (std::uint64_t index = 0; index < 64; ++index) {
        scaleRequest.hashes.push_back(HexHash(0x1000u + index));
    }
    const auto resetApply = bridge.ApplyLegacyMaterial(scaleRequest);
    assert(resetApply.ok && resetApply.layerPaths.size() == 64);
    const std::string resetLegacyProfile = "material_authoritative_reset";
    const auto resetLegacyPath = expectedMod / "profiles" /
        (resetLegacyProfile + ".usda");
    WriteText(resetLegacyPath, LegacyLayer({legacyHash1}));
    std::vector<std::string> resetActiveIds;
    resetActiveIds.reserve(resetApply.layerPaths.size() + 1);
    for (const auto& path : resetApply.layerPaths) {
        resetActiveIds.push_back(path.stem().string());
    }
    resetActiveIds.push_back(resetLegacyProfile);
    WriteText(expectedMod / "mod.usda", ModLayer(resetActiveIds));
    const auto retainedTexture = resetApply.importedTextures.front();
    assert(std::filesystem::is_regular_file(retainedTexture));

    // A failure before the empty-root commit cannot change activation.
    const auto activeRootBeforeReset = ReadText(expectedMod / "mod.usda");
    Config resetBeforeConfig;
    resetBeforeConfig.gameRoot = root;
    resetBeforeConfig.failureInjector = [](std::string_view point) {
        return point == "reset_before_root_publish";
    };
    Bridge resetBeforeBridge(std::move(resetBeforeConfig));
    const auto resetBeforeFailure = resetBeforeBridge.ClearAllOwned();
    assert(!resetBeforeFailure.ok && resetBeforeFailure.code == "injected_failure");
    assert(ReadText(expectedMod / "mod.usda") == activeRootBeforeReset);
    assert(std::filesystem::is_directory(expectedMod / "profiles"));

    // A simulated crash immediately after the empty-root commit leaves every
    // old layer on disk but none active. A retry may then retire the directory
    // without risking reactivation.
    Config resetAfterConfig;
    resetAfterConfig.gameRoot = root;
    resetAfterConfig.failureInjector = [](std::string_view point) {
        return point == "reset_after_root_publish";
    };
    Bridge resetAfterBridge(std::move(resetAfterConfig));
    const auto resetAfterFailure = resetAfterBridge.ClearAllOwned();
    assert(!resetAfterFailure.ok &&
        resetAfterFailure.code == "injected_failure_after_commit");
    const auto emptyRootText = ReadText(expectedMod / "mod.usda");
    assert(emptyRootText == ModLayer({}));
    assert(std::filesystem::is_directory(expectedMod / "profiles"));
    for (const auto& path : resetApply.layerPaths) {
        assert(std::filesystem::is_regular_file(path));
        assert(!RootReferences(emptyRootText, path));
    }

    // A failure between the two retirement renames is already past the empty
    // root commit. Profiles may be quarantined while textures remain in their
    // active cache path; retry must still rotate textures even though profiles/
    // no longer exists.
    Config resetBetweenConfig;
    resetBetweenConfig.gameRoot = root;
    resetBetweenConfig.failureInjector = [](std::string_view point) {
        return point == "reset_after_profile_retire";
    };
    Bridge resetBetweenBridge(std::move(resetBetweenConfig));
    const auto resetBetweenFailure = resetBetweenBridge.ClearAllOwned();
    assert(!resetBetweenFailure.ok &&
        resetBetweenFailure.code == "injected_failure_after_commit");
    const auto retiredProfiles = expectedMod / "profiles.retired";
    const auto retiredTextures = expectedMod / "textures.retired";
    assert(std::filesystem::is_directory(retiredProfiles));
    assert(!std::filesystem::exists(expectedMod / "profiles"));
    assert(std::filesystem::is_directory(expectedMod / "textures"));
    assert(!std::filesystem::exists(retiredTextures));
    for (const auto& path : resetApply.layerPaths) {
        assert(std::filesystem::is_regular_file(retiredProfiles / path.filename()));
    }
    assert(std::filesystem::is_regular_file(retiredProfiles / resetLegacyPath.filename()));
    assert(std::filesystem::is_regular_file(retainedTexture));

    const auto reset = bridge.ClearAllOwned();
    assert(reset.ok && reset.layerPaths.empty());
    const auto retainedTextureRelative = retainedTexture.lexically_relative(
        expectedMod / "textures");
    assert(!retainedTextureRelative.empty());
    assert(!std::filesystem::exists(expectedMod / "textures"));
    assert(std::filesystem::is_regular_file(
        retiredTextures / retainedTextureRelative));
    const auto resetModText = ReadText(expectedMod / "mod.usda");
    assert(resetModText == ModLayer({}));

    // Authoritative reset is idempotent and avoids needless watcher reloads.
    std::filesystem::last_write_time(expectedMod / "mod.usda", oldTime);
    const auto resetModTime = std::filesystem::last_write_time(expectedMod / "mod.usda");
    const auto repeatedReset = bridge.ClearAllOwned();
    assert(repeatedReset.ok && repeatedReset.layerPaths.empty());
    assert(std::filesystem::last_write_time(expectedMod / "mod.usda") == resetModTime);
    assert(std::filesystem::is_directory(retiredProfiles));
    assert(std::filesystem::is_directory(retiredTextures));
    assert(std::filesystem::is_regular_file(
        retiredTextures / retainedTextureRelative));

    // A later generation creates a fresh profiles/ directory. The retired
    // generation remains outside the sole root ledger and cannot reactivate.
    ApplyRequest postResetRequest = request;
    postResetRequest.hashes = {"00000000FEEDFACE"};
    const auto postResetApply = bridge.ApplyLegacyMaterial(postResetRequest);
    assert(postResetApply.ok && postResetApply.layerPaths.size() == 1);
    assert(postResetApply.importedTextures.size() == 1);
    const auto postResetTexture = postResetApply.importedTextures.front();
    assert(std::filesystem::is_regular_file(postResetTexture));
    const auto postResetRoot = ReadText(expectedMod / "mod.usda");
    assert(RootReferences(postResetRoot, postResetApply.layerPaths.front()));
    assert(postResetRoot.find(retiredProfiles.filename().string()) == std::string::npos);
    assert(std::filesystem::is_directory(retiredProfiles));

    // A second populated reset replaces both fixed quarantines instead of
    // creating per-session directories. A sentinel in the older texture cache
    // proves that replacement happened rather than another generation being
    // accumulated beside it.
    WriteText(retiredTextures / "old_generation.marker", "old texture generation");
    const auto rotatedReset = bridge.ClearAllOwned();
    assert(rotatedReset.ok && rotatedReset.layerPaths.size() == 1);
    assert(rotatedReset.layerPaths.front() == retiredProfiles);
    assert(std::filesystem::is_regular_file(
        retiredProfiles / postResetApply.layerPaths.front().filename()));
    assert(!std::filesystem::exists(
        retiredProfiles / resetApply.layerPaths.front().filename()));
    assert(!std::filesystem::exists(retiredTextures / "old_generation.marker"));
    const auto postResetTextureRelative = postResetTexture.lexically_relative(
        expectedMod / "textures");
    assert(!postResetTextureRelative.empty());
    assert(!std::filesystem::exists(expectedMod / "textures"));
    assert(std::filesystem::is_regular_file(
        retiredTextures / postResetTextureRelative));
    std::size_t profileRetirementDirectories = 0;
    std::size_t textureRetirementDirectories = 0;
    for (const auto& entry : std::filesystem::directory_iterator(expectedMod)) {
        const auto name = entry.path().filename().string();
        if (name == "profiles.retired" || name.rfind("profiles.tmp.", 0) == 0) {
            ++profileRetirementDirectories;
        }
        if (name == "textures.retired" || name.rfind("textures.tmp.", 0) == 0) {
            ++textureRetirementDirectories;
        }
    }
    assert(profileRetirementDirectories == 1);
    assert(textureRetirementDirectories == 1);

    // Refuse planted quarantine symlinks/junctions rather than allowing
    // recursive cleanup to cross the fixed mod boundary. Cover both a nested
    // entry and the exact quarantine root. Symlink creation may require
    // Developer Mode on Windows, so each assertion is conditional on support.
    const auto reparseApply = bridge.ApplyLegacyMaterial(postResetRequest);
    assert(reparseApply.ok && reparseApply.importedTextures.size() == 1);
    const auto externalTextureDirectory = root / "external_texture_sentinel";
    const auto externalTextureSentinel = externalTextureDirectory / "keep.txt";
    WriteText(externalTextureSentinel, "outside texture sentinel");
    std::error_code reparseError;
    const auto nestedReparse = retiredTextures / "planted_reparse";
    std::filesystem::create_directory_symlink(
        externalTextureDirectory, nestedReparse, reparseError);
    if (!reparseError) {
        const auto unsafeNestedRetirement = bridge.ClearAllOwned();
        assert(!unsafeNestedRetirement.ok &&
            unsafeNestedRetirement.code == "authoritative_reset_retire_failed_after_commit");
        assert(ReadText(expectedMod / "mod.usda") == ModLayer({}));
        assert(ReadText(externalTextureSentinel) == "outside texture sentinel");
        assert(std::filesystem::is_directory(expectedMod / "textures"));
        std::filesystem::remove(nestedReparse, reparseError);
        assert(!reparseError);
        const auto recoveredNestedRetirement = bridge.ClearAllOwned();
        assert(recoveredNestedRetirement.ok);
        assert(ReadText(externalTextureSentinel) == "outside texture sentinel");
    } else {
        reparseError.clear();
        const auto ordinaryNestedRetirement = bridge.ClearAllOwned();
        assert(ordinaryNestedRetirement.ok);
        assert(ReadText(externalTextureSentinel) == "outside texture sentinel");
    }

    const auto rootReparseApply = bridge.ApplyLegacyMaterial(postResetRequest);
    assert(rootReparseApply.ok && rootReparseApply.importedTextures.size() == 1);
    reparseError.clear();
    std::filesystem::remove_all(retiredTextures, reparseError);
    assert(!reparseError);
    reparseError.clear();
    std::filesystem::create_directory_symlink(
        externalTextureDirectory, retiredTextures, reparseError);
    if (!reparseError) {
        const auto unsafeRetirement = bridge.ClearAllOwned();
        assert(!unsafeRetirement.ok &&
            unsafeRetirement.code == "authoritative_reset_retire_failed_after_commit");
        assert(ReadText(expectedMod / "mod.usda") == ModLayer({}));
        assert(ReadText(externalTextureSentinel) == "outside texture sentinel");
        assert(std::filesystem::is_directory(expectedMod / "textures"));
        std::filesystem::remove(retiredTextures, reparseError);
        assert(!reparseError);
        const auto recoveredRetirement = bridge.ClearAllOwned();
        assert(recoveredRetirement.ok);
        assert(ReadText(externalTextureSentinel) == "outside texture sentinel");
        assert(!std::filesystem::exists(expectedMod / "textures"));
        assert(std::filesystem::is_directory(retiredTextures));
    } else {
        reparseError.clear();
        const auto ordinaryRetirement = bridge.ClearAllOwned();
        assert(ordinaryRetirement.ok);
        assert(ReadText(externalTextureSentinel) == "outside texture sentinel");
    }

    ApplyRequest unsafe = request;
    unsafe.profileId = "../outside";
    assert(!bridge.ApplyLegacyMaterial(unsafe).ok);
    unsafe = request;
    unsafe.hashes = {"not-a-hash"};
    assert(!bridge.ApplyLegacyMaterial(unsafe).ok);

    // The active ledger parser never accepts paths outside the fixed profiles
    // namespace. Authoritative reset can still recover from a malformed root,
    // and its retirement cannot touch the referenced outside file.
    const auto outsideSentinel = expectedMod.parent_path() / "outside.usda";
    WriteText(outsideSentinel, "outside sentinel");
    WriteText(expectedMod / "mod.usda",
        "#usda 1.0\n(\n    subLayers = [\n"
        "        @../outside.usda@\n    ]\n)\n");
    const auto unsafeRootApply = bridge.ApplyLegacyMaterial(postResetRequest);
    assert(!unsafeRootApply.ok && unsafeRootApply.code == "mod_index_read_failed");
    assert(ReadText(outsideSentinel) == "outside sentinel");
    const auto recoveredReset = bridge.ClearAllOwned();
    assert(recoveredReset.ok);
    assert(ReadText(expectedMod / "mod.usda") == ModLayer({}));
    assert(ReadText(outsideSentinel) == "outside sentinel");

    // AperturePBR exposes an anisotropy map. It has no tangent_texture input,
    // so the native role, content-addressed name, and USDA token must agree.
    assert(std::string(TextureRoleName(TextureRole::Anisotropy)) == "anisotropy");
    ApplyRequest anisotropyRequest = request;
    anisotropyRequest.profileId = "anisotropy_contract";
    anisotropyRequest.hashes = {"00000000A11507F1"};
    anisotropyRequest.material.textures.clear();
    anisotropyRequest.material.textures.push_back(
        {TextureRole::Anisotropy, source, {}});
    const auto anisotropyApply = bridge.ApplyLegacyMaterial(anisotropyRequest);
    assert(anisotropyApply.ok && anisotropyApply.layerPaths.size() == 1);
    assert(anisotropyApply.importedTextures.size() == 1);
    assert(anisotropyApply.importedTextures.front().filename().string().rfind(
        "anisotropy_", 0) == 0);
    const auto anisotropyLayer = ReadText(anisotropyApply.layerPaths.front());
    assert(anisotropyLayer.find("asset inputs:anisotropy_texture") != std::string::npos);
    assert(anisotropyLayer.find("tangent_texture") == std::string::npos);

    // RTX Remix 1.5.2's AperturePBR_Opacity contract. Exercise every texture
    // role and subsurface constant together so the unique-role bound, USDA
    // input names, and MDL-declared colour spaces cannot drift independently.
    assert(kTextureRoleCount == 11);
    assert(std::string(TextureRoleName(TextureRole::SubsurfaceTransmittance)) ==
           "subsurface_transmittance");
    assert(std::string(TextureRoleName(TextureRole::SubsurfaceThickness)) ==
           "subsurface_thickness");
    assert(std::string(TextureRoleName(TextureRole::SubsurfaceScattering)) ==
           "subsurface_scattering");
    assert(std::string(TextureRoleName(TextureRole::SubsurfaceRadius)) ==
           "subsurface_radius");

    ApplyRequest aperture152Request = request;
    aperture152Request.profileId = "aperture_152_contract";
    aperture152Request.hashes = {"0000000015200001"};
    aperture152Request.material.textures.clear();
    for (std::size_t role = 0; role < kTextureRoleCount; ++role) {
        aperture152Request.material.textures.push_back(
            {static_cast<TextureRole>(role), source, {}});
    }
    aperture152Request.material.subsurfaceTransmittanceColor =
        Vec3{0.1f, 0.2f, 0.3f};
    aperture152Request.material.subsurfaceMeasurementDistance = 16.0f;
    aperture152Request.material.subsurfaceSingleScatteringAlbedo =
        Vec3{0.4f, 0.5f, 0.6f};
    aperture152Request.material.subsurfaceVolumetricAnisotropy = -0.99f;
    aperture152Request.material.subsurfaceRadius = Vec3{0.7f, 0.8f, 0.9f};
    aperture152Request.material.subsurfaceRadiusScale = 1000.0f;
    aperture152Request.material.subsurfaceMaxSampleRadius = 65504.0f;
    aperture152Request.material.normalEncoding = 1;
    // Explicit false must win over every legacy inference trigger below.
    aperture152Request.material.enableThinFilm = false;
    aperture152Request.material.thinFilmThickness = 200.0f;
    aperture152Request.material.alphaIsThinFilmThickness = true;
    aperture152Request.material.enableEmission = false;
    aperture152Request.material.emissiveIntensity = 40.0f;
    aperture152Request.material.preloadTextures = false;
    aperture152Request.material.ignoreMaterial = true;
    aperture152Request.material.blendEnabled = false;
    aperture152Request.material.blendType = 8;
    aperture152Request.material.subsurfaceDiffusionProfile = true;
    aperture152Request.material.wrapModeU = 3;
    aperture152Request.material.alphaTestType = 6;

    const auto aperture152Apply = bridge.ApplyLegacyMaterial(aperture152Request);
    assert(aperture152Apply.ok && aperture152Apply.layerPaths.size() == 1);
    assert(aperture152Apply.importedTextures.size() == kTextureRoleCount);
    const auto aperture152Layer = ReadText(aperture152Apply.layerPaths.front());
    assert(HasTextureInput(aperture152Layer, "diffuse_texture", "sRGB"));
    assert(HasTextureInput(aperture152Layer, "normalmap_texture", "raw"));
    assert(HasTextureInput(aperture152Layer, "anisotropy_texture", "raw"));
    assert(HasTextureInput(aperture152Layer, "reflectionroughness_texture", "raw"));
    assert(HasTextureInput(aperture152Layer, "metallic_texture", "raw"));
    assert(HasTextureInput(aperture152Layer, "height_texture", "raw"));
    assert(HasTextureInput(aperture152Layer, "emissive_mask_texture", "sRGB"));
    assert(HasTextureInput(aperture152Layer,
                           "subsurface_transmittance_texture", "sRGB"));
    assert(HasTextureInput(aperture152Layer,
                           "subsurface_thickness_texture", "raw"));
    assert(HasTextureInput(aperture152Layer,
                           "subsurface_single_scattering_texture", "sRGB"));
    assert(HasTextureInput(aperture152Layer,
                           "subsurface_radius_texture", "raw"));
    assert(aperture152Layer.find(
        "color3f inputs:subsurface_transmittance_color = (0.100000001, 0.200000003, 0.300000012)") !=
        std::string::npos);
    assert(aperture152Layer.find(
        "float inputs:subsurface_measurement_distance = 16") != std::string::npos);
    assert(aperture152Layer.find(
        "color3f inputs:subsurface_single_scattering_albedo = (0.400000006, 0.5, 0.600000024)") !=
        std::string::npos);
    assert(aperture152Layer.find(
        "float inputs:subsurface_volumetric_anisotropy = -0.99000001") !=
        std::string::npos);
    assert(aperture152Layer.find(
        "color3f inputs:subsurface_radius = (0.699999988, 0.800000012, 0.899999976)") !=
        std::string::npos);
    assert(aperture152Layer.find(
        "float inputs:subsurface_radius_scale = 1000") != std::string::npos);
    assert(aperture152Layer.find(
        "float inputs:subsurface_max_sample_radius = 65504") != std::string::npos);
    assert(aperture152Layer.find("int inputs:encoding = 1") != std::string::npos);
    assert(aperture152Layer.find("bool inputs:enable_thin_film = 0") !=
        std::string::npos);
    assert(aperture152Layer.find("bool inputs:enable_thin_film = 1") ==
        std::string::npos);
    assert(aperture152Layer.find("bool inputs:enable_emission = 0") !=
        std::string::npos);
    assert(aperture152Layer.find("bool inputs:enable_emission = 1") ==
        std::string::npos);
    assert(aperture152Layer.find("bool inputs:preload_textures = 0") !=
        std::string::npos);
    assert(aperture152Layer.find("bool inputs:preload_textures = 1") ==
        std::string::npos);
    assert(aperture152Layer.find("bool inputs:ignore_material = 1") !=
        std::string::npos);
    assert(aperture152Layer.find("bool inputs:blend_enabled = 0") !=
        std::string::npos);
    assert(aperture152Layer.find("bool inputs:blend_enabled = 1") ==
        std::string::npos);
    assert(aperture152Layer.find("bool inputs:subsurface_diffusion_profile = 1") !=
        std::string::npos);
    assert(aperture152Layer.find("int inputs:wrap_mode_u = 3") != std::string::npos);
    assert(aperture152Layer.find("int inputs:alpha_test_type = 6") !=
        std::string::npos);

    // Every AlphaTestType and normal-map encoding value declared by the MDLs
    // is accepted, while values immediately outside each enum remain invalid.
    ApplyRequest enumRequest = aperture152Request;
    enumRequest.profileId = "aperture_152_enums";
    enumRequest.hashes = {"0000000015200002"};
    enumRequest.material.textures.clear();
    for (int alphaTest = 0; alphaTest <= 7; ++alphaTest) {
        enumRequest.material.alphaTestType = alphaTest;
        const auto enumApply = bridge.ApplyLegacyMaterial(enumRequest);
        assert(enumApply.ok);
    }
    for (int encoding = 0; encoding <= 2; ++encoding) {
        enumRequest.material.normalEncoding = encoding;
        const auto enumApply = bridge.ApplyLegacyMaterial(enumRequest);
        assert(enumApply.ok);
    }
    for (const int invalidEncoding : {-1, 3}) {
        enumRequest.material.normalEncoding = invalidEncoding;
        const auto enumApply = bridge.ApplyLegacyMaterial(enumRequest);
        assert(!enumApply.ok && enumApply.code == "invalid_material");
    }

    // Check every new hard range just beyond the 1.5.2 MDL boundary.
    auto invalid152 = aperture152Request;
    invalid152.material.subsurfaceTransmittanceColor = Vec3{1.01f, 0.0f, 0.0f};
    assert(!bridge.ApplyLegacyMaterial(invalid152).ok);
    invalid152 = aperture152Request;
    invalid152.material.subsurfaceMeasurementDistance = 16.01f;
    assert(!bridge.ApplyLegacyMaterial(invalid152).ok);
    invalid152 = aperture152Request;
    invalid152.material.subsurfaceSingleScatteringAlbedo = Vec3{0.0f, -0.01f, 0.0f};
    assert(!bridge.ApplyLegacyMaterial(invalid152).ok);
    invalid152 = aperture152Request;
    invalid152.material.subsurfaceVolumetricAnisotropy = 1.0f;
    assert(!bridge.ApplyLegacyMaterial(invalid152).ok);
    invalid152 = aperture152Request;
    invalid152.material.subsurfaceRadius = Vec3{0.0f, 0.0f, 1.01f};
    assert(!bridge.ApplyLegacyMaterial(invalid152).ok);
    invalid152 = aperture152Request;
    invalid152.material.subsurfaceRadiusScale = 1000.01f;
    assert(!bridge.ApplyLegacyMaterial(invalid152).ok);
    invalid152 = aperture152Request;
    invalid152.material.subsurfaceMaxSampleRadius = 65504.01f;
    assert(!bridge.ApplyLegacyMaterial(invalid152).ok);

    // Protocol-1 requests without explicit flags retain their prior inferred
    // writer behaviour. New requests can override every one of these above.
    ApplyRequest legacyInference = request;
    legacyInference.profileId = "legacy_flag_inference";
    legacyInference.hashes = {"0000000015200003"};
    legacyInference.material.textures.clear();
    legacyInference.material.textures.push_back({TextureRole::Normal, source, {}});
    legacyInference.material.emissiveIntensity = 1.0f;
    legacyInference.material.thinFilmThickness = 100.0f;
    const auto legacyInferenceApply = bridge.ApplyLegacyMaterial(legacyInference);
    assert(legacyInferenceApply.ok);
    const auto legacyInferenceLayer = ReadText(legacyInferenceApply.layerPaths.front());
    assert(legacyInferenceLayer.find("bool inputs:preload_textures = 1") !=
        std::string::npos);
    assert(legacyInferenceLayer.find("int inputs:encoding = 2") != std::string::npos);
    assert(legacyInferenceLayer.find("bool inputs:enable_emission = 1") !=
        std::string::npos);
    assert(legacyInferenceLayer.find("bool inputs:enable_thin_film = 1") !=
        std::string::npos);
    assert(legacyInferenceLayer.find("bool inputs:blend_enabled = 1") !=
        std::string::npos);

#ifdef _WIN32
    const auto normalMipSource = root / "garrysmod" / "data" / "normal_mips.bmp";
    WriteNormalMipFixture(normalMipSource);
    ApplyRequest normalMipRequest = request;
    normalMipRequest.profileId = "normal_mip_contract";
    normalMipRequest.hashes = {"00000000A11CE001"};
    normalMipRequest.material.textures.clear();
    normalMipRequest.material.textures.push_back(
        {TextureRole::Normal, normalMipSource, {}});
    const auto normalMipApply = bridge.ApplyLegacyMaterial(normalMipRequest);
    assert(normalMipApply.ok && normalMipApply.importedTextures.size() == 1);
    std::ifstream normalDds(normalMipApply.importedTextures.front(), std::ios::binary);
    normalDds.seekg(128 + 2 * 2 * 4);
    std::uint8_t normalMip[4]{};
    normalDds.read(reinterpret_cast<char*>(normalMip),
                   static_cast<std::streamsize>(sizeof(normalMip)));
    assert(normalDds);
    const float mipX = normalMip[2] / 127.5f - 1.0f;
    const float mipY = normalMip[1] / 127.5f - 1.0f;
    const float mipZ = normalMip[0] / 127.5f - 1.0f;
    const float mipLength = std::sqrt(mipX * mipX + mipY * mipY + mipZ * mipZ);
    assert(mipLength > 0.98f && mipLength < 1.02f);
    assert(normalMip[0] > 210 && normalMip[2] > 210);
#endif

    // The root ledger and active cache are bounded transactionally. Quota
    // rejection must never replace the previously published root.
    const auto profileQuotaRoot = root / "profile_quota";
    std::filesystem::create_directories(profileQuotaRoot / "garrysmod" / "data");
    std::filesystem::create_directories(profileQuotaRoot / "rtx-remix" / "mods");
    const auto profileQuotaSource =
        profileQuotaRoot / "garrysmod" / "data" / "source.dds";
    WriteMinimalDds(profileQuotaSource);
    Config profileQuotaConfig;
    profileQuotaConfig.gameRoot = profileQuotaRoot;
    profileQuotaConfig.maxActiveProfiles = 128;
    profileQuotaConfig.maxActiveTextureFiles = 1;
    profileQuotaConfig.maxActiveTextureBytes =
        std::filesystem::file_size(profileQuotaSource);
    Bridge profileQuotaBridge(std::move(profileQuotaConfig));
    ApplyRequest profileQuotaRequest;
    profileQuotaRequest.profileId = "profile_quota";
    profileQuotaRequest.material.roughness = 0.2f;
    profileQuotaRequest.material.textures = {
        {TextureRole::Albedo, profileQuotaSource, {}},
    };
    for (std::uint64_t index = 1; index <= 128; ++index) {
        profileQuotaRequest.hashes.push_back(HexHash(0xA000000000000000ull + index));
    }
    assert(profileQuotaBridge.ApplyLegacyMaterial(profileQuotaRequest).ok);
    profileQuotaRequest.material.roughness = 0.7f;
    assert(profileQuotaBridge.ApplyLegacyMaterial(profileQuotaRequest).ok);
    // A repeat prunes the inactive revisions created by the replacement while
    // reusing the current immutable layers.
    assert(profileQuotaBridge.ApplyLegacyMaterial(profileQuotaRequest).ok);
    const auto profileQuotaMod = profileQuotaBridge.ModDirectory() / "mod.usda";
    const auto profileQuotaRootBefore = ReadText(profileQuotaMod);
    const auto profileFilesBefore = CountRegularFiles(
        profileQuotaBridge.ModDirectory() / "profiles");
    assert(profileFilesBefore == 128);
    ApplyRequest profileOverflow = profileQuotaRequest;
    profileOverflow.profileId = "profile_quota_overflow";
    profileOverflow.hashes = {"B000000000000001"};
    const auto profileOverflowResult =
        profileQuotaBridge.ApplyLegacyMaterial(profileOverflow);
    assert(!profileOverflowResult.ok && profileOverflowResult.code == "resource_limit");
    assert(ReadText(profileQuotaMod) == profileQuotaRootBefore);
    assert(CountRegularFiles(profileQuotaBridge.ModDirectory() / "profiles") ==
           profileFilesBefore);
    assert(CountRegularFilesRecursive(profileQuotaBridge.ModDirectory() / "textures") == 1);

    const auto fileQuotaRoot = root / "file_quota";
    std::filesystem::create_directories(fileQuotaRoot / "garrysmod" / "data");
    std::filesystem::create_directories(fileQuotaRoot / "rtx-remix" / "mods");
    const auto fileQuotaSource = fileQuotaRoot / "garrysmod" / "data" / "source.dds";
    WriteMinimalDds(fileQuotaSource);
    Config fileQuotaConfig;
    fileQuotaConfig.gameRoot = fileQuotaRoot;
    fileQuotaConfig.maxActiveTextureFiles = 1;
    Bridge fileQuotaBridge(std::move(fileQuotaConfig));
    ApplyRequest fileQuotaBase;
    fileQuotaBase.profileId = "file_quota";
    fileQuotaBase.hashes = {"C000000000000001"};
    fileQuotaBase.material.roughness = 0.4f;
    assert(fileQuotaBridge.ApplyLegacyMaterial(fileQuotaBase).ok);
    const auto fileQuotaMod = fileQuotaBridge.ModDirectory() / "mod.usda";
    const auto fileQuotaRootBefore = ReadText(fileQuotaMod);
    ApplyRequest tooManyTextureFiles = fileQuotaBase;
    tooManyTextureFiles.material.textures = {
        {TextureRole::Albedo, fileQuotaSource, {}},
        {TextureRole::Normal, fileQuotaSource, {}},
    };
    const auto fileQuotaFailure =
        fileQuotaBridge.ApplyLegacyMaterial(tooManyTextureFiles);
    assert(!fileQuotaFailure.ok && fileQuotaFailure.code == "resource_limit");
    assert(ReadText(fileQuotaMod) == fileQuotaRootBefore);
    assert(CountRegularFiles(fileQuotaBridge.ModDirectory() / "profiles") == 1);
    assert(CountRegularFilesRecursive(fileQuotaBridge.ModDirectory() / "textures") == 0);
    tooManyTextureFiles.material.textures.resize(1);
    assert(fileQuotaBridge.ApplyLegacyMaterial(tooManyTextureFiles).ok);
    assert(CountRegularFilesRecursive(fileQuotaBridge.ModDirectory() / "textures") == 1);

    const auto byteQuotaRoot = root / "byte_quota";
    std::filesystem::create_directories(byteQuotaRoot / "garrysmod" / "data");
    std::filesystem::create_directories(byteQuotaRoot / "rtx-remix" / "mods");
    const auto byteQuotaSource = byteQuotaRoot / "garrysmod" / "data" / "source.dds";
    WriteMinimalDds(byteQuotaSource);
    const auto oneTextureBytes = std::filesystem::file_size(byteQuotaSource);
    Config byteQuotaConfig;
    byteQuotaConfig.gameRoot = byteQuotaRoot;
    byteQuotaConfig.maxActiveTextureBytes = oneTextureBytes;
    Bridge byteQuotaBridge(std::move(byteQuotaConfig));
    ApplyRequest byteQuotaFirst;
    byteQuotaFirst.profileId = "byte_quota_a";
    byteQuotaFirst.hashes = {"D000000000000001"};
    byteQuotaFirst.material.textures = {
        {TextureRole::Albedo, byteQuotaSource, {}},
    };
    assert(byteQuotaBridge.ApplyLegacyMaterial(byteQuotaFirst).ok);
    const auto byteQuotaMod = byteQuotaBridge.ModDirectory() / "mod.usda";
    const auto byteQuotaRootBefore = ReadText(byteQuotaMod);
    ApplyRequest byteQuotaSecond = byteQuotaFirst;
    byteQuotaSecond.profileId = "byte_quota_b";
    byteQuotaSecond.hashes = {"D000000000000002"};
    const auto byteQuotaFailure = byteQuotaBridge.ApplyLegacyMaterial(byteQuotaSecond);
    assert(!byteQuotaFailure.ok && byteQuotaFailure.code == "resource_limit");
    assert(ReadText(byteQuotaMod) == byteQuotaRootBefore);
    assert(CountRegularFilesRecursive(byteQuotaBridge.ModDirectory() / "textures") == 1);
    // Replacing the existing hash with an equal-size texture is valid even
    // though the old cache file exists transiently until the next transaction.
    byteQuotaSecond.hashes = byteQuotaFirst.hashes;
    assert(byteQuotaBridge.ApplyLegacyMaterial(byteQuotaSecond).ok);
    assert(ReadText(byteQuotaMod) != byteQuotaRootBefore);
    assert(byteQuotaBridge.ApplyLegacyMaterial(byteQuotaSecond).ok);
    assert(CountRegularFilesRecursive(byteQuotaBridge.ModDirectory() / "textures") == 1);

    // An over-limit root is rejected for ordinary edits, but authoritative
    // reset deliberately bypasses root parsing so it can always recover.
    const auto recoveryRoot = root / "quota_recovery";
    std::filesystem::create_directories(recoveryRoot / "garrysmod" / "data");
    const auto recoveryMod =
        recoveryRoot / "rtx-remix" / "mods" / "!advanced_material_editor";
    std::filesystem::create_directories(recoveryMod / "profiles");
    std::vector<std::string> recoveryProfiles;
    for (std::uint64_t index = 1; index <= 129; ++index) {
        const auto hash = HexHash(0xE000000000000000ull + index);
        const auto id = "hash_" + hash + "_0000000000000001";
        recoveryProfiles.push_back(id);
        WriteText(recoveryMod / "profiles" / (id + ".usda"), LegacyLayer({hash}));
    }
    WriteText(recoveryMod / "mod.usda", ModLayer(recoveryProfiles));
    Config recoveryConfig;
    recoveryConfig.gameRoot = recoveryRoot;
    recoveryConfig.maxActiveProfiles = 128;
    Bridge recoveryBridge(std::move(recoveryConfig));
    ApplyRequest rejectedRecoveryEdit;
    rejectedRecoveryEdit.profileId = "quota_recovery";
    rejectedRecoveryEdit.hashes = {"F000000000000001"};
    const auto rejectedRecoveryResult =
        recoveryBridge.ApplyLegacyMaterial(rejectedRecoveryEdit);
    assert(!rejectedRecoveryResult.ok);
    assert(recoveryBridge.ClearAllOwned().ok);
    assert(ReadText(recoveryMod / "mod.usda") == ModLayer({}));

    const auto invalidLimitRoot = root / "invalid_limit";
    std::filesystem::create_directories(invalidLimitRoot / "garrysmod" / "data");
    std::filesystem::create_directories(invalidLimitRoot / "rtx-remix" / "mods");
    Config invalidLimitConfig;
    invalidLimitConfig.gameRoot = invalidLimitRoot;
    invalidLimitConfig.maxActiveProfiles = 127;
    Bridge invalidLimitBridge(std::move(invalidLimitConfig));
    assert(!invalidLimitBridge.GetCapabilities().available);

    // Validate legacy block-compressed layout math independently from the
    // larger transaction suite: 5x3 DXT1 mips consume 16 + 8 + 8 bytes.
    const auto ddsRoot = root / "dds_validation";
    std::filesystem::create_directories(ddsRoot / "garrysmod" / "data");
    std::filesystem::create_directories(ddsRoot / "rtx-remix" / "mods");
    const auto validDxt1 = ddsRoot / "garrysmod" / "data" / "valid_dxt1.dds";
    WriteLegacyFourCcDds(validDxt1, TestFourCc('D', 'X', 'T', '1'), 5, 3, 3, 32);
    Config ddsConfig;
    ddsConfig.gameRoot = ddsRoot;
    Bridge ddsBridge(std::move(ddsConfig));
    ApplyRequest ddsRequest;
    ddsRequest.profileId = "valid_dxt1";
    ddsRequest.hashes = {"F000000000000003"};
    ddsRequest.material.textures.push_back({TextureRole::Albedo, validDxt1, {}});
    const auto validDxt1Result = ddsBridge.ApplyLegacyMaterial(ddsRequest);
    assert(validDxt1Result.ok);
    const auto validDdsRootText = ReadText(ddsBridge.ModDirectory() / "mod.usda");

    const auto shortDxt1 = ddsRoot / "garrysmod" / "data" / "short_dxt1.dds";
    WriteLegacyFourCcDds(shortDxt1, TestFourCc('D', 'X', 'T', '1'), 5, 3, 3, 31);
    ApplyRequest invalidDdsRequest = ddsRequest;
    invalidDdsRequest.profileId = "short_dxt1";
    invalidDdsRequest.hashes = {"F000000000000004"};
    invalidDdsRequest.material.textures.front().source = shortDxt1;
    const auto shortDxt1Result = ddsBridge.ApplyLegacyMaterial(invalidDdsRequest);
    assert(!shortDxt1Result.ok && shortDxt1Result.code == "invalid_dds");
    assert(ReadText(ddsBridge.ModDirectory() / "mod.usda") == validDdsRootText);

    const auto alphaPixelsOnly =
        ddsRoot / "garrysmod" / "data" / "alpha_pixels_only.dds";
    WriteMinimalDds(alphaPixelsOnly, 1, 1, 1, std::nullopt, 0x1u);
    invalidDdsRequest.profileId = "alpha_pixels_only";
    invalidDdsRequest.hashes = {"F000000000000005"};
    invalidDdsRequest.material.textures.front().source = alphaPixelsOnly;
    const auto alphaPixelsOnlyResult = ddsBridge.ApplyLegacyMaterial(invalidDdsRequest);
    assert(!alphaPixelsOnlyResult.ok && alphaPixelsOnlyResult.code == "invalid_dds");
    assert(ReadText(ddsBridge.ModDirectory() / "mod.usda") == validDdsRootText);

    const auto ambiguousNumeric =
        ddsRoot / "garrysmod" / "data" / "ambiguous_numeric_fourcc.dds";
    WriteLegacyFourCcDds(ambiguousNumeric, 28, 1, 1, 1, 1);
    invalidDdsRequest.profileId = "ambiguous_numeric_fourcc";
    invalidDdsRequest.hashes = {"F000000000000006"};
    invalidDdsRequest.material.textures.front().source = ambiguousNumeric;
    const auto ambiguousNumericResult = ddsBridge.ApplyLegacyMaterial(invalidDdsRequest);
    assert(!ambiguousNumericResult.ok && ambiguousNumericResult.code == "invalid_dds");
    assert(ReadText(ddsBridge.ModDirectory() / "mod.usda") == validDdsRootText);

    const auto overflowDxt1 =
        ddsRoot / "garrysmod" / "data" / "overflow_dxt1.dds";
    constexpr auto maximumU32 = std::numeric_limits<std::uint32_t>::max();
    WriteLegacyFourCcDds(
        overflowDxt1, TestFourCc('D', 'X', 'T', '1'), maximumU32, maximumU32, 1, 8);
    Config overflowConfig;
    overflowConfig.gameRoot = ddsRoot;
    overflowConfig.maxTextureDimension = maximumU32;
    Bridge overflowBridge(std::move(overflowConfig));
    invalidDdsRequest.profileId = "overflow_dxt1";
    invalidDdsRequest.hashes = {"F000000000000007"};
    invalidDdsRequest.material.textures.front().source = overflowDxt1;
    const auto overflowDxt1Result = overflowBridge.ApplyLegacyMaterial(invalidDdsRequest);
    assert(!overflowDxt1Result.ok && overflowDxt1Result.code == "invalid_dds");
    assert(ReadText(ddsBridge.ModDirectory() / "mod.usda") == validDdsRootText);

    // Real Garry's Mod libraries are frequently installed below localized
    // directory names. A valid DX10/BC7 texture must survive that path without
    // ANSI narrowing or being mistaken for an unsafe source.
#ifdef _WIN32
    const auto unicodeRoot = root / std::filesystem::path(L"\u6750\u8D28\u6D4B\u8BD5");
    const auto unicodeSource = unicodeRoot / "garrysmod" / "data" /
        std::filesystem::path(L"\u8D34\u56FE.dds");
#else
    const auto unicodeRoot = root / std::filesystem::u8path(u8"\u6750\u8D28\u6D4B\u8BD5");
    const auto unicodeSource = unicodeRoot / "garrysmod" / "data" /
        std::filesystem::u8path(u8"\u8D34\u56FE.dds");
#endif
    std::filesystem::create_directories(unicodeRoot / "garrysmod" / "data");
    std::filesystem::create_directories(unicodeRoot / "rtx-remix" / "mods");
    WriteDx10Bc7Dds(unicodeSource);
    Config unicodeConfig;
    unicodeConfig.gameRoot = unicodeRoot;
    Bridge unicodeBridge(std::move(unicodeConfig));
    ApplyRequest unicodeRequest;
    unicodeRequest.profileId = "unicode_bc7";
    unicodeRequest.hashes = {"F000000000000002"};
    unicodeRequest.material.textures.push_back(
        {TextureRole::Albedo, unicodeSource, {}});
    const auto unicodeResult = unicodeBridge.ApplyLegacyMaterial(unicodeRequest);
    assert(unicodeResult.ok);
    assert(unicodeResult.importedTextures.size() == 1);
    assert(std::filesystem::is_regular_file(unicodeResult.importedTextures.front()));
    assert(RootReferences(ReadText(unicodeBridge.ModDirectory() / "mod.usda"),
                          unicodeResult.layerPath));

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::cout << "advmat RTX writer tests passed\n";
    return 0;
}
