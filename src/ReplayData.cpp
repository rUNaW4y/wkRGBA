#include "ReplayData.h"
#include "Png.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace wkrgba::replay {
namespace {
constexpr std::array<uint8_t,7> prefix{'w','k','R','G','B','A',0};
constexpr std::array<uint8_t,9> magicV2{'w','k','R','G','B','A',0,2,0};
constexpr size_t extensionHeaderSize=magicV2.size()+8;
constexpr size_t limit=1024ull*1024*1024;
uint32_t get(std::span<const uint8_t> b,size_t pos) {
    if(pos>b.size() || b.size()-pos<4)throw std::invalid_argument("Truncated replay length");
    return uint32_t(b[pos])|uint32_t(b[pos+1])<<8|uint32_t(b[pos+2])<<16|uint32_t(b[pos+3])<<24;
}
uint64_t get64(std::span<const uint8_t> b,size_t pos){return uint64_t(get(b,pos))|(uint64_t(get(b,pos+4))<<32);}
void put(net::Bytes& b,size_t pos,uint32_t value){for(unsigned i=0;i<4;++i)b[pos+i]=uint8_t(value>>(8*i));}
struct Layout {size_t lengthPos,start,end;};
Layout layout(std::span<const uint8_t> b) {
    if(b.size()<16 || b.size()>limit || b[0]!='W' || b[1]!='A' || b[2]!=20 || b[3])
        throw std::invalid_argument("Unsupported W:A replay header");
    const auto mapSize=get(b,4);
    if(mapSize>b.size()-12)throw std::invalid_argument("Invalid replay map size");
    const size_t lengthPos=8+size_t(mapSize),start=lengthPos+4;
    const auto settingsSize=get(b,lengthPos);
    if(settingsSize>b.size()-start)throw std::invalid_argument("Invalid replay settings size");
    return {lengthPos,start,start+settingsSize};
}
std::optional<size_t> locate(std::span<const uint8_t> bytes,Layout l) {
    auto first=bytes.begin()+l.start,last=bytes.begin()+l.end;
    auto found=std::search(first,last,prefix.begin(),prefix.end());
    if(found==last)return {};
    const auto at=size_t(found-bytes.begin());
    if(l.end-at<magicV2.size() || bytes[at+prefix.size()+1] ||
       (bytes[at+prefix.size()]!=1 && bytes[at+prefix.size()]!=2))throw std::invalid_argument("Unsupported replay RGBA version");
    return at;
}
}
std::optional<Artwork> extract(std::span<const uint8_t> bytes) {
    auto l=layout(bytes);auto at=locate(bytes,l);if(!at)return {};
    size_t pos=*at;
    const auto size=get(bytes,pos+magicV2.size()),offerSize=get(bytes,pos+magicV2.size()+4);
    if(size<extensionHeaderSize || size>l.end-pos || offerSize>size-extensionHeaderSize)throw std::invalid_argument("Invalid replay RGBA extension");
    const auto payload=bytes.subspan(pos+extensionHeaderSize+offerSize,size-extensionHeaderSize-offerSize);
    Artwork result{};
    if(bytes[pos+prefix.size()]==1) {
        const auto offer=bytes.subspan(pos+extensionHeaderSize,offerSize);
        if(offer.size()!=89 || !std::equal(offer.begin(),offer.begin()+4,"WKRG") || offer[4]!=1 || offer[5]!=1 || offer[6] || offer[7])
            throw std::invalid_argument("Invalid legacy replay offer");
        net::Manifest manifest{};manifest.generation=get64(offer,8);manifest.width=get(offer,16);manifest.height=get(offer,20);manifest.threshold=offer[24];
        std::copy_n(offer.begin()+25,32,manifest.artwork.begin());std::copy_n(offer.begin()+57,32,manifest.collision.begin());
        if(manifest.threshold!=128)throw std::invalid_argument("Unsupported replay alpha rule");
        const size_t count=checkedArea(manifest.width,manifest.height);
        if(count*4!=payload.size())throw std::invalid_argument("Invalid legacy replay RGBA length");
        result.image={manifest.width,manifest.height,{}};result.image.pixels.resize(count);
        std::memcpy(result.image.pixels.data(),payload.data(),payload.size());
        auto identified=net::identify(result.image,128,manifest.generation);
        if(identified.artwork!=manifest.artwork || identified.collision!=manifest.collision)throw std::invalid_argument("Replay RGBA integrity mismatch");
        result.png=writeRgbaPngMemory(result.image);
    } else {
        auto manifest=net::decodeOffer(bytes.subspan(pos+extensionHeaderSize,offerSize)).manifest;
        if(manifest.threshold!=128 || manifest.payloadSize!=payload.size() || net::sha256(payload)!=manifest.payload)
            throw std::invalid_argument("Invalid compressed replay RGBA payload");
        result.png.assign(payload.begin(),payload.end());result.image=readPngMemory(result.png);
        auto identified=net::identify(result.image,128,manifest.generation);
        if(identified.width!=manifest.width || identified.height!=manifest.height || identified.artwork!=manifest.artwork ||
           identified.collision!=manifest.collision)throw std::invalid_argument("Replay RGBA integrity mismatch");
    }
    if(locate(bytes,{l.lengthPos,pos+size,l.end}))throw std::invalid_argument("Multiple replay RGBA extensions");
    return result;
}
net::Bytes embed(std::span<const uint8_t> bytes,const Image& image,std::span<const uint8_t> rgbaPng) {
    auto manifest=net::identify(image,128,1);manifest.payloadSize=uint32_t(rgbaPng.size());manifest.payload=net::sha256(rgbaPng);
    return embed(bytes,std::move(manifest),rgbaPng);
}
net::Bytes embed(std::span<const uint8_t> bytes,net::Manifest manifest,std::span<const uint8_t> rgbaPng) {
    auto l=layout(bytes);
    if(locate(bytes,l))throw std::invalid_argument("Replay already contains RGBA data");
    if(rgbaPng.size()<33 || rgbaPng.size()>128u*1024u*1024u)throw std::invalid_argument("Replay PNG outside limits");
    if(manifest.threshold!=128 || manifest.payloadSize!=rgbaPng.size() || manifest.payload!=net::sha256(rgbaPng))
        throw std::invalid_argument("Replay PNG does not match manifest");
    const auto offer=net::encode(net::Offer{manifest});
    const size_t size=extensionHeaderSize+offer.size()+rgbaPng.size();
    if(size>limit-bytes.size() || l.end-l.start+size>UINT32_MAX)throw std::invalid_argument("Replay extension exceeds limit");
    net::Bytes result;result.reserve(bytes.size()+size);
    result.insert(result.end(),bytes.begin(),bytes.begin()+l.end);
    result.insert(result.end(),magicV2.begin(),magicV2.end());result.resize(result.size()+8);
    put(result,l.end+magicV2.size(),uint32_t(size));put(result,l.end+magicV2.size()+4,uint32_t(offer.size()));
    result.insert(result.end(),offer.begin(),offer.end());
    result.insert(result.end(),rgbaPng.begin(),rgbaPng.end());
    result.insert(result.end(),bytes.begin()+l.end,bytes.end());
    put(result,l.lengthPos,uint32_t(l.end-l.start+size));
    return result;
}
}
