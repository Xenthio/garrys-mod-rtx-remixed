#include "internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#endif

namespace advmat::rtx_bridge::detail {
namespace {

constexpr std::array<std::uint8_t, 4> kDdsMagic{'D', 'D', 'S', ' '};

constexpr std::uint32_t FourCc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

enum class DdsStorage { BitsPerPixel, BlockCompressed, PackedPairs };

struct DdsLayout {
    DdsStorage storage;
    std::uint32_t unit;
};

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool ReadFile(const std::filesystem::path& path, std::uintmax_t maximum,
              std::vector<std::uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "cannot determine texture size: " + ec.message();
        return false;
    }
    if (size == 0 || size > maximum || size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max())) {
        error = "texture size is empty or exceeds the configured safety limit";
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream || !stream.read(reinterpret_cast<char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size()))) {
        error = "cannot read texture source";
        return false;
    }
    return true;
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

std::optional<DdsLayout> LegacyFourCcLayout(std::uint32_t fourCc) {
    switch (fourCc) {
    case FourCc('D', 'X', 'T', '1'):
    case FourCc('A', 'T', 'I', '1'):
    case FourCc('B', 'C', '4', 'U'):
    case FourCc('B', 'C', '4', 'S'):
        return DdsLayout{DdsStorage::BlockCompressed, 8};
    case FourCc('D', 'X', 'T', '2'):
    case FourCc('D', 'X', 'T', '3'):
    case FourCc('D', 'X', 'T', '4'):
    case FourCc('D', 'X', 'T', '5'):
    case FourCc('A', 'T', 'I', '2'):
    case FourCc('B', 'C', '5', 'U'):
    case FourCc('B', 'C', '5', 'S'):
    case FourCc('A', '2', 'D', '5'):
    case FourCc('x', 'G', 'B', 'R'):
    case FourCc('R', 'x', 'B', 'G'):
    case FourCc('R', 'B', 'x', 'G'):
    case FourCc('x', 'R', 'B', 'G'):
    case FourCc('R', 'G', 'x', 'B'):
    case FourCc('x', 'G', 'x', 'R'):
    case FourCc('G', 'X', 'R', 'B'):
    case FourCc('G', 'R', 'X', 'B'):
    case FourCc('R', 'X', 'G', 'B'):
    case FourCc('B', 'R', 'G', 'X'):
    case FourCc('A', '2', 'X', 'Y'):
    case FourCc('B', 'C', '6', 'H'):
    case FourCc('B', 'C', '7', 'L'):
    case FourCc('B', 'C', '7', '\0'):
        return DdsLayout{DdsStorage::BlockCompressed, 16};
    case FourCc('R', 'G', 'B', 'G'):
    case FourCc('G', 'R', 'G', 'B'):
    case FourCc('U', 'Y', 'V', 'Y'):
    case FourCc('Y', 'U', 'Y', '2'):
        return DdsLayout{DdsStorage::PackedPairs, 4};
    // DDS writers historically place only this documented subset of
    // D3DFORMAT enum values in dwFourCC. Reject the lower enum values because
    // they collide with ad-hoc DXGI-in-FourCC files and have ambiguous sizes.
    case 36: case 110: case 113: case 115:
        return DdsLayout{DdsStorage::BitsPerPixel, 64};
    case 111:
        return DdsLayout{DdsStorage::BitsPerPixel, 16};
    case 112: case 114:
        return DdsLayout{DdsStorage::BitsPerPixel, 32};
    case 116:
        return DdsLayout{DdsStorage::BitsPerPixel, 128};
    default:
        return std::nullopt;
    }
}

std::optional<DdsLayout> DxgiLayout(std::uint32_t format) {
    switch (format) {
    case 70: case 71: case 72:
    case 79: case 80: case 81:
        return DdsLayout{DdsStorage::BlockCompressed, 8};
    case 73: case 74: case 75:
    case 76: case 77: case 78:
    case 82: case 83: case 84:
    case 94: case 95: case 96:
    case 97: case 98: case 99:
        return DdsLayout{DdsStorage::BlockCompressed, 16};
    case 68: case 69:
        return DdsLayout{DdsStorage::PackedPairs, 4};
    case 1: case 2: case 3: case 4:
        return DdsLayout{DdsStorage::BitsPerPixel, 128};
    case 5: case 6: case 7: case 8:
        return DdsLayout{DdsStorage::BitsPerPixel, 96};
    case 9: case 10: case 11: case 12: case 13: case 14:
    case 15: case 16: case 17: case 18:
    case 19: case 20: case 21: case 22:
        return DdsLayout{DdsStorage::BitsPerPixel, 64};
    case 23: case 24: case 25: case 26:
    case 27: case 28: case 29: case 30: case 31: case 32:
    case 33: case 34: case 35: case 36: case 37: case 38:
    case 39: case 40: case 41: case 42: case 43:
    case 44: case 45: case 46: case 47:
    case 67: case 87: case 88: case 89: case 90: case 91: case 92: case 93:
        return DdsLayout{DdsStorage::BitsPerPixel, 32};
    case 48: case 49: case 50: case 51: case 52:
    case 53: case 54: case 55: case 56: case 57: case 58: case 59:
    case 85: case 86: case 115: case 191:
        return DdsLayout{DdsStorage::BitsPerPixel, 16};
    case 60: case 61: case 62: case 63: case 64: case 65:
        return DdsLayout{DdsStorage::BitsPerPixel, 8};
    case 66:
        return DdsLayout{DdsStorage::BitsPerPixel, 1};
    default:
        return std::nullopt;
    }
}

bool MultiplyWithin(std::uintmax_t left, std::uintmax_t right,
                    std::uintmax_t limit, std::uintmax_t& product) {
    if (left != 0 && right > limit / left) return false;
    product = left * right;
    return true;
}

bool AddDdsMipBytes(const DdsLayout& layout, std::uint32_t width,
                     std::uint32_t height, std::uintmax_t& required,
                     std::uintmax_t available) {
    if (required > available) return false;
    const auto remaining = available - required;
    std::uintmax_t levelBytes = 0;
    if (layout.storage == DdsStorage::BlockCompressed) {
        const auto blocksWide = std::max<std::uintmax_t>(
            1, (static_cast<std::uintmax_t>(width) + 3u) / 4u);
        const auto blocksHigh = std::max<std::uintmax_t>(
            1, (static_cast<std::uintmax_t>(height) + 3u) / 4u);
        std::uintmax_t blockCount = 0;
        if (!MultiplyWithin(blocksWide, blocksHigh, remaining, blockCount) ||
            !MultiplyWithin(blockCount, layout.unit, remaining, levelBytes)) {
            return false;
        }
    } else if (layout.storage == DdsStorage::PackedPairs) {
        const auto pairsWide = (static_cast<std::uintmax_t>(width) + 1u) / 2u;
        std::uintmax_t rowBytes = 0;
        if (!MultiplyWithin(pairsWide, layout.unit, remaining, rowBytes) ||
            !MultiplyWithin(rowBytes, height, remaining, levelBytes)) {
            return false;
        }
    } else {
        std::uintmax_t rowBits = 0;
        if (!MultiplyWithin(width, layout.unit,
                std::numeric_limits<std::uintmax_t>::max(), rowBits)) {
            return false;
        }
        const auto rowBytes = rowBits / 8u + (rowBits % 8u != 0 ? 1u : 0u);
        if (!MultiplyWithin(rowBytes, height, remaining, levelBytes)) return false;
    }
    if (levelBytes > remaining) return false;
    required += levelBytes;
    return true;
}

bool ValidateDds(const std::vector<std::uint8_t>& bytes, std::uint32_t maximumDimension,
                 std::string& error) {
    if (bytes.size() < 128 || !std::equal(kDdsMagic.begin(), kDdsMagic.end(), bytes.begin())) {
        error = "DDS source has no valid DDS magic/header";
        return false;
    }
    const auto headerSize = ReadU32(bytes, 4);
    const auto height = ReadU32(bytes, 12);
    const auto width = ReadU32(bytes, 16);
    const auto depth = ReadU32(bytes, 24);
    auto mipCount = ReadU32(bytes, 28);
    const auto pixelFormatSize = ReadU32(bytes, 76);
    if (headerSize != 124 || pixelFormatSize != 32 || width == 0 || height == 0 ||
        width > maximumDimension || height > maximumDimension) {
        error = "DDS header or dimensions are invalid";
        return false;
    }
    const auto caps2 = ReadU32(bytes, 112);
    constexpr std::uint32_t unsupportedLegacyResources = 0x0000FE00u | 0x00200000u;
    if (depth > 1 || (caps2 & unsupportedLegacyResources) != 0) {
        error = "DDS arrays, cube maps and volume textures are not valid material maps";
        return false;
    }

    if (mipCount == 0) mipCount = 1;
    std::uint32_t maximumMipCount = 1;
    for (auto dimension = std::max(width, height); dimension > 1; dimension /= 2) {
        ++maximumMipCount;
    }
    if (mipCount > maximumMipCount) {
        error = "DDS mip count exceeds the declared dimensions";
        return false;
    }

    constexpr std::uint32_t ddpfFourCc = 0x4u;
    constexpr std::uint32_t ddpfAlphaPixels = 0x1u;
    constexpr std::uint32_t ddpfAlpha = 0x2u;
    constexpr std::uint32_t ddpfRgb = 0x40u;
    constexpr std::uint32_t ddpfLuminance = 0x20000u;
    constexpr std::uint32_t ddpfBumpDuDv = 0x80000u;
    constexpr std::uint32_t ddpfUncompressedPrimary =
        ddpfAlpha | ddpfRgb | ddpfLuminance | ddpfBumpDuDv;
    const auto pixelFlags = ReadU32(bytes, 80);
    const auto fourCc = ReadU32(bytes, 84);
    std::size_t dataOffset = 128;
    std::optional<DdsLayout> layout;
    if ((pixelFlags & ddpfFourCc) != 0) {
        if (fourCc == FourCc('D', 'X', '1', '0')) {
            if (bytes.size() < 148) {
                error = "DDS DX10 header is truncated";
                return false;
            }
            const auto dxgiFormat = ReadU32(bytes, 128);
            const auto resourceDimension = ReadU32(bytes, 132);
            const auto miscFlag = ReadU32(bytes, 136);
            const auto arraySize = ReadU32(bytes, 140);
            if (resourceDimension != 3 || arraySize != 1 || (miscFlag & 0x4u) != 0) {
                error = "DDS arrays, cube maps and non-2D resources are not valid material maps";
                return false;
            }
            layout = DxgiLayout(dxgiFormat);
            dataOffset = 148;
        } else {
            layout = LegacyFourCcLayout(fourCc);
        }
    } else if ((pixelFlags & ddpfUncompressedPrimary) != 0) {
        const auto primaryFlags = pixelFlags & ddpfUncompressedPrimary;
        const auto bitsPerPixel = ReadU32(bytes, 88);
        const auto redMask = ReadU32(bytes, 92);
        const auto greenMask = ReadU32(bytes, 96);
        const auto blueMask = ReadU32(bytes, 100);
        const auto alphaMask = ReadU32(bytes, 104);
        const bool onePrimaryFlag = (primaryFlags & (primaryFlags - 1u)) == 0;
        const bool supportedBits = bitsPerPixel == 8 || bitsPerPixel == 16 ||
            bitsPerPixel == 24 || bitsPerPixel == 32 || bitsPerPixel == 64 ||
            bitsPerPixel == 128;
        const bool primaryMaskPresent = primaryFlags == ddpfAlpha
            ? alphaMask != 0
            : (redMask | greenMask | blueMask) != 0;
        bool masksFit = true;
        if (bitsPerPixel < 32) {
            const auto allowedMask = (std::uint32_t{1} << bitsPerPixel) - 1u;
            masksFit = ((redMask | greenMask | blueMask | alphaMask) & ~allowedMask) == 0;
        }
        const bool masksDisjoint = (redMask & greenMask) == 0 &&
            (redMask & blueMask) == 0 && (redMask & alphaMask) == 0 &&
            (greenMask & blueMask) == 0 && (greenMask & alphaMask) == 0 &&
            (blueMask & alphaMask) == 0;
        const bool alphaFlagConsistent = (pixelFlags & ddpfAlphaPixels) != 0 ||
            alphaMask == 0 || primaryFlags == ddpfAlpha;
        if (onePrimaryFlag && supportedBits && primaryMaskPresent && masksFit &&
            masksDisjoint && alphaFlagConsistent) {
            layout = DdsLayout{DdsStorage::BitsPerPixel, bitsPerPixel};
        }
    }
    if (!layout) {
        error = "DDS pixel format is unsupported or malformed";
        return false;
    }

    std::uintmax_t required = dataOffset;
    auto mipWidth = width;
    auto mipHeight = height;
    for (std::uint32_t level = 0; level < mipCount; ++level) {
        if (!AddDdsMipBytes(*layout, mipWidth, mipHeight, required, bytes.size())) {
            error = "DDS pixel or mip payload is truncated";
            return false;
        }
        mipWidth = std::max(1u, mipWidth / 2u);
        mipHeight = std::max(1u, mipHeight / 2u);
    }
    return true;
}

bool IsWicExtension(const std::string& extension) {
    static constexpr std::array<const char*, 7> supported{
        ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".gif"};
    return std::find(supported.begin(), supported.end(), extension) != supported.end();
}

std::uint64_t Fnv1a(const std::vector<std::uint8_t>& bytes,
                    const std::vector<TextureOperation>& operations) {
    std::uint64_t hash = 14695981039346656037ull;
    const auto add = [&hash](const void* data, std::size_t size) {
        const auto* cursor = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash ^= cursor[index];
            hash *= 1099511628211ull;
        }
    };
    add(bytes.data(), bytes.size());
    for (const auto& operation : operations) {
        const auto kind = static_cast<std::uint8_t>(operation.kind);
        add(&kind, sizeof(kind));
        add(&operation.color, sizeof(operation.color));
        add(&operation.strength, sizeof(operation.strength));
        const auto channel = static_cast<std::uint8_t>(operation.channel);
        add(&channel, sizeof(channel));
    }
    return hash;
}

std::string Hex(std::uint64_t value) {
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

#ifdef _WIN32
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bgra;
};

void ApplyOperations(Image& image, const std::vector<TextureOperation>& operations) {
    for (const auto& operation : operations) {
        if (operation.kind == TextureOperationKind::Invert) {
            for (std::size_t offset = 0; offset < image.bgra.size(); offset += 4) {
                image.bgra[offset + 0] = static_cast<std::uint8_t>(255 - image.bgra[offset + 0]);
                image.bgra[offset + 1] = static_cast<std::uint8_t>(255 - image.bgra[offset + 1]);
                image.bgra[offset + 2] = static_cast<std::uint8_t>(255 - image.bgra[offset + 2]);
            }
        } else if (operation.kind == TextureOperationKind::Multiply) {
            for (std::size_t offset = 0; offset < image.bgra.size(); offset += 4) {
                image.bgra[offset + 0] = static_cast<std::uint8_t>(std::clamp(
                    image.bgra[offset + 0] * operation.color.z, 0.0f, 255.0f));
                image.bgra[offset + 1] = static_cast<std::uint8_t>(std::clamp(
                    image.bgra[offset + 1] * operation.color.y, 0.0f, 255.0f));
                image.bgra[offset + 2] = static_cast<std::uint8_t>(std::clamp(
                    image.bgra[offset + 2] * operation.color.x, 0.0f, 255.0f));
            }
        } else if (operation.kind == TextureOperationKind::NormalFromHeight) {
            const auto source = image.bgra;
            const auto heightSample = [&source, &image, &operation](int x, int y) {
                x = std::clamp(x, 0, static_cast<int>(image.width) - 1);
                y = std::clamp(y, 0, static_cast<int>(image.height) - 1);
                const auto offset = (static_cast<std::size_t>(y) * image.width +
                                     static_cast<std::size_t>(x)) * 4;
                switch (operation.channel) {
                case HeightChannel::Red: return source[offset + 2] / 255.0f;
                case HeightChannel::Green: return source[offset + 1] / 255.0f;
                case HeightChannel::Blue: return source[offset + 0] / 255.0f;
                case HeightChannel::Alpha: return source[offset + 3] / 255.0f;
                case HeightChannel::Luminance:
                default:
                    return (source[offset + 2] * 0.2126f + source[offset + 1] * 0.7152f +
                            source[offset + 0] * 0.0722f) / 255.0f;
                }
            };
            for (std::uint32_t y = 0; y < image.height; ++y) {
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    const int ix = static_cast<int>(x);
                    const int iy = static_cast<int>(y);
                    const float gx =
                        -heightSample(ix - 1, iy - 1) - 2.0f * heightSample(ix - 1, iy) -
                        heightSample(ix - 1, iy + 1) + heightSample(ix + 1, iy - 1) +
                        2.0f * heightSample(ix + 1, iy) + heightSample(ix + 1, iy + 1);
                    const float gy =
                        -heightSample(ix - 1, iy - 1) - 2.0f * heightSample(ix, iy - 1) -
                        heightSample(ix + 1, iy - 1) + heightSample(ix - 1, iy + 1) +
                        2.0f * heightSample(ix, iy + 1) + heightSample(ix + 1, iy + 1);
                    float nx = -gx * operation.strength;
                    float ny = -gy * operation.strength;
                    float nz = 1.0f;
                    const float inverseLength = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
                    nx *= inverseLength;
                    ny *= inverseLength;
                    nz *= inverseLength;
                    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * 4;
                    image.bgra[offset + 0] = static_cast<std::uint8_t>((nz * 0.5f + 0.5f) * 255.0f);
                    image.bgra[offset + 1] = static_cast<std::uint8_t>((ny * 0.5f + 0.5f) * 255.0f);
                    image.bgra[offset + 2] = static_cast<std::uint8_t>((nx * 0.5f + 0.5f) * 255.0f);
                    image.bgra[offset + 3] = 255;
                }
            }
        }
    }
}

#pragma pack(push, 1)
struct DdsPixelFormat {
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t fourCc;
    std::uint32_t rgbBitCount;
    std::uint32_t redMask;
    std::uint32_t greenMask;
    std::uint32_t blueMask;
    std::uint32_t alphaMask;
};

struct DdsHeader {
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t height;
    std::uint32_t width;
    std::uint32_t pitch;
    std::uint32_t depth;
    std::uint32_t mipMapCount;
    std::uint32_t reserved[11];
    DdsPixelFormat pixelFormat;
    std::uint32_t caps;
    std::uint32_t caps2;
    std::uint32_t caps3;
    std::uint32_t caps4;
    std::uint32_t reserved2;
};
#pragma pack(pop)

static_assert(sizeof(DdsHeader) == 124, "DDS header layout must be 124 bytes");

std::vector<std::uint8_t> EncodeDds(Image image, bool normalizeNormalMips) {
    std::uint32_t mipCount = 1;
    for (auto dimension = std::max(image.width, image.height); dimension > 1; dimension /= 2) {
        ++mipCount;
    }

    DdsHeader header{};
    header.size = 124;
    header.flags = 0x1u | 0x2u | 0x4u | 0x8u | 0x1000u | 0x20000u;
    header.height = image.height;
    header.width = image.width;
    header.pitch = image.width * 4u;
    header.depth = 1;
    header.mipMapCount = mipCount;
    header.pixelFormat = {32, 0x1u | 0x40u, 0, 32,
                          0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u};
    header.caps = 0x1000u | 0x8u | 0x400000u;

    std::vector<std::uint8_t> output;
    output.reserve(128 + image.bgra.size() * 4 / 3 + 64);
    output.insert(output.end(), kDdsMagic.begin(), kDdsMagic.end());
    const auto* headerBytes = reinterpret_cast<const std::uint8_t*>(&header);
    output.insert(output.end(), headerBytes, headerBytes + sizeof(header));

    auto current = std::move(image.bgra);
    auto width = image.width;
    auto height = image.height;
    output.insert(output.end(), current.begin(), current.end());
    while (width > 1 || height > 1) {
        const auto nextWidth = std::max(1u, width / 2u);
        const auto nextHeight = std::max(1u, height / 2u);
        std::vector<std::uint8_t> next(static_cast<std::size_t>(nextWidth) * nextHeight * 4u);
        for (std::uint32_t y = 0; y < nextHeight; ++y) {
            for (std::uint32_t x = 0; x < nextWidth; ++x) {
                std::uint32_t sums[4]{};
                std::uint32_t samples = 0;
                for (std::uint32_t dy = 0; dy < 2 && y * 2 + dy < height; ++dy) {
                    for (std::uint32_t dx = 0; dx < 2 && x * 2 + dx < width; ++dx) {
                        const auto sourceOffset =
                            (static_cast<std::size_t>(y * 2 + dy) * width + x * 2 + dx) * 4;
                        for (int channel = 0; channel < 4; ++channel) {
                            sums[channel] += current[sourceOffset + channel];
                        }
                        ++samples;
                    }
                }
                const auto destinationOffset =
                    (static_cast<std::size_t>(y) * nextWidth + x) * 4;
                for (int channel = 0; channel < 4; ++channel) {
                    next[destinationOffset + channel] =
                        static_cast<std::uint8_t>(sums[channel] / samples);
                }
                if (normalizeNormalMips) {
                    float nx = next[destinationOffset + 2] / 127.5f - 1.0f;
                    float ny = next[destinationOffset + 1] / 127.5f - 1.0f;
                    float nz = next[destinationOffset + 0] / 127.5f - 1.0f;
                    const float lengthSquared = nx * nx + ny * ny + nz * nz;
                    if (lengthSquared > 1.0e-12f) {
                        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
                        nx *= inverseLength;
                        ny *= inverseLength;
                        nz *= inverseLength;
                    } else {
                        nx = 0.0f;
                        ny = 0.0f;
                        nz = 1.0f;
                    }
                    const auto encodeComponent = [](float value) {
                        const float encoded = std::clamp(
                            (value * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f);
                        return static_cast<std::uint8_t>(encoded + 0.5f);
                    };
                    next[destinationOffset + 0] = encodeComponent(nz);
                    next[destinationOffset + 1] = encodeComponent(ny);
                    next[destinationOffset + 2] = encodeComponent(nx);
                }
            }
        }
        output.insert(output.end(), next.begin(), next.end());
        current = std::move(next);
        width = nextWidth;
        height = nextHeight;
    }
    return output;
}

template <typename T>
class ComObject {
public:
    ~ComObject() { if (value_) value_->Release(); }
    T** Put() { return &value_; }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
private:
    T* value_ = nullptr;
};

class ComApartment {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
                     owns_(result_ == S_OK || result_ == S_FALSE) {}
    ~ComApartment() { if (owns_) CoUninitialize(); }
    HRESULT Result() const { return result_; }
private:
    HRESULT result_;
    bool owns_;
};

bool DecodeWithWic(const std::filesystem::path& path, std::uint32_t maximumDimension,
                   std::uintmax_t maximumBytes,
                   Image& image, std::string& error) {
    ComApartment apartment;
    if (FAILED(apartment.Result()) && apartment.Result() != RPC_E_CHANGED_MODE) {
        error = "COM initialization failed";
        return false;
    }

    ComObject<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.Put()));
    ComObject<IWICBitmapDecoder> decoder;
    ComObject<IWICBitmapFrameDecode> frame;
    ComObject<IWICFormatConverter> converter;
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, decoder.Put());
    }
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, frame.Put());
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(converter.Put());
    if (SUCCEEDED(result)) {
        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
    std::uintmax_t encodedBytes = 128;
    if (SUCCEEDED(result) && (width == 0 || height == 0 || width > maximumDimension ||
                              height > maximumDimension)) {
        result = E_INVALIDARG;
        error = "decoded texture dimensions exceed the configured safety limit";
    }
    if (SUCCEEDED(result)) {
        auto mipWidth = static_cast<std::uintmax_t>(width);
        auto mipHeight = static_cast<std::uintmax_t>(height);
        while (true) {
            const auto rowBytes = mipWidth * 4u;
            if (encodedBytes > maximumBytes || rowBytes > maximumBytes ||
                mipHeight > (maximumBytes - encodedBytes) / rowBytes) {
                result = E_INVALIDARG;
                error = "decoded texture mip chain exceeds the configured safety limit";
                break;
            }
            encodedBytes += rowBytes * mipHeight;
            if (mipWidth == 1 && mipHeight == 1) break;
            mipWidth = std::max<std::uintmax_t>(1, mipWidth / 2);
            mipHeight = std::max<std::uintmax_t>(1, mipHeight / 2);
        }
    }
    if (SUCCEEDED(result)) {
        const auto rowBytes = static_cast<std::uintmax_t>(width) * 4u;
        const auto baseBytes = rowBytes * height;
        if (rowBytes > std::numeric_limits<UINT>::max() ||
            baseBytes > std::numeric_limits<UINT>::max()) {
            result = E_INVALIDARG;
            error = "decoded texture exceeds the Windows decoder buffer limit";
        }
    }
    if (SUCCEEDED(result)) {
        image.width = width;
        image.height = height;
        image.bgra.resize(static_cast<std::size_t>(width) * height * 4u);
        result = converter->CopyPixels(nullptr, width * 4u,
            static_cast<UINT>(image.bgra.size()), image.bgra.data());
    }
    if (FAILED(result)) {
        if (error.empty()) {
            error = "Windows Imaging Component could not decode the texture (HRESULT " +
                std::to_string(static_cast<unsigned long>(result)) + ")";
        }
        return false;
    }
    return true;
}
#endif

} // namespace

bool SupportsWicConversion() noexcept {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

bool SupportsTextureOperations() noexcept {
    return SupportsWicConversion();
}

Result ImportTexture(const TextureImportConfig& config,
                     const std::string& profileId,
                     const TextureInput& input,
                     ImportedTexture& output) {
    std::error_code ec;
    std::filesystem::path source = input.source;
    if (source.is_relative()) {
        // The Lua file API commonly reports paths relative to garrysmod/ or
        // garrysmod/data/. Resolve only these well-known in-install locations.
        const auto& gameRoot = config.gameRoot;
        const std::filesystem::path candidates[]{
            gameRoot / source,
            gameRoot / "garrysmod" / source,
            gameRoot / "garrysmod" / "data" / source,
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::is_regular_file(candidate, ec)) {
                source = candidate;
                break;
            }
            ec.clear();
        }
    }
    const auto canonicalSource = std::filesystem::canonical(source, ec);
    if (ec || !std::filesystem::is_regular_file(canonicalSource, ec)) {
        return Result::Failure("invalid_texture_path", "texture source is not a readable regular file");
    }

    bool allowed = false;
    for (const auto& root : config.allowedRoots) {
        if (IsDescendantOrSame(canonicalSource, root)) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        return Result::Failure("texture_path_outside_allowed_roots",
            "texture source resolves outside the configured import roots");
    }

    // ~gmod_topbr belongs to the client's ToPBR pipeline.  Even though it is
    // below the broad default game-root import boundary, never let this addon
    // read or derive assets from another writer's managed output tree.
    std::error_code protectedEc;
    const auto protectedRoot = std::filesystem::canonical(
        config.gameRoot / "rtx-remix" / "mods" / "~gmod_topbr", protectedEc);
    if (!protectedEc && IsDescendantOrSame(canonicalSource, protectedRoot)) {
        return Result::Failure("protected_texture_path",
            "texture source is inside the protected ~gmod_topbr output tree");
    }

    const auto extension = Lowercase(canonicalSource.extension().string());
    const bool isDds = extension == ".dds";
    if (!isDds && !IsWicExtension(extension)) {
        return Result::Failure("unsupported_texture_format",
            "only DDS and Windows Imaging Component formats are accepted");
    }
#ifndef _WIN32
    if (!isDds) {
        return Result::Failure("texture_conversion_unavailable",
            "this build can only import existing DDS files");
    }
#endif

    std::vector<std::uint8_t> sourceBytes;
    std::string error;
    if (!ReadFile(canonicalSource, config.maxBytes, sourceBytes, error)) {
        return Result::Failure("texture_read_failed", error);
    }
    if (isDds) {
        if (!input.operations.empty()) {
            return Result::Failure("dds_bake_unsupported",
                "bake operations require a WIC-decodable source such as PNG; compressed DDS editing is not supported");
        }
        if (!ValidateDds(sourceBytes, config.maxDimension, error)) {
            return Result::Failure("invalid_dds", error);
        }
    }

    // Derive and validate the content-addressed destination before decoding.
    // An unchanged generated image therefore costs one bounded source read and
    // hash, but no WIC decode, bake pass, mip generation, or file rewrite.
    const auto digest = Hex(Fnv1a(sourceBytes, input.operations));
    const std::string filename = std::string(TextureRoleName(input.role)) + "_" + digest + ".dds";
    const auto destination = config.modDirectory / "textures" / profileId / filename;
    if (!IsDescendantOrSame(destination, config.modDirectory)) {
        return Result::Failure("unsafe_texture_destination", "computed texture destination escaped the mod directory");
    }
    bool runtimeExists = false;
    if (!InspectPlainPath(config.gameRoot / "rtx-remix", true,
                          runtimeExists, error) || !runtimeExists) {
        if (error.empty()) error = "rtx-remix runtime directory is missing";
        return Result::Failure("unsafe_texture_destination", error);
    }
    const auto destinationDirectory = destination.parent_path();
    if (!EnsurePlainDirectoryTree(config.gameRoot, destinationDirectory, error)) {
        return Result::Failure("unsafe_texture_destination", error);
    }
    bool destinationExists = false;
    if (!InspectPlainPath(destination, false, destinationExists, error)) {
        return Result::Failure("unsafe_texture_destination", error);
    }
    output.role = input.role;
    output.absolutePath = destination;
    output.layerAssetPath = "../textures/" + profileId + "/" + filename;
    if (destinationExists) {
        std::vector<std::uint8_t> cachedBytes;
        std::string cacheError;
        const bool validCache = ReadFile(
            destination, config.maxBytes, cachedBytes, cacheError) &&
            ValidateDds(cachedBytes, config.maxDimension, cacheError);
        // For a direct DDS import the content-address key is a digest of these
        // exact bytes, so a structurally valid but altered cache file must not
        // be trusted. WIC-derived output has no cheap expected byte stream and
        // is still protected by full structural validation.
        if (validCache && (!isDds || cachedBytes == sourceBytes)) {
            output.changed = false;
            return Result::Success(profileId);
        }
    }

    std::vector<std::uint8_t> ddsBytes;
    if (isDds) {
        ddsBytes = std::move(sourceBytes);
    } else {
#ifdef _WIN32
        Image image;
        if (!DecodeWithWic(canonicalSource, config.maxDimension, config.maxBytes, image, error)) {
            return Result::Failure("texture_decode_failed", error);
        }
        ApplyOperations(image, input.operations);
        ddsBytes = EncodeDds(std::move(image), input.role == TextureRole::Normal);
#else
        return Result::Failure("texture_conversion_unavailable", "unreachable conversion path");
#endif
    }

    if (!ValidatePlainDirectoryTree(config.gameRoot, destinationDirectory, error) ||
        !InspectPlainPath(destination, false, destinationExists, error)) {
        return Result::Failure("unsafe_texture_destination", error);
    }
    if (!AtomicWriteBytes(destination, ddsBytes, error)) {
        return Result::Failure("texture_write_failed", error);
    }
    output.changed = true;
    return Result::Success(profileId);
}

} // namespace advmat::rtx_bridge::detail
