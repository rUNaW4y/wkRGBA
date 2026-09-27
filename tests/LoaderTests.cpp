#include "Loader.h"
#include "Png.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <array>
#include <cstring>
#include <stdexcept>
using namespace wkrgba;
// Keep the compiler from caching an IAT target across installation in main.
__declspec(noinline) HANDLE openW(const wchar_t* path){return CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);}
__declspec(noinline) HANDLE openA(const char* path){return CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);}
__declspec(noinline) HANDLE overwrite(const wchar_t* path){return CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);}
void check(bool p,const char* m){if(!p)throw std::runtime_error(m);}
uint32_t be32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];}
bool hasChunk(const std::vector<uint8_t>& png,const char (&wanted)[5]) {
    for(size_t at=8;at+12<=png.size();) {
        const auto size=size_t(be32(png.data()+at));
        if(size>png.size()-at-12)return false;
        if(std::memcmp(png.data()+at+4,wanted,4)==0)return true;
        at+=size+12;
    }
    return false;
}
std::vector<uint8_t> readHandle(HANDLE h) {
    check(h!=INVALID_HANDLE_VALUE,"File open failed");
    struct Close{HANDLE h;~Close(){CloseHandle(h);}}close{h};
    DWORD size=GetFileSize(h,nullptr);check(size!=INVALID_FILE_SIZE && size<1024*1024,"Unexpected fixture size");
    std::vector<uint8_t> data(size);DWORD got{};
    check(ReadFile(h,data.data(),size,&got,nullptr)&&got==size,"File read failed");return data;
}
void verifyNative(std::vector<uint8_t>& encoded,const Image& original,bool editorEdges=false) {
    using Microsoft::WRL::ComPtr;
    check(encoded.size()>26 && encoded[25]==3,"Native reader must see indexed PNG");
    check(!hasChunk(encoded,"tRNS"),"Native proxy must use W:A opaque-black emptiness, not PNG transparency");
    ComPtr<IWICImagingFactory> f;
    check(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f))),"WIC factory");
    ComPtr<IWICStream>s;f->CreateStream(&s);s->InitializeFromMemory(encoded.data(),DWORD(encoded.size()));
    ComPtr<IWICBitmapDecoder>d;check(SUCCEEDED(f->CreateDecoderFromStream(s.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&d)),"Native decode");
    ComPtr<IWICBitmapFrameDecode>frame;d->GetFrame(0,&frame);
    ComPtr<IWICPalette>pal;f->CreatePalette(&pal);check(SUCCEEDED(frame->CopyPalette(pal.Get())),"Palette");
    UINT count{};pal->GetColorCount(&count);check(count==65,"Native proxy must reserve W:A palette slots for borders and holes");
    ComPtr<IWICFormatConverter>c;f->CreateFormatConverter(&c);
    check(SUCCEEDED(c->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"Native conversion");
    std::vector<Pixel>pixels(original.pixels.size());
    check(SUCCEEDED(c->CopyPixels(nullptr,original.width*4,UINT(pixels.size()*4),reinterpret_cast<BYTE*>(pixels.data()))),"Native pixels");
    for(size_t i=0;i<pixels.size();++i) {
        check(pixels[i].a==255,"Native proxy palette must remain fully opaque");
        const auto x=uint32_t(i%original.width),y=uint32_t(i/original.width);
        const bool reserved=editorEdges && (x<nativeProxyBorder || y<nativeProxyBorder ||
            x+nativeProxyBorder>=original.width || y+nativeProxyBorder>=original.height);
        check((pixels[i].r!=0||pixels[i].g!=0||pixels[i].b!=0)==(!reserved&&original.pixels[i].a>=128),
              "Proxy black collision mismatch");
    }
}
int main(){
    auto root=std::filesystem::temp_directory_path()/("wkRGBA-loader-test-"+std::to_string(GetCurrentProcessId()));
    try {
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        std::filesystem::create_directories(root/"SavedLevels");
        Image original{256,96,{}};
        for(unsigned i=0;i<256*96;++i)original.pixels.push_back({uint8_t(i),uint8_t(i>>8),17,uint8_t(i)});
        original.pixels[0]={0,0,0,255};
        auto path=root/"SavedLevels"/"rgba.png";
        writeRgbaPng(path,original);writeRgbaPng(root/"outside.png",original);
        Image opaque{256,256,std::vector<Pixel>(256*256,Pixel{120,80,40,255})};
        const auto padding=nativeProxyTopClearance(opaque);auto padded=addTransparentTop(opaque,padding);
        check(padding>=nativeProxyFullMapTopClearance && (opaque.height+padding)%8==0,
              "Top clearance must meet its minimum and preserve W:A's eight-pixel dimension alignment");
        check(padded.height==opaque.height+padding,"Top clearance must extend map dimensions");
        check(padded.pixels[size_t(padding)*padded.width+128]==opaque.pixels[128],
              "Top clearance must shift, not crop, original artwork");
        auto opaqueProxy=root/"opaque-proxy.png";writeNativePng(opaqueProxy,padded,128);
        auto opaqueDecoded=readPngMemory(readHandle(openW(opaqueProxy.c_str())),false);
        check(opaqueDecoded.pixels[size_t(padding-1)*opaque.width+128].r==0,
              "Fully opaque proxy must reserve worm-sized top clearance");
        check(opaqueDecoded.pixels[size_t(padding)*opaque.width+128].r!=0,
              "Top clearance must end before playable terrain");
        check(opaqueDecoded.pixels[size_t(padding)*opaque.width].r!=0,
              "Proxy must not carve a left edge gutter");
        check(opaqueDecoded.pixels.back().r!=0,
              "Proxy must not carve right or bottom edge gutters");
        auto legacyProxy=root/"legacy-proxy.png";writeNativePng(legacyProxy,opaque,128,{},64);
        auto legacyDecoded=readPngMemory(readHandle(openW(legacyProxy.c_str())),false);
        check(legacyDecoded.width==opaque.width&&legacyDecoded.height==opaque.height,
              "Legacy placement clearance must remain within W:A's accepted native dimensions");
        check(legacyDecoded.pixels[size_t(63)*opaque.width+128].r==0 &&
              legacyDecoded.pixels[size_t(64)*opaque.width+128].r!=0,
              "Legacy compatibility proxy must reserve exactly 64 placement rows");
        auto paddedBytes=readHandle(openW(opaqueProxy.c_str()));
        // Simulate W:A's eight-pixel top border on the padded indexed map,
        // then remove placement clearance. The palette proxy below the border
        // must remain byte-for-byte equivalent to the original artwork proxy.
        auto croppedBytes=resizeIndexedPngTop(paddedBytes,opaque.height,nativeProxyBorder);
        auto cropped=readPngMemory(croppedBytes,false);
        check(cropped.width==opaque.width&&cropped.height==opaque.height,"Bordered final map dimensions must equal artwork");
        check(cropped.pixels[size_t(nativeProxyBorder)*cropped.width+128].r!=0,
              "Cropping placement clearance must retain artwork immediately below the native top border");
        auto before=readHandle(openW(path.c_str()));
        std::filesystem::create_directories(root/"DATA");
        std::array<uint8_t,40> editorLevel{};
        editorLevel[16]=1; // no indestructible borders
        auto editorPng=attachWaLevelMetadata(writeRgbaPngMemory(original),editorLevel);
        {std::ofstream theme(root/"DATA"/"current.thm",std::ios::binary);theme.write(reinterpret_cast<const char*>(editorPng.data()),editorPng.size());}
        check(loader::installTestImports(GetModuleHandleW(nullptr),root/"SavedLevels"),"Install real host IAT hooks");
        auto nativeW=readHandle(openW(path.c_str()));
        verifyNative(nativeW,original);
        check(extractWaLevelMetadata(nativeW)==std::optional<std::vector<uint8_t>>(
                  std::vector<uint8_t>(editorLevel.begin(),editorLevel.end())),
              "RGBA editor proxy must carry the active native custom-level state");
        auto firstImport=loader::lastImport();
        check(bool(firstImport),"RGBA import was not published");
        auto decodedNative=readPngMemory(nativeW,false);
        check(matchesNativeProxy(original,decodedNative),"Decoded native map must bind to its RGBA source");
        auto borderedNative=decodedNative;
        for(size_t i=0;i<borderedNative.pixels.size();++i)
            if(original.pixels[i].a<128)borderedNative.pixels[i]={255,192,0,255};
        check(matchesNativeProxy(original,borderedNative),"Native additions in source-transparent pixels must preserve RGBA binding");
        auto opaqueEdgeBorder=decodedNative;
        for(uint32_t y=0;y<original.height;++y)for(uint32_t x=0;x<original.width;++x)
            if(x<8||y<8||x+8>=original.width||y+8>=original.height)
                opaqueEdgeBorder.pixels[size_t(y)*original.width+x]={255,192,0,255};
        check(matchesNativeProxy(original,opaqueEdgeBorder),"Native 8-pixel borders over opaque artwork must preserve RGBA binding");
        auto nativeWithHoles=decodedNative;
        for(uint32_t y=24;y<48;++y)for(uint32_t x=96;x<144;++x)
            nativeWithHoles.pixels[size_t(y)*original.width+x]={0,0,0,0};
        check(matchesNativeProxy(original,nativeWithHoles),"Generated transparent holes must preserve RGBA binding");
        auto borderedSource=original;
        for(auto& pixel:borderedSource.pixels)if(pixel.a<128)pixel={255,192,0,255};
        // W:A adds borders to the existing proxy; it does not reclassify the
        // source as a completely opaque import. Keep one ignored air sample so
        // this encoded fixture follows the same eight-pixel gutter path.
        borderedSource.pixels[size_t(64)*borderedSource.width+128].a=0;
        auto borderedPath=root/"bordered-native.png";
        writeNativePng(borderedPath,borderedSource,128);
        auto borderedEncoded=readHandle(openW(borderedPath.c_str()));
        check(matchesNativeProxyPng(original,borderedEncoded),"Encoded native border additions must preserve network RGBA binding");
        auto wrongMap=original;
        for(auto& pixel:wrongMap.pixels)if(pixel.a>=128)pixel={255,255,255,255};
        check(!matchesNativeProxy(wrongMap,decodedNative),"Different native artwork must not bind by dimensions alone");
        check(readPngMemory(before).pixels==original.pixels,"Memory decoder must preserve original RGBA bytes");
        auto nativeA=readHandle(openA(path.string().c_str()));
        verifyNative(nativeA,original);
        check(loader::lastImport()==firstImport,"Repeated opens must reuse the decoded RGBA and native proxy cache");
        check(loader::lastImport()->artwork()->pixels==original.pixels,"Lazily restored RGBA must remain byte-exact");
        auto outside=readHandle(openW((root/"outside.png").c_str()));
        check(outside[25]==6,"Outside SavedLevels must not be intercepted");
        HANDLE writable=overwrite(path.c_str());
        check(writable!=INVALID_HANDLE_VALUE,"Native editor writes must be redirected to its working proxy");
        CloseHandle(writable);
        auto opaquePath=root/"SavedLevels"/"opaque.png";writeRgbaPng(opaquePath,opaque);
        auto editorOpaque=readHandle(openW(opaquePath.c_str()));
        auto editorOpaqueImage=readPngMemory(editorOpaque,false);
        check(editorOpaqueImage.width==opaque.width&&editorOpaqueImage.height==opaque.height,
              "Editor proxy must keep source dimensions; teleport clearance is gameplay-only");
        check(editorOpaqueImage.pixels[opaque.width/2].r==0,
              "Fully opaque editor proxy must expose top air during W:A placement classification");
        check(editorOpaqueImage.pixels[size_t(nativeProxyEditorTopClearance-1)*opaque.width+opaque.width/2].r==0 &&
              editorOpaqueImage.pixels[size_t(nativeProxyEditorTopClearance)*opaque.width+opaque.width/2].r!=0,
              "Editor placement seed must retain the established offline geometry");
        auto pristineImport=loader::networkImport();
        check(pristineImport&&readPngMemory(pristineImport->nativePng,false).pixels[opaque.width/2].r!=0,
              "Canonical gameplay proxy must retain the complete uncarved source terrain");
        auto canonicalWire=readPngMemory(
            resizeIndexedPngTop(pristineImport->nativePng,opaque.height+padding,0),false);
        check(canonicalWire.pixels[size_t(padding-1)*opaque.width+opaque.width/2].r==0 &&
              canonicalWire.pixels[size_t(padding)*opaque.width+opaque.width/2].r!=0,
              "Canonical module-client proxy must contain only the external clearance before complete artwork");
        auto doublePaddedEditor=readPngMemory(
            resizeIndexedPngTop(editorOpaque,opaque.height+padding,0),false);
        check(doublePaddedEditor.pixels[size_t(padding)*opaque.width+opaque.width/2].r==0,
              "Editor placement proxy must never be reused as the padded module-client terrain");
        check(extractWaLevelMetadata(editorOpaque).has_value(),
              "Editor proxy must retain native custom-level classification");
        auto currentTheme=root/"DATA"/"current.thm";
        {std::ofstream f(currentTheme,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(editorOpaque.data()),editorOpaque.size());}
        auto openTheme=readPngMemory(readHandle(openW(currentTheme.c_str())),false);
        check(openTheme.height==opaque.height,
              "current.thm must remain source-sized while W:A generates native borders and holes");
        auto retainedBase=loader::networkImport();
        check(retainedBase&&readPngMemory(retainedBase->nativePng,false).pixels[opaque.width/2].r!=0,
              "Reading the seeded editor theme must not replace the pristine game-start collision map");
        auto writeLand=[&](uint32_t topBorder,uint32_t holes,bool classificationStrip=false,uint32_t locationY=0) {
            std::vector<uint8_t> land;
            auto u8=[&](uint8_t v){land.push_back(v);};
            auto u16=[&](uint16_t v){u8(uint8_t(v));u8(uint8_t(v>>8));};
            auto u32=[&](uint32_t v){u16(uint16_t(v));u16(uint16_t(v>>16));};
            u32(0x1a444e4c);u32(0);u32(opaque.width);u32(opaque.height);u32(topBorder);u32(0);u32(holes);u32(locationY?1:0);
            if(locationY){u32(opaque.width/2);u32(locationY);}
            auto image=[&](uint8_t bpp,uint8_t fill) {
                const auto bytes=size_t(opaque.width)*opaque.height*bpp/8;
                u32(0x1a474d49);u32(uint32_t(14+bytes));u8(bpp);u8(0);u16(uint16_t(opaque.width));u16(uint16_t(opaque.height));
                const auto pixels=land.size();land.insert(land.end(),bytes,fill);
                if(classificationStrip) {
                    const auto rowBytes=size_t(opaque.width)*bpp/8;
                    std::fill_n(land.begin()+pixels,rowBytes*padding,uint8_t(0));
                }
                if(holes&&bpp==8) {
                    const auto rowBytes=size_t(opaque.width);
                    for(uint32_t y=padding+8;y<padding+16&&y<opaque.height;++y)
                        std::fill_n(land.begin()+pixels+size_t(y)*rowBytes+32,16,uint8_t(0));
                }
            };
            image(8,1);image(1,0xff);image(1,0xff);
            const auto size=uint32_t(land.size());std::memcpy(land.data()+4,&size,4);
            std::ofstream f(root/"DATA"/"land.dat",std::ios::binary|std::ios::trunc);
            f.write(reinterpret_cast<const char*>(land.data()),land.size());
            return land;
        };
        auto rawLand=writeLand(0,1,true,padding);
        check(rawLand.size()>=32&&*reinterpret_cast<const uint32_t*>(rawLand.data())==0x1a444e4c&&
              *reinterpret_cast<const uint32_t*>(rawLand.data()+8)==opaque.width&&
              *reinterpret_cast<const uint32_t*>(rawLand.data()+12)==opaque.height,
              "Raw editor LAND fixture must match the source map");
        auto canonicalSnapshot=loader::canonicalizeNetworkLand(rawLand,opaque,opaque.height+padding,false);
        check(*reinterpret_cast<const uint32_t*>(canonicalSnapshot.data()+12)==opaque.height+padding &&
              *reinterpret_cast<const uint32_t*>(canonicalSnapshot.data()+24)==1,
              "Canonical LAND snapshot must preserve W:A holes while adding teleport height");
        const auto firstHoleX=*reinterpret_cast<const uint32_t*>(canonicalSnapshot.data()+32);
        const auto firstHoleY=*reinterpret_cast<const uint32_t*>(canonicalSnapshot.data()+36);
        check(firstHoleX>=52&&firstHoleX<opaque.width-52&&
              firstHoleY>=padding+52&&firstHoleY<opaque.height+padding-52,
              "First-pass holes must receive playable source coordinates shifted below teleport clearance");
        const auto canonicalPixels=46+size_t(*reinterpret_cast<const uint32_t*>(canonicalSnapshot.data()+28))*8;
        check(canonicalSnapshot[canonicalPixels+size_t(padding)*opaque.width]!=0 &&
              canonicalSnapshot[canonicalPixels+size_t(firstHoleY)*opaque.width+firstHoleX]==0,
              "Canonical LAND must restore source terrain and materialize the first-pass hole mask");
        auto borderedSnapshot=loader::canonicalizeNetworkLand(rawLand,opaque,opaque.height,true);
        const auto borderedPixels=46+size_t(*reinterpret_cast<const uint32_t*>(borderedSnapshot.data()+28))*8;
        const auto borderedHoleX=*reinterpret_cast<const uint32_t*>(borderedSnapshot.data()+32);
        const auto borderedHoleY=*reinterpret_cast<const uint32_t*>(borderedSnapshot.data()+36);
        check(*reinterpret_cast<const uint32_t*>(borderedSnapshot.data()+12)==opaque.height &&
              borderedSnapshot[borderedPixels+opaque.width/2]==0 &&
              borderedSnapshot[borderedPixels+size_t(nativeProxyBorder)*opaque.width+opaque.width/2]!=0,
              "Bordered LAND must remove the editor clearance while preserving the native top frame");
        check(borderedSnapshot[borderedPixels+size_t(borderedHoleY)*opaque.width+borderedHoleX]==0,
              "Bordered LAND must materialize requested holes inside playable terrain");
        check(borderedSnapshot[borderedPixels+size_t(borderedHoleY+32)*opaque.width+borderedHoleX+32]==0 &&
              borderedSnapshot[borderedPixels+size_t(borderedHoleY+33)*opaque.width+borderedHoleX+33]!=0,
              "Generated LAND holes must use an exact circular radius without a serrated edge");
        auto crossingLand=writeLand(0,1,true,120);
        const auto crossingPixels=46+size_t(*reinterpret_cast<const uint32_t*>(crossingLand.data()+28))*8;
        for(int dy=-46;dy<=46;++dy)for(int dx=-46;dx<=46;++dx)if(dx*dx+dy*dy<=46*46)
            crossingLand[crossingPixels+size_t(120+dy)*opaque.width+size_t(opaque.width/2+dx)]=0;
        auto completedCrossing=loader::canonicalizeNetworkLand(crossingLand,opaque,opaque.height+padding,false);
        const auto completedPixels=46+size_t(*reinterpret_cast<const uint32_t*>(completedCrossing.data()+28))*8;
        check(*reinterpret_cast<const uint32_t*>(completedCrossing.data()+36)==120+padding &&
              completedCrossing[completedPixels+size_t(padding+120-46)*opaque.width+opaque.width/2]==0,
              "Classification restoration must not clip the upper half of a hole crossing row 128");
        auto canonicalArtwork=loader::applyNetworkLandCollision(canonicalSnapshot,padded);
        check(canonicalArtwork.pixels[size_t(firstHoleY)*opaque.width+firstHoleX].a==0 &&
              canonicalArtwork.pixels[size_t(padding)*opaque.width].a==255,
              "RGBA preview must derive its holes and solid terrain from the canonical LAND plane");
        loader::setNetworkGameplayReady(false);
        auto sourceSnapshot=loader::canonicalizeNetworkLand(rawLand,opaque,opaque.height,false);
        auto expectedFirstPreview=loader::applyNetworkLandCollision(sourceSnapshot,opaque);
        {std::ofstream f(root/"DATA"/"land.dat",std::ios::binary|std::ios::trunc);
         f.write(reinterpret_cast<const char*>(rawLand.data()),std::streamsize(rawLand.size()));}
        readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(loader::networkImport()->artwork()->pixels==expectedFirstPreview.pixels,
              "The first editor LAND pass must immediately publish materialized holes to the host preview");
        writeLand(0,18,true);readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(loader::networkImport()==loader::lastImport(),
              "Editor LAND hole count must select W:A's hole-modified current theme for game start");
        check(loader::networkImport()->artwork()->pixels[size_t(padding+8)*opaque.width+32].a==0 &&
              loader::networkImport()->artwork()->pixels[32].a>=128,
              "LAND holes must enter the RGBA collision image while the temporary top strip is restored");
        {std::ofstream f(currentTheme,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(editorOpaque.data()),editorOpaque.size());}
        auto holePreview=readPngMemory(readHandle(openW(currentTheme.c_str())),false);
        check(holePreview.pixels[size_t(padding+8)*opaque.width+32].r==0,
              "Host editor preview must show the authoritative LAND hole mask");
        check(loader::networkImport()==loader::lastImport(),
              "A later current.thm bake must retain the authoritative LAND hole selection even when waLV still says zero holes");
        writeLand(0,0,true);readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(loader::networkImport()==retainedBase,
              "Clearing editor holes must restore the separately retained pristine game-start proxy");
        loader::setNetworkGameplayReady(true);
        writeLand(0,0,true,padding);auto paddedLand=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(*reinterpret_cast<const uint32_t*>(paddedLand.data()+12)==opaque.height+padding,
              "Borderless land.dat without holes must receive teleport placement clearance");
        const auto firstImagePixels=46+size_t(*reinterpret_cast<const uint32_t*>(paddedLand.data()+28))*8;
        check(paddedLand[firstImagePixels+size_t(padding)*opaque.width]!=0,
              "Temporary editor placement strip must be restored at the first source row after padding");
        check(*reinterpret_cast<const uint32_t*>(paddedLand.data()+36)==padding,
              "Placement coordinates generated against the temporary strip must not be shifted twice");
        check(loader::lastImport()->height==opaque.height+padding,
              "RGBA artwork must follow the gameplay-only land.dat clearance");
        check(loader::networkImport()->height==opaque.height,
              "Network serialization must retain the source-sized map after gameplay padding");
        check(loader::setNetworkCanonicalGameplay(true),
              "A borderless network map must activate its canonical wire representation");
        auto canonicalTheme=readPngMemory(readHandle(openW(currentTheme.c_str())),false);
        check(canonicalTheme.height==opaque.height+padding,
              "current.thm must expose the same canonical PNG transmitted to network peers");
        writeLand(0,0);auto canonicalLand=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(*reinterpret_cast<const uint32_t*>(canonicalLand.data()+12)==opaque.height+padding,
              "Canonical network terrain must match the padded wire-map height");
        check(loader::lastImport()->height==opaque.height+padding,
              "Canonical network artwork must retain the transmitted top clearance");
        loader::setNetworkCanonicalGameplay(false);
        check(loader::lastImport()->height==opaque.height,
              "Leaving the network game must restore the source-sized editor map");
        loader::setNetworkGameplayReady(false);
        writeLand(0,18);auto editorLand=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(*reinterpret_cast<const uint32_t*>(editorLand.data()+12)==opaque.height,
              "Network editor land.dat must not receive gameplay clearance before game start");
        loader::setNetworkGameplayReady(true);
        readHandle(openW(currentTheme.c_str()));writeLand(1,18);
        auto borderedHoles=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(*reinterpret_cast<const uint32_t*>(borderedHoles.data()+12)==opaque.height,
              "Top-border land.dat with holes must stay source-sized so W:A's holes survive");
        auto fullBorderLevel=*extractWaLevelMetadata(editorOpaque);fullBorderLevel[16]=0;
        const auto fullBorderFixture=root/"full-border-theme.png";writeNativePng(fullBorderFixture,opaque,128,fullBorderLevel);
        const auto fullBorderBytes=readHandle(openW(fullBorderFixture.c_str()));
        {std::ofstream f(currentTheme,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(fullBorderBytes.data()),fullBorderBytes.size());}
        readHandle(openW(currentTheme.c_str()));writeLand(0,0);
        auto fullBorderLand=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(*reinterpret_cast<const uint32_t*>(fullBorderLand.data()+12)==opaque.height,
              "Full-border map state must suppress teleport clearance even when LAND TopBorder is clear");
        auto fullBorderMap=loader::lastImport();
        loader::setNetworkCanonicalLand(fullBorderLand);
        check(fullBorderMap&&loader::setNetworkCanonicalGameplayExact(*fullBorderMap->artwork(),
                  fullBorderMap->rgbaPng,fullBorderMap->nativePng),
              "A synchronized LAND snapshot must activate with its exact canonical map");
        writeLand(0,0);
        auto synchronizedLand=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(synchronizedLand==fullBorderLand,
              "Network peers must consume the host's exact LAND pixels, hole count and locations");
        loader::setNetworkCanonicalGameplay(false);
        {std::ofstream f(currentTheme,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(editorOpaque.data()),editorOpaque.size());}
        readHandle(openW(currentTheme.c_str()));writeLand(0,18);
        auto nativeHoles=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(*reinterpret_cast<const uint32_t*>(nativeHoles.data()+12)==opaque.height+padding,
              "Borderless land.dat with holes must receive teleport clearance after W:A generates them");
        loader::acceptImage(opaque,L"wkRGBA_network.png",writeRgbaPngMemory(opaque),editorOpaque);
        writeLand(0,18);
        auto clientLandA=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        auto clientLandB=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(*reinterpret_cast<const uint32_t*>(clientLandA.data()+12)==opaque.height+padding &&
              *reinterpret_cast<const uint32_t*>(clientLandB.data()+12)==opaque.height+padding,
              "A network client without a SavedLevels path must transform every land.dat open exactly like the host");
        check(loader::lastImport()->height==opaque.height+padding,
              "Repeated client land.dat opens must not accumulate teleport padding");
        check(loader::networkImport()->height==opaque.height,
              "Repeated gameplay opens must not replace the source-sized network map");
        loader::setLegacyCompatibilityGameplay(true,true);
        writeLand(0,18,true,padding);
        auto legacyLand=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(*reinterpret_cast<const uint32_t*>(legacyLand.data()+12)==opaque.height,
              "Legacy compatibility must leave W:A land.dat dimensions unchanged");
        check(*reinterpret_cast<const uint32_t*>(legacyLand.data()+24)==18,
              "Legacy compatibility must preserve W:A's generated hole state");
        check(*reinterpret_cast<const uint32_t*>(legacyLand.data()+36)==padding,
              "Legacy compatibility must preserve W:A's placement coordinates");
        const auto legacyPixels=46+size_t(*reinterpret_cast<const uint32_t*>(legacyLand.data()+28))*8;
        check(legacyLand[legacyPixels]!=0,
              "Legacy compatibility must restore the disposable top classification strip");
        loader::setLegacyCompatibilityGameplay(true,false);
        writeLand(0,18,true,padding);
        auto exactLegacyLand=readHandle(openW((root/"DATA"/"land.dat").c_str()));
        check(*reinterpret_cast<const uint32_t*>(exactLegacyLand.data()+12)==opaque.height,
              "Exact legacy compatibility must preserve W:A's land.dat height");
        check(*reinterpret_cast<const uint32_t*>(exactLegacyLand.data()+24)==18,
              "Exact legacy compatibility must preserve W:A's generated holes");
        check(*reinterpret_cast<const uint32_t*>(exactLegacyLand.data()+36)==padding,
              "Exact legacy compatibility must preserve W:A's placement coordinates");
        check(exactLegacyLand[legacyPixels]==0,
              "Exact legacy compatibility must not restore or transform W:A's classification strip");
        loader::setLegacyCompatibilityGameplay(false,false);
        {std::ofstream f(root/"SavedLevels"/"indexed.png",std::ios::binary);f.write(reinterpret_cast<const char*>(nativeA.data()),nativeA.size());}
        auto nativeAgain=readHandle(openW((root/"SavedLevels"/"indexed.png").c_str()));
        check(nativeAgain==nativeA,"Indexed maps must pass through unchanged");
        loader::uninstallTestImports();
        auto after=readHandle(openW(path.c_str()));
        check(before==after,"Original PNG must not be modified");
        CoUninitialize();
        std::cout<<"IAT CreateFileA/W: RGBA intercepted, opaque-black native collision proxy decoded, original unchanged, indexed and outside paths passed through\n";
        return 0;
    }catch(const std::exception& e){loader::uninstallTestImports();std::cerr<<e.what()<<'\n';return 1;}
}

