#include "Terrain.h"
#include <stdexcept>
#include <utility>

namespace wkrgba {
size_t checkedArea(uint32_t w, uint32_t h) {
    if (!w || !h || w > maxDimension || h > maxDimension || uint64_t(w) * h > maxPixels)
        throw std::invalid_argument("Map dimensions exceed wkRGBA's x86 memory budget");
    return size_t(w) * h;
}
void Image::validate() const {
    if (pixels.size() != checkedArea(width, height))
        throw std::invalid_argument("RGBA buffer size does not match dimensions");
}
Terrain::Terrain(Image image, uint8_t threshold) : source_(std::move(image)), threshold_(threshold) {
    source_.validate();
    if (!threshold) throw std::invalid_argument("Alpha threshold must be 1..255");
    reset();
}
void Terrain::reset() {
    live_ = source_;
    solid_.resize(source_.pixels.size());
    for (size_t i = 0; i < solid_.size(); ++i) {
        solid_[i] = source_.pixels[i].a >= threshold_;
        if (!solid_[i]) live_.pixels[i] = {};
    }
}
void Terrain::sync(Rect r, std::span<const uint8_t> mask, size_t stride,
                   std::span<const Pixel> colours, size_t colourStride) {
    if (r.x > live_.width || r.y > live_.height || r.width > live_.width - r.x || r.height > live_.height - r.y)
        throw std::invalid_argument("Dirty rectangle outside map");
    if (!r.width || !r.height) return;
    // Division avoids overflow even with an untrusted stride.
    const auto fits = [&](size_t count, size_t pitch) {
        return pitch >= r.width && count >= r.width &&
               (r.height == 1 || pitch <= (count - r.width) / (r.height - 1));
    };
    if (!fits(mask.size(), stride) || (!colours.empty() && !fits(colours.size(), colourStride)))
        throw std::invalid_argument("Dirty rectangle buffer is too small");
    for (uint32_t y = 0; y < r.height; ++y) {
        for (uint32_t x = 0; x < r.width; ++x) {
            const auto i = size_t(r.y + y) * live_.width + r.x + x;
            const bool occupied = mask[size_t(y) * stride + x] != 0;
            if (!occupied) live_.pixels[i] = {};
            else if (!solid_[i]) {
                // New girders/terrain must not resurrect the original artwork.
                live_.pixels[i] = colours.empty() ? Pixel{128, 128, 128, 255} : colours[size_t(y) * colourStride + x];
                live_.pixels[i].a = 255;
            }
            solid_[i] = occupied;
        }
    }
}
Pixel overOpaque(Pixel f, Pixel b) {
    const auto mix = [a = unsigned(f.a)](unsigned s, unsigned d) {
        return uint8_t((s * a + d * (255 - a) + 127) / 255);
    };
    return {mix(f.r, b.r), mix(f.g, b.g), mix(f.b, b.b), 255};
}
}
