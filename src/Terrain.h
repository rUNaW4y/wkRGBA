#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace wkrgba {
struct Pixel {
    uint8_t r{}, g{}, b{}, a{};
    bool operator==(const Pixel&) const = default;
};
static_assert(sizeof(Pixel) == 4);
constexpr uint32_t maxDimension = 32768;
// The game executable is large-address-aware.  The production renderer keeps
// one source image and materializes only the visible viewport, so maps of this
// size remain inside the 32-bit address space.
constexpr uint64_t maxPixels = 192ull * 1024 * 1024;
size_t checkedArea(uint32_t width, uint32_t height);
struct Image {
    uint32_t width{}, height{};
    std::vector<Pixel> pixels;
    void validate() const;
};
struct Rect { uint32_t x{}, y{}, width{}, height{}; };

// Source colour and authoritative game occupancy are separate. This class
// never simulates explosions: an adapter must supply the game's current mask.
class Terrain {
public:
    explicit Terrain(Image source, uint8_t alphaThreshold = 128);
    const Image& image() const { return live_; }
    const std::vector<uint8_t>& mask() const { return solid_; }
    void sync(Rect rect, std::span<const uint8_t> mask, size_t stride,
              std::span<const Pixel> nativeColours = {}, size_t colourStride = 0);
    void reset();
private:
    Image source_, live_;
    std::vector<uint8_t> solid_;
    uint8_t threshold_;
};
// Straight-alpha source-over on an opaque destination; no palette conversion.
Pixel overOpaque(Pixel foreground, Pixel background);
}
