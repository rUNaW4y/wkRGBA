#include "Png.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace wkrgba {
uint8_t nativeProxyIndex(Pixel pixel,uint8_t threshold) noexcept {
    if(pixel.a<threshold)return 0;
    // The proxy is collision/editor data only; the lossless RGBA artwork is
    // rendered separately.  Keep it at 64 terrain colours so W:A has palette
    // slots available for borders, holes and generated placement data.
    return uint8_t(1+(unsigned(pixel.r)>>6)*16+(unsigned(pixel.g)>>6)*4+(unsigned(pixel.b)>>6));
}
Pixel nativeProxyColour(Pixel pixel) noexcept {
    const auto r=unsigned(pixel.r)>>6,g=unsigned(pixel.g)>>6,b=unsigned(pixel.b)>>6;
    return {uint8_t(r*64+32),uint8_t(g*64+32),uint8_t(b*64+32),255};
}
uint32_t nativeProxyTopClearance(const Image& image,uint8_t threshold) noexcept {
    // Maps which already contain transparent space retain only W:A's normal
    // border gutter.  Completely opaque maps need a worm-sized air strip or
    // automatic placement cannot find a legal volume and becomes manual.
    for(const auto pixel:image.pixels)if(pixel.a<threshold)return nativeProxyBorder;
    auto rows=(std::min)(nativeProxyFullMapTopClearance,(std::max)(nativeProxyBorder,image.height/2));
    // W:A only accepts custom-map dimensions divisible by eight. Keep the
    // requested clearance as a minimum and round it up just enough for the
    // final height to satisfy that invariant (60 becomes 64 for 8-aligned maps).
    rows+=(8-((image.height+rows)&7))&7;
    return rows;
}
Image addTransparentTop(const Image& image,uint32_t rows) {
    image.validate();if(!rows)return image;
    if(rows>maxDimension-image.height)throw std::invalid_argument("Top padding exceeds map height limit");
    Image padded{image.width,image.height+rows,{}};
    padded.pixels.resize(checkedArea(padded.width,padded.height));
    std::copy(image.pixels.begin(),image.pixels.end(),padded.pixels.begin()+size_t(rows)*image.width);
    return padded;
}
bool matchesNativeProxy(const Image& source,const Image& native) {
    source.validate();native.validate();
    if(source.width!=native.width || source.height!=native.height)return false;
    const auto topClearance=nativeProxyTopClearance(source);
    uint64_t solid{},matched{};
    for(uint32_t y=0;y<source.height;++y) for(uint32_t x=0;x<source.width;++x) {
        const auto i=size_t(y)*source.width+x;
        const auto p=source.pixels[i],n=native.pixels[i];
        // W:A can add indestructible borders in pixels that were transparent
        // in the RGBA source.  Those additions are legitimate and must not
        // break host/client association with the lossless artwork.
        if(p.a<128 || x<nativeProxyBorder || y<topClearance ||
           x+nativeProxyBorder>=source.width || y+nativeProxyBorder>=source.height)continue;
        if(n.a<128)continue;
        ++solid;
        const auto expected=nativeProxyColour(p);
        matched+=n.r==expected.r && n.g==expected.g && n.b==expected.b;
    }
    return solid && matched*100>=solid*95;
}
namespace {
using Microsoft::WRL::ComPtr;
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " failed (HRESULT " + std::to_string(uint32_t(hr)) + ")");
}
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComScope() { if (hr != RPC_E_CHANGED_MODE) check(hr, "COM initialization"); }
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
ComPtr<IWICImagingFactory> factory() {
    ComPtr<IWICImagingFactory> f;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)), "WIC factory");
    return f;
}
uint32_t bigEndian(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t crc32(std::span<const uint8_t> data) {
    static const auto table = [] {
        std::array<uint32_t,256> result{};
        for (uint32_t i=0;i<256;++i) {
            auto v=i;
            for (int bit=0;bit<8;++bit) v=(v>>1)^((v&1)?0xEDB88320u:0);
            result[i]=v;
        }
        return result;
    }();
    uint32_t crc=0xFFFFFFFF;
    for(auto byte:data) crc=table[(crc^byte)&255]^(crc>>8);
    return ~crc;
}
void validateChunks(std::span<const uint8_t> bytes) {
    size_t pos=8;
    bool idat=false, endedIdat=false;
    while(pos<bytes.size()) {
        if(bytes.size()-pos<12) throw std::runtime_error("Truncated PNG chunk");
        const size_t size=bigEndian(bytes.data()+pos);
        if(size>bytes.size()-pos-12) throw std::runtime_error("Invalid PNG chunk length");
        const std::string type(reinterpret_cast<const char*>(bytes.data()+pos+4),4);
        for (const char c:type) if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z'))) throw std::runtime_error("Invalid PNG chunk type");
        if (type[2]>='a'&&type[2]<='z') throw std::runtime_error("Invalid PNG reserved chunk bit");
        if(crc32(bytes.subspan(pos+4,size+4))!=bigEndian(bytes.data()+pos+8+size)) throw std::runtime_error("PNG chunk CRC mismatch");
        if(type=="IHDR" && pos!=8) throw std::runtime_error("Duplicate PNG header");
        if(type=="acTL"||type=="fcTL"||type=="fdAT") throw std::runtime_error("Animated PNG maps are not supported");
        if (type=="IDAT") {
            if(endedIdat) throw std::runtime_error("Non-contiguous PNG image data");
            idat=true;
        } else if(idat) endedIdat=true;
        if(type=="IEND") {
            if(size||!idat||pos+12!=bytes.size()) throw std::runtime_error("Invalid PNG end");
            return;
        }
        if (!(type[0]&32) && type!="IHDR" && type!="PLTE" && type!="IDAT") throw std::runtime_error("Unknown critical PNG chunk");
        pos+=size+12;
    }
    throw std::runtime_error("Missing PNG end");
}
std::optional<std::vector<uint8_t>> chunkPayload(std::span<const uint8_t> png,const char (&wanted)[5]) {
    validateChunks(png);size_t pos=8;
    while(pos<png.size()) {
        const size_t size=bigEndian(png.data()+pos);
        if(std::memcmp(png.data()+pos+4,wanted,4)==0)
            return std::vector<uint8_t>(png.begin()+pos+8,png.begin()+pos+8+size);
        if(std::memcmp(png.data()+pos+4,"IEND",4)==0)break;
        pos+=size+12;
    }
    return std::nullopt;
}
std::vector<uint8_t> insertChunkBeforePalette(std::span<const uint8_t> png,const char (&type)[5],std::span<const uint8_t> payload) {
    validateChunks(png);
    // Updating waLV must replace the old editor state. Appending another waLV
    // leaves the stale chunk first, and W:A consequently sees old border and
    // hole flags while wkRGBA believes it sent the new state.
    std::vector<uint8_t> clean;clean.reserve(png.size());clean.insert(clean.end(),png.begin(),png.begin()+8);
    for(size_t at=8;at<png.size();) {
        const size_t size=bigEndian(png.data()+at);const auto chunkBytes=size+12;
        if(std::memcmp(png.data()+at+4,type,4)!=0)
            clean.insert(clean.end(),png.begin()+at,png.begin()+at+chunkBytes);
        at+=chunkBytes;
    }
    validateChunks(clean);size_t pos=8;
    while(pos<clean.size()) {
        const size_t size=bigEndian(clean.data()+pos);
        const std::string current(reinterpret_cast<const char*>(clean.data()+pos+4),4);
        if(current=="PLTE"||current=="IDAT"||current=="IEND")break;
        pos+=size+12;
    }
    std::vector<uint8_t> result;result.reserve(clean.size()+payload.size()+12);
    result.insert(result.end(),clean.begin(),clean.begin()+pos);
    auto appendBig=[&](uint32_t value){result.push_back(uint8_t(value>>24));result.push_back(uint8_t(value>>16));result.push_back(uint8_t(value>>8));result.push_back(uint8_t(value));};
    appendBig(uint32_t(payload.size()));const auto crcBegin=result.size();
    result.insert(result.end(),type,type+4);result.insert(result.end(),payload.begin(),payload.end());
    appendBig(crc32(std::span(result).subspan(crcBegin,4+payload.size())));
    result.insert(result.end(),clean.begin()+pos,clean.end());validateChunks(result);return result;
}
}
EncodedPng readRgbaPngWithBytes(const std::filesystem::path& path) {
    // Validate dimensions before asking a codec to allocate a decoded image.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open PNG");
    const auto fileSize=file.tellg();
    if (fileSize < 33 || fileSize > 128ll * 1024 * 1024) throw std::runtime_error("PNG outside 33 byte..128 MiB input limits");
    file.seekg(0);
    std::vector<uint8_t> encoded(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(encoded.data()),encoded.size())) throw std::runtime_error("Cannot read complete PNG");
    auto image=readPngMemory(encoded);
    return {std::move(image),std::move(encoded)};
}
Image readRgbaPng(const std::filesystem::path& path) {return std::move(readRgbaPngWithBytes(path).image);}
Image readPngMemory(std::span<const uint8_t> bytes,bool rgbaOnly) {
    if(bytes.size()<33 || bytes.size()>128ull*1024*1024)throw std::runtime_error("PNG outside input limits");
    auto header=bytes.first(33);
    constexpr std::array<uint8_t, 8> signature{137,80,78,71,13,10,26,10};
    if (!std::equal(signature.begin(), signature.end(), header.begin()) || bigEndian(header.data()+8) != 13 ||
        std::string(reinterpret_cast<const char*>(header.data()+12),4) != "IHDR") throw std::runtime_error("Invalid PNG header");
    if (header[24] != 8 || (header[25] != 6 && (rgbaOnly || header[25]!=3))) throw std::runtime_error("Expected RGBA32 PNG or explicitly allowed indexed native PNG");
    if (header[26] || header[27] || header[28] > 1) throw std::runtime_error("Invalid PNG encoding");
    Image image{bigEndian(header.data()+16), bigEndian(header.data()+20), {}};
    const auto count = checkedArea(image.width, image.height);
    std::vector<uint8_t> encoded(bytes.begin(),bytes.end());
    validateChunks(encoded);
    ComScope scope;
    auto f = factory();
    ComPtr<IWICStream> stream;
    check(f->CreateStream(&stream), "Input stream");
    check(stream->InitializeFromMemory(encoded.data(),DWORD(encoded.size())), "Input bytes");
    ComPtr<IWICBitmapDecoder> decoder;
    check(f->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder), "PNG decoder");
    GUID format{};
    check(decoder->GetContainerFormat(&format), "PNG container");
    if (format != GUID_ContainerFormatPng) throw std::runtime_error("Not a PNG container");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "PNG frame");
    UINT w{}, h{};
    check(frame->GetSize(&w, &h), "PNG dimensions");
    if (w != image.width || h != image.height) throw std::runtime_error("PNG dimensions changed during loading");
    ComPtr<IWICFormatConverter> converter;
    check(f->CreateFormatConverter(&converter), "RGBA converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "RGBA conversion");
    image.pixels.resize(count);
    check(converter->CopyPixels(nullptr, image.width * 4, UINT(count * 4), reinterpret_cast<BYTE*>(image.pixels.data())), "RGBA pixels");
    return image;
}
bool pngDimensionsMatch(std::span<const uint8_t> bytes,uint32_t width,uint32_t height) {
    constexpr std::array<uint8_t,8> signature{137,80,78,71,13,10,26,10};
    return bytes.size()>=33 && std::equal(signature.begin(),signature.end(),bytes.begin()) &&
        bigEndian(bytes.data()+8)==13 && std::string(reinterpret_cast<const char*>(bytes.data()+12),4)=="IHDR" &&
        bigEndian(bytes.data()+16)==width && bigEndian(bytes.data()+20)==height;
}
std::vector<uint8_t> attachPrivatePayload(std::span<const uint8_t> png,std::span<const uint8_t> payload) {
    if(payload.empty() || payload.size()>128u*1024u*1024u)throw std::invalid_argument("Private PNG payload outside limits");
    validateChunks(png);size_t pos=8,iend=0;
    while(pos<png.size()) {
        const size_t size=bigEndian(png.data()+pos);const std::string type(reinterpret_cast<const char*>(png.data()+pos+4),4);
        if(type=="wkRg")throw std::invalid_argument("PNG already contains wkRGBA payload");
        if(type=="IEND"){iend=pos;break;}pos+=size+12;
    }
    if(!iend)throw std::invalid_argument("PNG end not found");
    std::vector<uint8_t> result;result.reserve(png.size()+payload.size()+12);
    result.insert(result.end(),png.begin(),png.begin()+iend);
    auto appendBig=[&](uint32_t value){result.push_back(uint8_t(value>>24));result.push_back(uint8_t(value>>16));result.push_back(uint8_t(value>>8));result.push_back(uint8_t(value));};
    appendBig(uint32_t(payload.size()));const size_t crcBegin=result.size();
    result.insert(result.end(),{'w','k','R','g'});result.insert(result.end(),payload.begin(),payload.end());
    appendBig(crc32(std::span(result).subspan(crcBegin,4+payload.size())));
    result.insert(result.end(),png.begin()+iend,png.end());validateChunks(result);return result;
}
std::optional<std::vector<uint8_t>> extractPrivatePayload(std::span<const uint8_t> png) {
    validateChunks(png);size_t pos=8;std::optional<std::vector<uint8_t>> result;
    while(pos<png.size()) {
        const size_t size=bigEndian(png.data()+pos);const std::string type(reinterpret_cast<const char*>(png.data()+pos+4),4);
        if(type=="wkRg") {
            if(result)throw std::invalid_argument("Multiple wkRGBA PNG payloads");
            result=std::vector<uint8_t>(png.begin()+pos+8,png.begin()+pos+8+size);
        }
        if(type=="IEND")break;pos+=size+12;
    }
    return result;
}
std::optional<std::vector<uint8_t>> extractWaLevelMetadata(std::span<const uint8_t> png) {
    auto result=chunkPayload(png,"waLV");
    if(result && (result->size()<40 || result->size()>42))return std::nullopt;
    return result;
}
std::vector<uint8_t> attachWaLevelMetadata(std::span<const uint8_t> png,std::span<const uint8_t> metadata) {
    if(metadata.size()<40||metadata.size()>42)throw std::invalid_argument("Invalid W:A level metadata size");
    return insertChunkBeforePalette(png,"waLV",metadata);
}
std::vector<uint8_t> resizeIndexedPngTop(std::span<const uint8_t> bytes,uint32_t targetHeight,uint32_t retainedTopRows) {
    if(bytes.size()<33 || bytes[24]!=8 || bytes[25]!=3)throw std::invalid_argument("Expected indexed PNG");
    validateChunks(bytes);ComScope scope;auto f=factory();
    std::vector<uint8_t> owned(bytes.begin(),bytes.end());
    ComPtr<IWICStream> input;check(f->CreateStream(&input),"Indexed resize input stream");
    check(input->InitializeFromMemory(owned.data(),DWORD(owned.size())),"Indexed resize input bytes");
    ComPtr<IWICBitmapDecoder> decoder;check(f->CreateDecoderFromStream(input.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder),"Indexed resize decoder");
    ComPtr<IWICBitmapFrameDecode> source;check(decoder->GetFrame(0,&source),"Indexed resize frame");
    UINT width{},height{};check(source->GetSize(&width,&height),"Indexed resize dimensions");
    if(!width||!height||!targetHeight||targetHeight>maxDimension)throw std::invalid_argument("Invalid indexed resize dimensions");
    WICPixelFormatGUID sourceFormat{};check(source->GetPixelFormat(&sourceFormat),"Indexed resize format");
    if(sourceFormat!=GUID_WICPixelFormat8bppIndexed)throw std::invalid_argument("Indexed PNG decoder changed format");
    ComPtr<IWICPalette> palette;check(f->CreatePalette(&palette),"Indexed resize palette");
    check(source->CopyPalette(palette.Get()),"Indexed resize copy palette");
    std::vector<BYTE> sourcePixels(size_t(width)*height);
    check(source->CopyPixels(nullptr,width,UINT(sourcePixels.size()),sourcePixels.data()),"Indexed resize source pixels");
    std::vector<BYTE> target(size_t(width)*targetHeight,0);
    if(targetHeight>=height) {
        const auto added=targetHeight-height;
        std::copy(sourcePixels.begin(),sourcePixels.end(),target.begin()+size_t(added)*width);
    } else {
        const auto removed=height-targetHeight;
        std::copy(sourcePixels.begin()+size_t(removed)*width,sourcePixels.end(),target.begin());
        const auto rows=(std::min)({retainedTopRows,targetHeight,height});
        if(rows)std::copy_n(sourcePixels.begin(),size_t(rows)*width,target.begin());
    }
    ComPtr<IStream> memory;check(CreateStreamOnHGlobal(nullptr,TRUE,&memory),"Indexed resize memory stream");
    ComPtr<IWICBitmapEncoder> encoder;check(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"Indexed resize encoder");
    check(encoder->Initialize(memory.Get(),WICBitmapEncoderNoCache),"Indexed resize encoder init");
    ComPtr<IWICBitmapFrameEncode> frame;check(encoder->CreateNewFrame(&frame,nullptr),"Indexed resize output frame");
    check(frame->Initialize(nullptr),"Indexed resize output init");check(frame->SetSize(width,targetHeight),"Indexed resize output dimensions");
    auto format=GUID_WICPixelFormat8bppIndexed;check(frame->SetPixelFormat(&format),"Indexed resize output format");
    if(format!=GUID_WICPixelFormat8bppIndexed)throw std::runtime_error("Indexed resize encoder changed format");
    check(frame->SetPalette(palette.Get()),"Indexed resize output palette");
    check(frame->WritePixels(targetHeight,width,UINT(target.size()),target.data()),"Indexed resize output pixels");
    check(frame->Commit(),"Indexed resize frame commit");check(encoder->Commit(),"Indexed resize commit");
    STATSTG stat{};check(memory->Stat(&stat,STATFLAG_NONAME),"Indexed resize size");
    LARGE_INTEGER zero{};check(memory->Seek(zero,STREAM_SEEK_SET,nullptr),"Indexed resize rewind");
    std::vector<uint8_t> result(size_t(stat.cbSize.QuadPart));ULONG read{};
    check(memory->Read(result.data(),ULONG(result.size()),&read),"Indexed resize read");
    if(read!=result.size())throw std::runtime_error("Incomplete indexed resized PNG");
    if(auto metadata=extractWaLevelMetadata(bytes))result=attachWaLevelMetadata(result,*metadata);
    return result;
}
bool matchesNativeProxyPng(const Image& original,std::span<const uint8_t> bytes) {
    original.validate();
    if(!pngDimensionsMatch(bytes,original.width,original.height) || bytes[24]!=8 || bytes[25]!=3)return false;
    validateChunks(bytes);ComScope scope;auto f=factory();
    std::vector<uint8_t> owned(bytes.begin(),bytes.end());
    ComPtr<IWICStream> stream;check(f->CreateStream(&stream),"Native proxy stream");
    check(stream->InitializeFromMemory(owned.data(),DWORD(owned.size())),"Native proxy bytes");
    ComPtr<IWICBitmapDecoder> decoder;check(f->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnDemand,&decoder),"Native proxy decoder");
    ComPtr<IWICBitmapFrameDecode> frame;check(decoder->GetFrame(0,&frame),"Native proxy frame");
    ComPtr<IWICFormatConverter> converter;check(f->CreateFormatConverter(&converter),"Native proxy converter");
    check(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"Native proxy RGBA conversion");
    constexpr uint32_t blockRows=64;std::vector<Pixel> block(size_t(original.width)*blockRows);
    uint64_t solid{},matched{};const auto topClearance=nativeProxyTopClearance(original);
    for(uint32_t y0=0;y0<original.height;y0+=blockRows){
        const auto rows=(std::min)(blockRows,original.height-y0);
        WICRect rect{0,int(y0),int(original.width),int(rows)};
        check(converter->CopyPixels(&rect,original.width*4,UINT(size_t(original.width)*rows*4),reinterpret_cast<BYTE*>(block.data())),"Native proxy rows");
        for(uint32_t row=0;row<rows;++row)for(uint32_t x=0;x<original.width;++x){
            const auto y=y0+row,at=size_t(y)*original.width+x;
            const auto p=original.pixels[at],n=block[size_t(row)*original.width+x];
            // Landscape::InitBorders stamps an exact eight-pixel band.  It may
            // overwrite opaque source pixels, so exclude that band from map
            // identity instead of weakening the comparison over the interior.
            if(p.a<128 || x<nativeProxyBorder || y<topClearance ||
               x+nativeProxyBorder>=original.width || y+nativeProxyBorder>=original.height)continue;
            // Generated holes and destruction turn proxy terrain into the
            // transparent palette entry. They are valid gameplay mutations,
            // so compare identity only where native terrain still exists.
            if(n.a<128)continue;
            ++solid;
            const auto expected=nativeProxyColour(p);
            matched+=n.r==expected.r && n.g==expected.g && n.b==expected.b;
        }
    }
    return solid && matched*100>=solid*95;
}
void writeRgbaPng(const std::filesystem::path& path, const Image& image) {
    image.validate();
    ComScope scope;
    auto f = factory();
    ComPtr<IWICStream> stream;
    check(f->CreateStream(&stream), "Output stream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Output file");
    ComPtr<IWICBitmapEncoder> encoder;
    check(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG encoder initialization");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr), "Output frame");
    check(frame->Initialize(nullptr), "Output frame initialization");
    check(frame->SetSize(image.width, image.height), "Output dimensions");
    // WIC's PNG encoder natively accepts BGRA. Reorder bytes, never quantize
    // or premultiply: even RGB under alpha zero must survive a round trip.
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format), "Output format");
    if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG encoder cannot preserve alpha");
    std::vector<Pixel> row(image.width);
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            auto p = image.pixels[size_t(y) * image.width + x];
            row[x] = {p.b,p.g,p.r,p.a};
        }
        check(frame->WritePixels(1, image.width * 4, image.width * 4, reinterpret_cast<BYTE*>(row.data())), "Output pixels");
    }
    check(frame->Commit(), "Output frame commit");
    check(encoder->Commit(), "Output commit");
}
std::vector<uint8_t> writeRgbaPngMemory(const Image& image) {
    image.validate();
    ComScope scope;auto f=factory();
    ComPtr<IStream> stream;check(CreateStreamOnHGlobal(nullptr,TRUE,&stream),"Memory PNG stream");
    ComPtr<IWICBitmapEncoder> encoder;check(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"Memory PNG encoder");
    check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Memory PNG encoder initialization");
    ComPtr<IWICBitmapFrameEncode> frame;check(encoder->CreateNewFrame(&frame,nullptr),"Memory PNG frame");
    check(frame->Initialize(nullptr),"Memory PNG frame init");check(frame->SetSize(image.width,image.height),"Memory PNG size");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;check(frame->SetPixelFormat(&format),"Memory PNG format");
    if(format!=GUID_WICPixelFormat32bppBGRA)throw std::runtime_error("PNG encoder cannot preserve alpha");
    std::vector<Pixel> row(image.width);
    for(uint32_t y=0;y<image.height;++y){
        for(uint32_t x=0;x<image.width;++x){auto p=image.pixels[size_t(y)*image.width+x];row[x]={p.b,p.g,p.r,p.a};}
        check(frame->WritePixels(1,image.width*4,image.width*4,reinterpret_cast<BYTE*>(row.data())),"Memory PNG pixels");
    }
    check(frame->Commit(),"Memory PNG frame commit");check(encoder->Commit(),"Memory PNG commit");
    STATSTG stat{};check(stream->Stat(&stat,STATFLAG_NONAME),"Memory PNG size query");
    if(stat.cbSize.QuadPart<33 || stat.cbSize.QuadPart>128ll*1024*1024)throw std::runtime_error("Encoded PNG outside limits");
    LARGE_INTEGER zero{};check(stream->Seek(zero,STREAM_SEEK_SET,nullptr),"Memory PNG rewind");
    std::vector<uint8_t> bytes(size_t(stat.cbSize.QuadPart));ULONG got{};
    check(stream->Read(bytes.data(),ULONG(bytes.size()),&got),"Memory PNG read");
    if(got!=bytes.size())throw std::runtime_error("Incomplete memory PNG");
    return bytes;
}
void writeNativePng(const std::filesystem::path& path,const Image& image,uint8_t threshold,
                    std::span<const uint8_t> waLevelMetadata,uint32_t classificationTopRows) {
    image.validate();
    if(!threshold) throw std::invalid_argument("Invalid alpha threshold");
    ComScope scope; auto f=factory();
    ComPtr<IWICStream> stream; check(f->CreateStream(&stream),"Native PNG stream");
    check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"Native PNG file");
    ComPtr<IWICBitmapEncoder> encoder;
    check(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"Native PNG encoder");
    check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"Native encoder init");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame,nullptr),"Native frame");
    check(frame->Initialize(nullptr),"Native frame init");
    check(frame->SetSize(image.width,image.height),"Native size");
    auto format=GUID_WICPixelFormat8bppIndexed;
    check(frame->SetPixelFormat(&format),"Native format");
    if(format!=GUID_WICPixelFormat8bppIndexed) throw std::runtime_error("Native PNG encoder changed format");
    // W:A's native map convention is an opaque black palette entry at index
    // zero (empty terrain), not PNG alpha transparency.  Keeping every palette
    // entry opaque also avoids sending its loader down the RGBA/transparency
    // path used when it classifies a map for automatic worm placement.
    // CMapEditor::BakeMap rejects hole generation when its palette count is
    // above 0x61 (97).  A full 112-entry proxy therefore disables holes and
    // automatic placement and can force border edits through W:A's lossy
    // palette path.  One black entry plus a 4x4x4 RGB cube is sufficient for
    // collision identity and leaves 47 of W:A's 112 map slots available.
    std::array<WICColor,65> colours{};
    colours[0]=0xFF000000u;
    for(unsigned bucket=0;bucket<64;++bucket) {
        const unsigned r=bucket/16,rem=bucket%16,g=rem/4,b=rem%4,index=bucket+1;
        colours[index]=0xFF000000u|((r*64+32)<<16)|((g*64+32)<<8)|(b*64+32);
    }
    ComPtr<IWICPalette> palette; check(f->CreatePalette(&palette),"Native palette");
    check(palette->InitializeCustom(colours.data(),UINT(colours.size())),"Native colours");
    check(frame->SetPalette(palette.Get()),"Native frame palette");
    std::vector<BYTE> row(image.width);
    for(uint32_t y=0;y<image.height;++y) {
        for(uint32_t x=0;x<image.width;++x) {
            auto p=image.pixels[size_t(y)*image.width+x];
            // A completely solid custom map is classified before the game
            // builds current.thm/land.dat.  Reserve air in the disposable
            // editor proxy so W:A keeps automatic placement enabled.  The
            // canonical native map cached by Loader is encoded separately
            // without this strip, so gameplay receives the complete artwork
            // shifted below newly prepended clearance rather than a crop.
            row[x]=y<classificationTopRows?0:nativeProxyIndex(p,threshold);
        }
        check(frame->WritePixels(1,image.width,image.width,row.data()),"Native pixels");
    }
    check(frame->Commit(),"Native frame commit"); check(encoder->Commit(),"Native PNG commit");
    palette.Reset();frame.Reset();encoder.Reset();stream.Reset();

    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)throw std::runtime_error("Cannot reopen native PNG for W:A metadata");
    const auto length=input.tellg();if(length<33)throw std::runtime_error("Native PNG metadata input is truncated");
    std::vector<uint8_t> png(static_cast<size_t>(length));input.seekg(0);
    if(!input.read(reinterpret_cast<char*>(png.data()),length))throw std::runtime_error("Cannot read native PNG metadata input");
    input.close();
    // Preserve waLV only when the source actually supplied it.  Inventing a
    // generated-land header for an imported image makes W:A classify the map
    // as generated terrain and interferes with editor holes and placement.
    auto augmented=waLevelMetadata.size()>=40&&waLevelMetadata.size()<=42
        ?insertChunkBeforePalette(png,"waLV",waLevelMetadata):std::move(png);
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    if(!output || !output.write(reinterpret_cast<const char*>(augmented.data()),augmented.size()))
        throw std::runtime_error("Cannot write native PNG W:A metadata");
}
}
