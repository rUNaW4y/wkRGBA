#pragma once
#include "Transfer.h"
namespace wkrgba::replay {
struct Artwork {Image image;net::Bytes png;};
// W:A v20 settings extension. Native map bytes and event stream are preserved.
net::Bytes embed(std::span<const uint8_t> replay,const Image& image,std::span<const uint8_t> rgbaPng);
net::Bytes embed(std::span<const uint8_t> replay,net::Manifest manifest,std::span<const uint8_t> rgbaPng);
std::optional<Artwork> extract(std::span<const uint8_t> replay);
}
