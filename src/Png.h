#pragma once
#include "Terrain.h"
#include <filesystem>
#include <optional>
#include <vector>

namespace wkrgba {
inline constexpr uint32_t nativeProxyBorder=8;
// W:A's automatic placement search needs more than a sprite-height strip on
// a completely solid custom map.  128 pixels is the smallest value already
// verified in live play to keep the scheme's automatic teleporting enabled.
inline constexpr uint32_t nativeProxyFullMapTopClearance=128;
// Keep editor classification identical to the established offline geometry.
// Network packaging removes this disposable strip before adding gameplay
// clearance, so it must not be reduced globally for legacy compatibility.
inline constexpr uint32_t nativeProxyEditorTopClearance=128;
struct EncodedPng { Image image; std::vector<uint8_t> bytes; };
// Accepts specifically PNG colour type 6, 8 bits per channel (RGBA32).
// This restriction prevents silently treating black as transparency.
Image readRgbaPng(const std::filesystem::path& path);
EncodedPng readRgbaPngWithBytes(const std::filesystem::path& path);
// Native indexed PNGs are accepted only for validating map/replay association.
Image readPngMemory(std::span<const uint8_t> encoded, bool rgbaOnly = true);
std::vector<uint8_t> writeRgbaPngMemory(const Image& image);
uint8_t nativeProxyIndex(Pixel pixel,uint8_t threshold=128) noexcept;
Pixel nativeProxyColour(Pixel pixel) noexcept;
uint32_t nativeProxyTopClearance(const Image& image,uint8_t threshold=128) noexcept;
Image addTransparentTop(const Image& image,uint32_t rows);
bool pngDimensionsMatch(std::span<const uint8_t> encoded,uint32_t width,uint32_t height);
bool matchesNativeProxyPng(const Image& original,std::span<const uint8_t> encoded);
bool matchesNativeProxy(const Image& original,const Image& native);
std::vector<uint8_t> attachPrivatePayload(std::span<const uint8_t> png,std::span<const uint8_t> payload);
std::optional<std::vector<uint8_t>> extractPrivatePayload(std::span<const uint8_t> png);
std::optional<std::vector<uint8_t>> extractWaLevelMetadata(std::span<const uint8_t> png);
std::vector<uint8_t> attachWaLevelMetadata(std::span<const uint8_t> png,std::span<const uint8_t> metadata);
// Resizes an indexed W:A working PNG only along its top edge.  Shrinking drops
// the temporary placement clearance while retaining the native top border;
// growing prepends empty (palette index zero) rows.
std::vector<uint8_t> resizeIndexedPngTop(std::span<const uint8_t> png,uint32_t targetHeight,
                                         uint32_t retainedTopRows=0);
void writeRgbaPng(const std::filesystem::path& path, const Image& image);
// Temporary native representation, not the RGBA render surface.
void writeNativePng(const std::filesystem::path& path,const Image& image,uint8_t threshold,
                    std::span<const uint8_t> waLevelMetadata={},uint32_t classificationTopRows=0);
}
