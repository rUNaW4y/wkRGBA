#include "Png.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 3 || argc > 4 || (std::wstring(argv[1]) != L"inspect" && std::wstring(argv[1]) != L"preview")) {
            std::cerr << "wkRGBA tools (does not install game hooks)\n"
                      << "wkRGBATool inspect input.png\n"
                      << "wkRGBATool preview input.png output.png\n";
            return 2;
        }
        const bool preview = std::wstring(argv[1]) == L"preview";
        if ((preview && argc != 4) || (!preview && argc != 3)) throw std::invalid_argument("Invalid argument count");
        auto image = wkrgba::readRgbaPng(argv[2]);
        // Bounded exact colour count: one bit per RGB value = 2 MiB.
        std::vector<uint8_t> seen(1 << 21);
        size_t colours{}, transparent{}, partial{};
        for (auto p : image.pixels) {
            if (!p.a) ++transparent;
            else {
                if (p.a != 255) ++partial;
                const auto rgb = uint32_t(p.r) << 16 | uint32_t(p.g) << 8 | p.b;
                const auto bit = uint8_t(1 << (rgb & 7));
                if (!(seen[rgb >> 3] & bit)) { ++colours; seen[rgb >> 3] |= bit; }
            }
        }
        const auto solid=std::count_if(image.pixels.begin(),image.pixels.end(),[](wkrgba::Pixel p){return p.a>=128;});
        std::cout << image.width << " x " << image.height << " RGBA32\n"
                  << "Visible RGB colours: " << colours << " (no quantization)\n"
                  << "Transparent pixels: " << transparent << "; partial alpha: " << partial << '\n'
                  << "Solid pixels (alpha >= 128): " << solid << '\n';
        if (preview) {
            const std::filesystem::path out(argv[3]);
            if (std::filesystem::exists(out)) throw std::runtime_error("Output already exists; choose a new filename");
            auto result = std::move(image);
            for (uint32_t y = 0; y < result.height; ++y) {
                for (uint32_t x = 0; x < result.width; ++x) {
                    const uint8_t shade = ((x / 16 + y / 16) & 1) ? 72 : 48;
                    auto& p = result.pixels[size_t(y) * result.width + x];
                    if(p.a<128)p={};
                    p = wkrgba::overOpaque(p, {shade,shade,shade,255});
                }
            }
            wkrgba::writeRgbaPng(out, result);
            std::cout << "Terrain preview written (alpha below 128 removed).\n";
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << "wkRGBA: " << e.what() << '\n'; return 1; }
}
