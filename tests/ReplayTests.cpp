#include "ReplayData.h"
#include "Png.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace wkrgba;
void require(bool value){if(!value)throw std::runtime_error("Replay assertion failed");}
template<class F>void rejects(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Malformed replay accepted");}
void put(net::Bytes& bytes,size_t pos,uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes[pos+i]=uint8_t(value>>(8*i));}
net::Bytes legacyReplay(const net::Bytes& original,const Image& image){
    constexpr uint8_t magic[9]={'w','k','R','G','B','A',0,1,0};
    auto manifest=net::identify(image,128,7);net::Bytes offer(89);
    std::memcpy(offer.data(),"WKRG",4);offer[4]=1;offer[5]=1;
    put(offer,8,manifest.generation,8);put(offer,16,manifest.width,4);put(offer,20,manifest.height,4);offer[24]=manifest.threshold;
    std::copy(manifest.artwork.begin(),manifest.artwork.end(),offer.begin()+25);
    std::copy(manifest.collision.begin(),manifest.collision.end(),offer.begin()+57);
    const size_t extensionSize=17+offer.size()+image.pixels.size()*sizeof(Pixel);
    net::Bytes result(original.begin(),original.begin()+20);
    result.insert(result.end(),std::begin(magic),std::end(magic));result.resize(result.size()+8);
    put(result,29,extensionSize,4);put(result,33,offer.size(),4);
    result.insert(result.end(),offer.begin(),offer.end());
    auto pixels=reinterpret_cast<const uint8_t*>(image.pixels.data());result.insert(result.end(),pixels,pixels+image.pixels.size()*sizeof(Pixel));
    result.insert(result.end(),original.begin()+20,original.end());put(result,12,4+extensionSize,4);return result;
}
int main(){try{
    net::Bytes original{'W','A',20,0,4,0,0,0,3,0,0,0,4,0,0,0,10,20,30,40,50,60,70,80};
    Image image{640,32,{}};image.pixels.resize(640*32);
    for(size_t i=0;i<image.pixels.size();++i)image.pixels[i]={uint8_t(i),uint8_t(i>>8),uint8_t(i*7),uint8_t(i%256)};
    require(!replay::extract(original));
    auto png=writeRgbaPngMemory(image);
    auto packed=replay::embed(original,image,png);auto restored=replay::extract(packed);
    require(restored && restored->image.pixels==image.pixels && restored->png==png);
    require(packed.size()<original.size()+image.pixels.size()*4);
    require(std::equal(original.begin(),original.begin()+12,packed.begin()));
    require(std::equal(original.end()-4,original.end(),packed.end()-4));
    rejects([&]{replay::embed(packed,image,png);});
    auto corrupt=packed;corrupt[150]^=1;rejects([&]{replay::extract(corrupt);});
    corrupt=packed;corrupt[4]=255;corrupt[5]=255;corrupt[6]=255;corrupt[7]=255;rejects([&]{replay::extract(corrupt);});
    corrupt=packed;corrupt[29]=255;corrupt[30]=255;corrupt[31]=255;corrupt[32]=255;rejects([&]{replay::extract(corrupt);});
    for(size_t n:{0u,3u,10u,16u,40u,100u})rejects([&]{replay::extract(std::span(packed).first(n));});
    auto old=legacyReplay(original,image);auto legacy=replay::extract(old);
    require(legacy && legacy->image.pixels==image.pixels && readPngMemory(legacy->png).pixels==image.pixels);
    std::cout<<"Replay compressed/legacy round trip, event preservation, integrity and bounds OK\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
