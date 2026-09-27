#include "Loader.h"
#include "Png.h"
#include "Transfer.h"
#include "RenderProbe.h"
#include "ExperimentalScene.h"
#include "WormLabels.h"
#include "ReplayHooks.h"
#include "NetworkHooks.h"
#include "InlineHook.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace wkrgba::loader {
ImportedMap::ImportedMap(std::filesystem::path path,Image image,std::vector<uint8_t> png,std::vector<uint8_t> native)
    : source(std::move(path)),width(image.width),height(image.height),manifest(net::identify(image,128,1)),
      rgbaPng(std::move(png)),nativePng(std::move(native)),artwork_(std::make_shared<const Image>(std::move(image))) {
    if(rgbaPng.empty())rgbaPng=writeRgbaPngMemory(*artwork_);
    if(!nativePng.empty())if(auto metadata=extractWaLevelMetadata(nativePng);metadata&&metadata->size()>=20) {
        const auto* state=metadata->data()+16;
        const uint32_t noBorders=uint32_t(state[0])|uint32_t(state[1])<<8|uint32_t(state[2])<<16|uint32_t(state[3])<<24;
        bordered=noBorders==0;
    }
    manifest.payloadSize=uint32_t(rgbaPng.size());manifest.payload=net::sha256(rgbaPng);
}
std::shared_ptr<const Image> ImportedMap::artwork() const {
    std::lock_guard lock(artworkMutex_);
    if(!artwork_) {
        auto decoded=std::make_shared<const Image>(readPngMemory(rgbaPng));
        if(decoded->width!=width || decoded->height!=height)throw std::runtime_error("Cached RGBA dimensions changed");
        artwork_=std::move(decoded);
    }
    return artwork_;
}
void ImportedMap::discardArtwork(){std::lock_guard lock(artworkMutex_);artwork_.reset();}
namespace {
using OpenA=decltype(&CreateFileA); using OpenW=decltype(&CreateFileW);
OpenA originalA=CreateFileA; OpenW originalW=CreateFileW;
decltype(&GetProcAddress) originalProc=GetProcAddress;
FARPROC WINAPI hookProc(HMODULE module,LPCSTR name) {
    return experimental::resolve(module,name,originalProc(module,name));
}
std::atomic<bool> enabled{false};
std::atomic<bool> networkGameplayReady{true};
std::atomic<bool> legacyCompatibilityGameplay{false};
std::atomic<bool> legacyRestoreTop{false};
InlineHook editorNativeStateHook;
struct Store {
    std::mutex mutex;
    std::shared_ptr<const ImportedMap> map;
    std::shared_ptr<const ImportedMap> landBaseMap;
    std::shared_ptr<const ImportedMap> classificationBaseMap;
    std::filesystem::path cachedSource;
    std::filesystem::file_time_type cachedWriteTime{};
    uintmax_t cachedSize{};
    bool borderedMode{};
    std::filesystem::path proxyPath;
    HANDLE proxyKeeper{INVALID_HANDLE_VALUE};
    std::filesystem::path themeProxyPath;
    HANDLE themeProxyKeeper{INVALID_HANDLE_VALUE};
    std::filesystem::file_time_type themeWriteTime{};
    uintmax_t themeSize{};
    bool themeBordered{};
    uint32_t themeHeight{};
    bool networkCanonicalGameplay{};
    bool placementClassificationSeeded{};
    uint32_t editorHoleCount{};
    std::vector<uint8_t> networkLand;
};
Store& store(){static auto* p=new Store;return *p;}
std::filesystem::path savedLevels;
std::filesystem::path gameRoot;
std::filesystem::path logPath;
thread_local bool inside=false;
void log(const std::string& text) noexcept {
    try {std::ofstream f(logPath,std::ios::app);f<<"t="<<GetTickCount64()<<" "<<text<<'\n';}catch(...){}
}
void clearRuntimeDirectory(const std::filesystem::path& directory) noexcept {
    try {
        std::filesystem::create_directories(directory);
        for(const auto& entry:std::filesystem::directory_iterator(directory)) {
            std::error_code error;
            std::filesystem::remove_all(entry.path(),error);
        }
    }catch(...){}
}
std::wstring lower(std::wstring s){std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return wchar_t(towlower(c));});return s;}
bool candidate(const std::filesystem::path& path) {
    if(lower(path.extension().wstring())!=L".png")return false;
    auto full=lower(std::filesystem::weakly_canonical(path).make_preferred().wstring());
    auto root=lower(std::filesystem::path(savedLevels).make_preferred().wstring());
    if(!root.empty() && root.back()!=L'\\')root+=L'\\';
    return !root.empty() && full.starts_with(root);
}
bool rgbaHeader(const wchar_t* path) {
    HANDLE h=originalW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    std::array<uint8_t,26> data{};DWORD count{};
    const bool read=ReadFile(h,data.data(),DWORD(data.size()),&count,nullptr)!=0;
    CloseHandle(h);
    constexpr std::array<uint8_t,8> signature{137,80,78,71,13,10,26,10};
    return read && count==data.size() && std::equal(signature.begin(),signature.end(),data.begin()) && data[24]==8 && data[25]==6;
}
std::optional<std::vector<uint8_t>> editorLevelMetadata() noexcept {
    try {
        if(gameRoot.empty())return std::nullopt;
        std::ifstream file(gameRoot/L"DATA"/L"current.thm",std::ios::binary|std::ios::ate);
        if(!file)return std::nullopt;const auto length=file.tellg();
        if(length<45||length>128ll*1024*1024)return std::nullopt;
        std::vector<uint8_t> bytes(static_cast<size_t>(length));file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(bytes.data()),length))return std::nullopt;
        constexpr std::array<uint8_t,8> signature{137,80,78,71,13,10,26,10};
        const auto png=std::search(bytes.begin(),bytes.end(),signature.begin(),signature.end());
        if(png==bytes.end())return std::nullopt;
        auto metadata=extractWaLevelMetadata(std::span<const uint8_t>(&*png,size_t(bytes.end()-png)));
        if(!metadata||metadata->size()<40||metadata->size()>42)return std::nullopt;
        return metadata;
    }catch(...){return std::nullopt;}
}
std::optional<bool> editorBorderEnabled(const std::optional<std::vector<uint8_t>>& metadata) noexcept {
    if(!metadata||metadata->size()<20)return std::nullopt;
    const auto* p=metadata->data()+16;
    const uint32_t noIndestructibleBorders=uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
    return noIndestructibleBorders==0;
}
bool isCurrentTheme(const std::filesystem::path& path) noexcept {
    try {
        if(gameRoot.empty())return false;
        return lower(std::filesystem::weakly_canonical(path).make_preferred().wstring())==
               lower(std::filesystem::weakly_canonical(gameRoot/L"DATA"/L"current.thm").make_preferred().wstring());
    }catch(...){return false;}
}
bool isLandData(const std::filesystem::path& path) noexcept {
    try {
        if(gameRoot.empty())return false;
        return lower(std::filesystem::weakly_canonical(path).make_preferred().wstring())==
               lower(std::filesystem::weakly_canonical(gameRoot/L"DATA"/L"land.dat").make_preferred().wstring());
    }catch(...){return false;}
}
void clearThemeProxy(Store& cache) noexcept {
    if(cache.themeProxyKeeper!=INVALID_HANDLE_VALUE){CloseHandle(cache.themeProxyKeeper);cache.themeProxyKeeper=INVALID_HANDLE_VALUE;}
    cache.themeProxyPath.clear();cache.themeSize=0;cache.themeHeight=0;
}
HANDLE serveCurrentTheme(const wchar_t* path,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD flags,HANDLE templateFile) {
    auto& cache=store();std::filesystem::path sourcePath;std::shared_ptr<const ImportedMap> canonicalMap,classificationBase,holeBase;
    uint32_t observedHoles{};bool classificationSeeded{};
    {
        std::lock_guard lock(cache.mutex);
        if(cache.networkCanonicalGameplay)canonicalMap=cache.map;
        if(!canonicalMap&&(!cache.map || cache.cachedSource.empty()))return INVALID_HANDLE_VALUE;
        sourcePath=cache.cachedSource;classificationBase=cache.classificationBaseMap;holeBase=cache.landBaseMap;
        observedHoles=cache.editorHoleCount;classificationSeeded=cache.placementClassificationSeeded;
    }
    if(!canonicalMap&&!std::filesystem::exists(sourcePath))return INVALID_HANDLE_VALUE;
    const auto themePath=std::filesystem::weakly_canonical(path);
    const auto themeSize=std::filesystem::file_size(themePath);
    const auto themeTime=std::filesystem::last_write_time(themePath);
    std::ifstream input(themePath,std::ios::binary|std::ios::ate);
    if(!input || themeSize<37 || themeSize>128ull*1024*1024)return INVALID_HANDLE_VALUE;
    std::vector<uint8_t> theme(static_cast<size_t>(themeSize));input.seekg(0);
    if(!input.read(reinterpret_cast<char*>(theme.data()),std::streamsize(theme.size())))return INVALID_HANDLE_VALUE;
    constexpr std::array<uint8_t,8> signature{137,80,78,71,13,10,26,10};
    const auto found=std::search(theme.begin(),theme.end(),signature.begin(),signature.end());
    if(found==theme.end())return INVALID_HANDLE_VALUE;
    const auto offset=size_t(found-theme.begin());std::span<const uint8_t> native(theme.data()+offset,theme.size()-offset);
    auto metadata=extractWaLevelMetadata(native);if(!metadata||metadata->size()<20)return INVALID_HANDLE_VALUE;
    const auto* state=metadata->data()+16;
    const uint32_t noBorders=uint32_t(state[0])|uint32_t(state[1])<<8|uint32_t(state[2])<<16|uint32_t(state[3])<<24;
    const bool bordered=noBorders==0;
    uint32_t nativeHoles{};
    if(metadata->size()>=32) {
        const auto* count=metadata->data()+28;
        nativeHoles=uint32_t(count[0])|uint32_t(count[1])<<8|uint32_t(count[2])<<16|uint32_t(count[3])<<24;
        log("W:A current.thm serialized hole count="+std::to_string(nativeHoles));
    }
    if(canonicalMap) {
        const auto& adjusted=canonicalMap->nativePng;
        if(adjusted.size()<33||adjusted[25]!=3)return INVALID_HANDLE_VALUE;
        const auto canonicalWidth=(uint32_t(adjusted[16])<<24)|(uint32_t(adjusted[17])<<16)|
                                  (uint32_t(adjusted[18])<<8)|adjusted[19];
        const auto canonicalHeight=(uint32_t(adjusted[20])<<24)|(uint32_t(adjusted[21])<<16)|
                                   (uint32_t(adjusted[22])<<8)|adjusted[23];
        if(canonicalWidth!=canonicalMap->width||canonicalHeight!=canonicalMap->height)return INVALID_HANDLE_VALUE;
        std::vector<uint8_t> output;output.reserve(offset+adjusted.size());
        output.insert(output.end(),theme.begin(),theme.begin()+offset);output.insert(output.end(),adjusted.begin(),adjusted.end());
        wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
        if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"wkc",0,tempFile))throw std::runtime_error("Cannot create canonical W:A theme");
        {std::ofstream out(tempFile,std::ios::binary|std::ios::trunc);if(!out.write(reinterpret_cast<const char*>(output.data()),output.size()))throw std::runtime_error("Cannot write canonical W:A theme");}
        HANDLE keeper=originalW(tempFile,DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
                                FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,templateFile);
        if(keeper==INVALID_HANDLE_VALUE){DeleteFileW(tempFile);throw std::runtime_error("Cannot retain canonical W:A theme");}
        HANDLE result=originalW(tempFile,GENERIC_READ,share|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
                                (flags&~FILE_ATTRIBUTE_NORMAL)|FILE_ATTRIBUTE_TEMPORARY,templateFile);
        if(result==INVALID_HANDLE_VALUE){CloseHandle(keeper);throw std::runtime_error("Cannot open canonical W:A theme");}
        {
            std::lock_guard lock(cache.mutex);clearThemeProxy(cache);
            cache.themeProxyPath=tempFile;cache.themeProxyKeeper=keeper;cache.themeSize=themeSize;cache.themeWriteTime=themeTime;
            cache.themeBordered=false;cache.themeHeight=canonicalHeight;
        }
        log("W:A current.thm replaced with canonical network PNG "+std::to_string(canonicalWidth)+"x"+std::to_string(canonicalHeight));
        return result;
    }
    auto original=readRgbaPngWithBytes(sourcePath);
    Image desired=std::move(original.image);
    if(native.size()<33 || native[25]!=3)return INVALID_HANDLE_VALUE;
    const auto nativeWidth=(uint32_t(native[16])<<24)|(uint32_t(native[17])<<16)|(uint32_t(native[18])<<8)|native[19];
    const auto nativeHeight=(uint32_t(native[20])<<24)|(uint32_t(native[21])<<16)|(uint32_t(native[22])<<8)|native[23];
    if(nativeWidth!=desired.width)return INVALID_HANDLE_VALUE;
    std::vector<uint8_t> adjusted(native.begin(),native.end());
    if(nativeHeight!=desired.height)adjusted=resizeIndexedPngTop(native,desired.height,bordered?nativeProxyBorder:0);
    if(bordered&&classificationSeeded&&classificationBase&&
       pngDimensionsMatch(classificationBase->nativePng,desired.width,desired.height)) {
        // The initial borderless editor proxy contains a disposable 128-row
        // air strip. W:A keeps that strip when borders are enabled without
        // changing dimensions. Rebuild from the separately retained pristine
        // proxy, then carry only the current border metadata forward.
        adjusted=classificationBase->nativePng;
        adjusted=attachWaLevelMetadata(adjusted,*metadata);
        log("Bordered current.thm rebuilt from complete source; temporary placement strip removed");
    }
    if(!bordered&&observedHoles&&holeBase&&
       pngDimensionsMatch(holeBase->nativePng,desired.width,desired.height)) {
        // LAND is authoritative for editor-generated holes.  Returning this
        // already-derived proxy makes the host preview show the same mask
        // that will be serialized to peers at game start.
        adjusted=holeBase->nativePng;
        adjusted=attachWaLevelMetadata(adjusted,*metadata);
        log("W:A current.thm preview synchronized with authoritative LAND holes="+
            std::to_string(observedHoles));
    }
    std::vector<uint8_t> output;output.reserve(offset+adjusted.size());
    output.insert(output.end(),theme.begin(),theme.begin()+offset);output.insert(output.end(),adjusted.begin(),adjusted.end());
    wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
    if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"wkt",0,tempFile))throw std::runtime_error("Cannot create adjusted W:A theme");
    {std::ofstream out(tempFile,std::ios::binary|std::ios::trunc);if(!out.write(reinterpret_cast<const char*>(output.data()),output.size()))throw std::runtime_error("Cannot write adjusted W:A theme");}
    HANDLE keeper=originalW(tempFile,DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
                            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,templateFile);
    if(keeper==INVALID_HANDLE_VALUE){DeleteFileW(tempFile);throw std::runtime_error("Cannot retain adjusted W:A theme");}
    HANDLE result=originalW(tempFile,GENERIC_READ,share|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
                            (flags&~FILE_ATTRIBUTE_NORMAL)|FILE_ATTRIBUTE_TEMPORARY,templateFile);
    if(result==INVALID_HANDLE_VALUE){CloseHandle(keeper);throw std::runtime_error("Cannot open adjusted W:A theme");}
    auto rgba=writeRgbaPngMemory(desired);if(auto level=extractWaLevelMetadata(original.bytes))rgba=attachWaLevelMetadata(rgba,*level);
    auto imported=std::make_shared<ImportedMap>(sourcePath,std::move(desired),std::move(rgba),adjusted);imported->discardArtwork();
    imported->bordered=bordered;
    {
        std::lock_guard lock(cache.mutex);clearThemeProxy(cache);
        cache.themeProxyPath=tempFile;cache.themeProxyKeeper=keeper;cache.themeSize=themeSize;cache.themeWriteTime=themeTime;
        cache.themeBordered=bordered;
        cache.themeHeight=imported->height;cache.map=std::move(imported);
        // The first disposable proxy for a fully opaque borderless image has
        // a temporary top-air strip solely to make W:A classify the map as
        // auto-placeable.  Until the editor actually adds holes or borders,
        // retain the separately encoded, uncarved collision map as the source
        // for the canonical game-start map.
        if(cache.placementClassificationSeeded&&!bordered&&!nativeHoles) {
            if(cache.editorHoleCount&&cache.landBaseMap&&!cache.landBaseMap->bordered)cache.map=cache.landBaseMap;
            else cache.map=cache.landBaseMap=cache.classificationBaseMap;
            log("W:A editor classification proxy observed; "+
                std::string(cache.editorHoleCount?"hole-modified":"pristine")+
                " collision map retained for game start (LAND holes="+std::to_string(cache.editorHoleCount)+")");
        } else {
            cache.landBaseMap=cache.map;
            // Border toggles must not discard the separately retained source
            // classification. Otherwise removing the border later makes LAND
            // holes invisible until the PNG is reloaded from disk.
            if(!cache.classificationBaseMap)cache.placementClassificationSeeded=false;
            // waLV remains zero after the editor writes a nonzero LAND hole
            // count. Do not let a later current.thm read erase the state that
            // was observed from the authoritative LAND header.
            cache.editorHoleCount=observedHoles?observedHoles:nativeHoles;
        }
    }
    log("W:A current.thm synchronized at editor dimensions; borders and holes deferred to land.dat");
    return result;
}
uint32_t landU32(const std::vector<uint8_t>& bytes,size_t at) {
    if(at+4>bytes.size())throw std::runtime_error("Truncated land.dat");
    return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;
}
void landPut32(std::vector<uint8_t>& bytes,size_t at,uint32_t value) {
    if(at+4>bytes.size())throw std::runtime_error("Truncated land.dat");
    for(unsigned i=0;i<4;++i)bytes[at+i]=uint8_t(value>>(i*8));
}
std::vector<uint8_t> addLandTopRows(const std::vector<uint8_t>& input,uint32_t rows,bool shiftLocations=true) {
    if(input.size()<32||landU32(input,0)!=0x1a444e4c)throw std::runtime_error("Invalid land.dat signature");
    const auto width=landU32(input,8),height=landU32(input,12),locations=landU32(input,28);
    if(!width||!height||height+rows>maxDimension||locations>100000)throw std::runtime_error("Invalid land.dat dimensions");
    size_t at=32+size_t(locations)*8;if(at>input.size())throw std::runtime_error("Invalid land.dat locations");
    std::vector<uint8_t> output(input.begin(),input.begin()+at);
    for(uint32_t i=0;shiftLocations&&i<locations;++i) {
        const size_t yAt=32+size_t(i)*8+4;landPut32(output,yAt,landU32(output,yAt)+rows);
    }
    for(unsigned imageIndex=0;imageIndex<3;++imageIndex) {
        const auto start=at;if(start+14>input.size()||landU32(input,start)!=0x1a474d49)throw std::runtime_error("Invalid land.dat IMG");
        const auto blockSize=landU32(input,start+4);if(blockSize<14||start+blockSize>input.size())throw std::runtime_error("Invalid land.dat IMG size");
        size_t p=start+8;uint8_t bpp=input[p++];
        if(!bpp)bpp=input[p++];else if(bpp>32){while(p<start+blockSize&&input[p++]){}if(p>=start+blockSize)throw std::runtime_error("Invalid land.dat IMG description");bpp=input[p++];}
        if(p>=start+blockSize)throw std::runtime_error("Invalid land.dat IMG flags");const auto flags=input[p++];
        if(flags&0x40)throw std::runtime_error("Compressed land.dat IMG is unsupported");
        if(flags&0x80){if(p+2>start+blockSize)throw std::runtime_error("Invalid land.dat IMG palette");const auto count=uint16_t(input[p])|uint16_t(input[p+1])<<8;p+=2+size_t(count)*3;}
        if(p+4>start+blockSize)throw std::runtime_error("Invalid land.dat IMG dimensions");
        const auto imageWidth=uint16_t(input[p])|uint16_t(input[p+1])<<8;
        const auto imageHeight=uint16_t(input[p+2])|uint16_t(input[p+3])<<8;
        if(uint32_t(imageWidth)!=width||uint32_t(imageHeight)!=height||!bpp)throw std::runtime_error("Mismatched land.dat IMG dimensions");
        const size_t dimensionAt=p;p+=4;
        const uint64_t rowBytes64=(uint64_t(width)*bpp+7)/8;
        const uint64_t dataBytes64=rowBytes64*height,added64=rowBytes64*rows;
        if(dataBytes64>SIZE_MAX||added64>SIZE_MAX||p+size_t(dataBytes64)>start+blockSize)throw std::runtime_error("Invalid land.dat IMG pixels");
        const auto outputStart=output.size();output.insert(output.end(),input.begin()+start,input.begin()+p);
        const auto newHeight=height+rows;output[outputStart+(dimensionAt-start)+2]=uint8_t(newHeight);output[outputStart+(dimensionAt-start)+3]=uint8_t(newHeight>>8);
        output.insert(output.end(),size_t(added64),0);
        output.insert(output.end(),input.begin()+p,input.begin()+start+blockSize);
        landPut32(output,outputStart+4,uint32_t(blockSize+added64));at=start+blockSize;
    }
    output.insert(output.end(),input.begin()+at,input.end());landPut32(output,12,height+rows);landPut32(output,4,uint32_t(output.size()));
    return output;
}
std::vector<uint8_t> restoreLandClassificationRows(const std::vector<uint8_t>& input,const Image& original,
                                                    uint32_t rows,bool preserveNativeBorders=false) {
    if(input.size()<32||landU32(input,0)!=0x1a444e4c)throw std::runtime_error("Invalid land.dat signature");
    const auto width=landU32(input,8),height=landU32(input,12),locations=landU32(input,28);
    original.validate();
    if(width!=original.width||height!=original.height||rows>height||locations>100000)
        throw std::runtime_error("Classification restore dimensions do not match land.dat");
    auto output=input;size_t at=32+size_t(locations)*8;
    if(at>output.size())throw std::runtime_error("Invalid land.dat locations");
    for(unsigned imageIndex=0;imageIndex<3;++imageIndex) {
        const auto start=at;if(start+14>output.size()||landU32(output,start)!=0x1a474d49)throw std::runtime_error("Invalid land.dat IMG");
        const auto blockSize=landU32(output,start+4);if(blockSize<14||start+blockSize>output.size())throw std::runtime_error("Invalid land.dat IMG size");
        size_t p=start+8;uint8_t bpp=output[p++];
        if(!bpp)bpp=output[p++];else if(bpp>32){while(p<start+blockSize&&output[p++]){}if(p>=start+blockSize)throw std::runtime_error("Invalid land.dat IMG description");bpp=output[p++];}
        if(p>=start+blockSize)throw std::runtime_error("Invalid land.dat IMG flags");const auto flags=output[p++];
        if(flags&0x40)throw std::runtime_error("Compressed land.dat IMG is unsupported");
        if(flags&0x80){if(p+2>start+blockSize)throw std::runtime_error("Invalid land.dat IMG palette");const auto count=uint16_t(output[p])|uint16_t(output[p+1])<<8;p+=2+size_t(count)*3;}
        if(p+4>start+blockSize)throw std::runtime_error("Invalid land.dat IMG dimensions");
        const auto imageWidth=uint16_t(output[p])|uint16_t(output[p+1])<<8;
        const auto imageHeight=uint16_t(output[p+2])|uint16_t(output[p+3])<<8;p+=4;
        if(uint32_t(imageWidth)!=width||uint32_t(imageHeight)!=height)throw std::runtime_error("Mismatched land.dat IMG dimensions");
        const auto rowBytes=(size_t(width)*bpp+7)/8;if(p+rowBytes*height>start+blockSize)throw std::runtime_error("Invalid land.dat IMG pixels");
        if(bpp==8) {
            // W:A compacts the 65-entry import palette while baking LAND, so
            // the original nativeProxyIndex is not necessarily the final LAND
            // byte. Learn the exact remap from the untouched source rows and
            // use it when restoring the temporary classification strip.
            std::array<std::array<uint32_t,256>,65> frequencies{};
            for(uint32_t y=rows;y<height;++y)for(uint32_t x=0;x<width;++x) {
                const auto sourceIndex=nativeProxyIndex(original.pixels[size_t(y)*width+x],128);
                if(sourceIndex<frequencies.size())++frequencies[sourceIndex][output[p+size_t(y)*rowBytes+x]];
            }
            std::array<uint8_t,65> remap{};
            for(size_t sourceIndex=0;sourceIndex<remap.size();++sourceIndex) {
                const auto best=std::max_element(frequencies[sourceIndex].begin(),frequencies[sourceIndex].end());
                remap[sourceIndex]=best!=frequencies[sourceIndex].end()&&*best
                    ?uint8_t(best-frequencies[sourceIndex].begin()):uint8_t(sourceIndex);
            }
            const auto firstY=preserveNativeBorders?(std::min)(nativeProxyBorder,rows):0u;
            const auto firstX=preserveNativeBorders?(std::min)(nativeProxyBorder,width):0u;
            const auto lastX=preserveNativeBorders&&width>nativeProxyBorder?width-nativeProxyBorder:width;
            for(uint32_t y=firstY;y<rows;++y)for(uint32_t x=firstX;x<lastX;++x)
                output[p+size_t(y)*rowBytes+x]=remap[nativeProxyIndex(original.pixels[size_t(y)*width+x],128)];
        } else if(bpp==1) {
            const auto firstY=preserveNativeBorders?(std::min)(nativeProxyBorder,rows):0u;
            if(!preserveNativeBorders) {
                for(uint32_t y=firstY;y<rows;++y)
                    std::fill_n(output.begin()+p+size_t(y)*rowBytes,rowBytes,uint8_t(0xff));
            } else {
                const auto firstX=(std::min)(nativeProxyBorder,width);
                const auto lastX=width>nativeProxyBorder?width-nativeProxyBorder:width;
                for(uint32_t y=firstY;y<rows;++y)for(uint32_t x=firstX;x<lastX;++x)
                    output[p+size_t(y)*rowBytes+x/8]|=uint8_t(1u<<(x&7));
            }
        } else throw std::runtime_error("Unsupported land.dat classification image depth");
        at=start+blockSize;
    }
    return output;
}
struct LandPlane { uint8_t bpp{};size_t pixels{},rowBytes{}; };
std::array<LandPlane,3> mutableLandPlanes(std::vector<uint8_t>& land,uint32_t width,uint32_t height) {
    const auto locations=landU32(land,28);size_t at=32+size_t(locations)*8;
    std::array<LandPlane,3> planes{};
    for(auto& plane:planes) {
        const auto start=at;if(start+14>land.size()||landU32(land,start)!=0x1a474d49)throw std::runtime_error("Invalid LAND image");
        const auto blockSize=landU32(land,start+4);if(blockSize<14||start+blockSize>land.size())throw std::runtime_error("Invalid LAND image size");
        size_t p=start+8;uint8_t bpp=land[p++];
        if(!bpp)bpp=land[p++];else if(bpp>32){while(p<start+blockSize&&land[p++]){}if(p>=start+blockSize)throw std::runtime_error("Invalid LAND description");bpp=land[p++];}
        if(p>=start+blockSize)throw std::runtime_error("Invalid LAND flags");const auto flags=land[p++];
        if(flags&0x40)throw std::runtime_error("Compressed LAND image is unsupported");
        if(flags&0x80){if(p+2>start+blockSize)throw std::runtime_error("Invalid LAND palette");const auto count=uint16_t(land[p])|uint16_t(land[p+1])<<8;p+=2+size_t(count)*3;}
        if(p+4>start+blockSize)throw std::runtime_error("Invalid LAND dimensions");
        const auto imageWidth=uint16_t(land[p])|uint16_t(land[p+1])<<8;
        const auto imageHeight=uint16_t(land[p+2])|uint16_t(land[p+3])<<8;p+=4;
        const auto rowBytes=(size_t(width)*bpp+7)/8;
        if(uint32_t(imageWidth)!=width||uint32_t(imageHeight)!=height||p+rowBytes*height>start+blockSize)
            throw std::runtime_error("Mismatched LAND image dimensions");
        plane={bpp,p,rowBytes};at=start+blockSize;
    }
    return planes;
}
bool ensureFirstGenerationHoles(std::vector<uint8_t>& land,const Image& source) {
    const auto requested=landU32(land,24),locations=landU32(land,28);
    if(!requested)return false;
    auto planes=mutableLandPlanes(land,source.width,source.height);
    if(planes[0].bpp!=8||planes[1].bpp!=1||planes[2].bpp!=1)return false;
    size_t carved{};
    // Ignore W:A's native eight-pixel frame while deciding whether holes were
    // actually carved. Otherwise the border itself can satisfy the threshold
    // and leave a nonzero holes header with no cavities in playable terrain.
    for(uint32_t y=nativeProxyBorder;y+nativeProxyBorder<source.height;++y)
        for(uint32_t x=nativeProxyBorder;x+nativeProxyBorder<source.width;++x) {
        const auto i=size_t(y)*source.width+x;
        if(source.pixels[i].a>=128&&!land[planes[0].pixels+size_t(y)*planes[0].rowBytes+x])++carved;
        }
    if(locations<requested||source.width<112||source.height<112)return false;

    // A native W:A cavity removes roughly 6500 pixels. If enough terrain was
    // removed and the first requested locations point into those cavities,
    // retain W:A's placement. We still repaint the complete circular masks:
    // restoring the temporary 128-row classification strip can otherwise
    // fill only the upper halves of holes crossing that boundary.
    constexpr int radius=46,margin=52,minDistance=104;
    struct Point {uint32_t x,y;};std::vector<Point> centres;centres.reserve(requested);
    bool useExisting=carved>=size_t(requested)*2000;
    for(uint32_t i=0;i<requested&&useExisting;++i) {
        const auto x=landU32(land,32+size_t(i)*8),y=landU32(land,36+size_t(i)*8);
        if(x<margin||y<margin||x+margin>=source.width||y+margin>=source.height)useExisting=false;
        else centres.push_back({x,y});
    }
    if(!useExisting)centres.clear();

    uint32_t state=2166136261u;
    const auto stride=(std::max)(size_t(1),source.pixels.size()/2048);
    for(size_t i=0;i<source.pixels.size();i+=stride) {
        const auto p=source.pixels[i];state=(state^p.r)*16777619u;state=(state^p.g)*16777619u;
        state=(state^p.b)*16777619u;state=(state^p.a)*16777619u;
    }
    state^=requested*0x9E3779B9u;
    auto random=[&] {state^=state<<13;state^=state>>17;state^=state<<5;return state;};
    for(uint32_t hole=uint32_t(centres.size());hole<requested;++hole) {
        bool placed{};
        for(unsigned attempt=0;attempt<20000&&!placed;++attempt) {
            const auto x=uint32_t(margin)+random()%(source.width-2*margin);
            const auto y=uint32_t(margin)+random()%(source.height-2*margin);
            bool clear=true;for(const auto other:centres) {
                const auto dx=int64_t(x)-other.x,dy=int64_t(y)-other.y;
                if(dx*dx+dy*dy<int64_t(minDistance)*minDistance){clear=false;break;}
            }
            if(!clear)continue;
            unsigned solidSamples{};
            constexpr std::array<std::pair<int,int>,9> samples{{{0,0},{-32,0},{32,0},{0,-32},{0,32},{-24,-24},{24,-24},{-24,24},{24,24}}};
            for(const auto [dx,dy]:samples)
                solidSamples+=source.pixels[size_t(int(y)+dy)*source.width+uint32_t(int(x)+dx)].a>=128;
            if(solidSamples<8)continue;
            centres.push_back({x,y});placed=true;
        }
        if(!placed)return false;
    }
    for(uint32_t i=0;i<requested;++i) {
        landPut32(land,32+size_t(i)*8,centres[i].x);landPut32(land,36+size_t(i)*8,centres[i].y);
        for(int dy=-radius;dy<=radius;++dy)for(int dx=-radius;dx<=radius;++dx) {
            // Keep the generated collision mask perfectly circular. Random
            // per-pixel radius changes created the visibly serrated cavities
            // seen on RGBA maps even though their placement was correct.
            if(dx*dx+dy*dy>radius*radius)continue;
            const auto x=uint32_t(int(centres[i].x)+dx),y=uint32_t(int(centres[i].y)+dy);
            land[planes[0].pixels+size_t(y)*planes[0].rowBytes+x]=0;
            for(size_t p=1;p<planes.size();++p)
                land[planes[p].pixels+size_t(y)*planes[p].rowBytes+x/8]&=uint8_t(~(1u<<(x&7)));
        }
    }
    log(std::string(useExisting?"Completed":"Materialized")+" "+std::to_string(requested)+
        " requested circular holes in authoritative LAND after restoring the temporary placement strip");
    return true;
}
std::shared_ptr<const ImportedMap> holeMapFromLand(const std::vector<uint8_t>& land,
                                                   const ImportedMap& pristine,
                                                   const ImportedMap& current,
                                                   bool classificationRestored=false) {
    if(land.size()<32||landU32(land,0)!=0x1a444e4c||landU32(land,8)!=pristine.width||
       landU32(land,12)!=pristine.height)return {};
    const auto locations=landU32(land,28);size_t start=32+size_t(locations)*8;
    if(locations>100000||start+14>land.size()||landU32(land,start)!=0x1a474d49)return {};
    const auto blockSize=landU32(land,start+4);if(blockSize<14||start+blockSize>land.size())return {};
    size_t p=start+8;uint8_t bpp=land[p++];
    if(!bpp)bpp=land[p++];else if(bpp>32){while(p<start+blockSize&&land[p++]){}if(p>=start+blockSize)return {};bpp=land[p++];}
    if(p>=start+blockSize)return {};const auto flags=land[p++];if(flags&0x40)return {};
    if(flags&0x80){if(p+2>start+blockSize)return {};const auto count=uint16_t(land[p])|uint16_t(land[p+1])<<8;p+=2+size_t(count)*3;}
    if(bpp!=8||p+4>start+blockSize)return {};
    const auto width=uint16_t(land[p])|uint16_t(land[p+1])<<8;
    const auto height=uint16_t(land[p+2])|uint16_t(land[p+3])<<8;p+=4;
    if(uint32_t(width)!=pristine.width||uint32_t(height)!=pristine.height||p+size_t(width)*height>start+blockSize)return {};

    Image image=*pristine.artwork();
    const auto restoreRows=classificationRestored?0:(std::min)(nativeProxyEditorTopClearance,image.height);
    for(uint32_t y=restoreRows;y<image.height;++y)for(uint32_t x=0;x<image.width;++x)
        if(land[p+size_t(y)*image.width+x]==0)image.pixels[size_t(y)*image.width+x].a=0;

    auto metadata=extractWaLevelMetadata(current.nativePng);
    auto rgba=writeRgbaPngMemory(image);
    if(auto level=extractWaLevelMetadata(pristine.rgbaPng))rgba=attachWaLevelMetadata(rgba,*level);
    wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
    if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"wkh",0,tempFile))return {};
    struct Cleanup { wchar_t* path;~Cleanup(){DeleteFileW(path);} } cleanup{tempFile};
    writeNativePng(tempFile,image,128,metadata?std::span<const uint8_t>(*metadata):std::span<const uint8_t>{});
    std::ifstream file(tempFile,std::ios::binary|std::ios::ate);const auto length=file.tellg();
    if(!file||length<33||length>128ll*1024*1024)return {};
    std::vector<uint8_t> native(static_cast<size_t>(length));file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(native.data()),length))return {};
    auto result=std::make_shared<ImportedMap>(pristine.source,std::move(image),std::move(rgba),std::move(native));
    result->bordered=current.bordered;result->discardArtwork();return result;
}
std::pair<const uint8_t*,size_t> firstLand8Plane(const std::vector<uint8_t>& land,
                                                 uint32_t expectedWidth,
                                                 uint32_t expectedHeight) {
    if(land.size()<32||landU32(land,0)!=0x1a444e4c||landU32(land,8)!=expectedWidth||
       landU32(land,12)!=expectedHeight)throw std::runtime_error("LAND dimensions do not match canonical artwork");
    const auto locations=landU32(land,28);if(locations>100000)throw std::runtime_error("Invalid LAND locations");
    size_t start=32+size_t(locations)*8;
    if(start+14>land.size()||landU32(land,start)!=0x1a474d49)throw std::runtime_error("Invalid LAND image");
    const auto blockSize=landU32(land,start+4);if(blockSize<14||start+blockSize>land.size())throw std::runtime_error("Invalid LAND image size");
    size_t p=start+8;uint8_t bpp=land[p++];
    if(!bpp)bpp=land[p++];else if(bpp>32){while(p<start+blockSize&&land[p++]){}if(p>=start+blockSize)throw std::runtime_error("Invalid LAND description");bpp=land[p++];}
    if(p>=start+blockSize)throw std::runtime_error("Invalid LAND flags");const auto flags=land[p++];
    if(flags&0x40)throw std::runtime_error("Compressed LAND image is unsupported");
    if(flags&0x80){if(p+2>start+blockSize)throw std::runtime_error("Invalid LAND palette");const auto count=uint16_t(land[p])|uint16_t(land[p+1])<<8;p+=2+size_t(count)*3;}
    if(bpp!=8||p+4>start+blockSize)throw std::runtime_error("LAND collision plane is not 8-bit");
    const auto width=uint16_t(land[p])|uint16_t(land[p+1])<<8;
    const auto height=uint16_t(land[p+2])|uint16_t(land[p+3])<<8;p+=4;
    const auto bytes=size_t(expectedWidth)*expectedHeight;
    if(uint32_t(width)!=expectedWidth||uint32_t(height)!=expectedHeight||p+bytes>start+blockSize)throw std::runtime_error("Invalid LAND collision dimensions");
    return {land.data()+p,bytes};
}
HANDLE serveLandData(const wchar_t* path,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD flags,HANDLE templateFile) {
    if(legacyCompatibilityGameplay.load(std::memory_order_acquire)) {
        struct ReleasePin { ~ReleasePin(){network::canonicalTerrainConsumed();} } releasePin;
        if(!legacyRestoreTop.load(std::memory_order_acquire)) {
            // Mixed session using W:A's original playable proxy. Suppress all
            // offline padding/restoration while leaving the native file
            // untouched, exactly as it was sent to the legacy client.
            return INVALID_HANDLE_VALUE;
        }
        // The mixed-lobby wire PNG restores the disposable classification
        // strip without changing dimensions. Mirror that one idempotent
        // restoration in the host's already-decoded LAND grids. Holes below
        // the strip, borders, object locations and every header field remain
        // W:A-owned, so host and legacy client simulate the same pixels.
        auto& cache=store();std::shared_ptr<const ImportedMap> active;
        {std::lock_guard lock(cache.mutex);active=cache.landBaseMap?cache.landBaseMap:cache.map;}
        if(!active)return INVALID_HANDLE_VALUE;
        auto original=active->artwork();const auto rows=(std::min)(nativeProxyTopClearance(*original),nativeProxyEditorTopClearance);
        if(!rows)return INVALID_HANDLE_VALUE;
        std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return INVALID_HANDLE_VALUE;
        const auto length=file.tellg();if(length<32||length>512ll*1024*1024)return INVALID_HANDLE_VALUE;
        std::vector<uint8_t> land(static_cast<size_t>(length));file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(land.data()),length)||landU32(land,0)!=0x1a444e4c)
            return INVALID_HANDLE_VALUE;
        if(landU32(land,8)!=active->width||landU32(land,12)!=active->height)return INVALID_HANDLE_VALUE;
        auto restored=restoreLandClassificationRows(land,*original,(std::min)(rows,active->height));
        wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
        if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"wkm",0,tempFile))
            throw std::runtime_error("Cannot create mixed-lobby land.dat");
        {std::ofstream out(tempFile,std::ios::binary|std::ios::trunc);
         if(!out.write(reinterpret_cast<const char*>(restored.data()),restored.size()))
             throw std::runtime_error("Cannot write mixed-lobby land.dat");}
        auto result=originalW(tempFile,GENERIC_READ,share|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
                              (flags&~FILE_ATTRIBUTE_NORMAL)|FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,
                              templateFile);
        if(result==INVALID_HANDLE_VALUE)DeleteFileW(tempFile);
        else log("Mixed-lobby LAND restored the source top rows without changing dimensions, holes or placements");
        return result;
    }
    auto& cache=store();std::shared_ptr<const ImportedMap> active,canonicalMap;std::vector<uint8_t> canonicalLand;
    bool canonical{},classificationSeeded{};
    {std::lock_guard lock(cache.mutex);
        canonical=cache.networkCanonicalGameplay;
        classificationSeeded=cache.placementClassificationSeeded;
        if(canonical){canonicalMap=cache.map;canonicalLand=cache.networkLand;}
        active=cache.landBaseMap?cache.landBaseMap:cache.map;
    }
    if(canonical) {
        // The lobby's serialized map buffer and current.thm are already the
        // canonical wire image here.  W:A can nevertheless build land.dat
        // from an older, decoded editor bitmap.  In that case the host would
        // simulate the unpadded height while every client simulates the wire
        // height, causing an immediate desynchronization.  Bring only the
        // serialized terrain grids up to the canonical height; never crop,
        // regenerate, or add a second clearance when W:A already used it.
        struct ReleasePin { ~ReleasePin(){network::canonicalTerrainConsumed();} } releasePin;
        if(!canonicalMap)return INVALID_HANDLE_VALUE;
        if(canonicalLand.size()>=32&&landU32(canonicalLand,0)==0x1a444e4c&&
           landU32(canonicalLand,8)==canonicalMap->width&&landU32(canonicalLand,12)==canonicalMap->height) {
            wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
            if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"wks",0,tempFile))
                throw std::runtime_error("Cannot create synchronized network land.dat");
            {std::ofstream out(tempFile,std::ios::binary|std::ios::trunc);
             if(!out.write(reinterpret_cast<const char*>(canonicalLand.data()),canonicalLand.size()))
                 throw std::runtime_error("Cannot write synchronized network land.dat");}
            auto result=originalW(tempFile,GENERIC_READ,share|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
                                  (flags&~FILE_ATTRIBUTE_NORMAL)|FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,
                                  templateFile);
            if(result==INVALID_HANDLE_VALUE)DeleteFileW(tempFile);
            else log("Exact host-generated land.dat supplied to network terrain: "+
                     std::to_string(canonicalMap->width)+"x"+std::to_string(canonicalMap->height)+
                     " holes="+std::to_string(landU32(canonicalLand,24))+
                     " locations="+std::to_string(landU32(canonicalLand,28)));
            return result;
        }
        std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return INVALID_HANDLE_VALUE;
        const auto length=file.tellg();if(length<32||length>512ll*1024*1024)return INVALID_HANDLE_VALUE;
        std::vector<uint8_t> land(static_cast<size_t>(length));file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(land.data()),length))return INVALID_HANDLE_VALUE;
        if(landU32(land,0)!=0x1a444e4c)return INVALID_HANDLE_VALUE;
        const auto landWidth=landU32(land,8),landHeight=landU32(land,12);
        if(landWidth!=canonicalMap->width || landHeight>canonicalMap->height) {
            log("Canonical network land.dat rejected: native="+std::to_string(landWidth)+"x"+
                std::to_string(landHeight)+" wire="+std::to_string(canonicalMap->width)+"x"+
                std::to_string(canonicalMap->height));
            return INVALID_HANDLE_VALUE;
        }
        if(landHeight==canonicalMap->height) {
            log("W:A network land.dat already matches canonical wire map "+std::to_string(landWidth)+"x"+
                std::to_string(landHeight));
            return INVALID_HANDLE_VALUE;
        }
        const auto rows=canonicalMap->height-landHeight;
        if(classificationSeeded&&active&&landHeight==active->height) {
            const auto classificationRows=(std::min)(rows,nativeProxyEditorTopClearance);
            land=restoreLandClassificationRows(land,*active->artwork(),(std::min)(classificationRows,active->height));
            log("Temporary editor placement strip restored from pristine RGBA collision before canonical padding");
        }
        auto adjustedLand=addLandTopRows(land,rows,!classificationSeeded);
        wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
        if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"wkn",0,tempFile))
            throw std::runtime_error("Cannot create canonical network land.dat");
        {std::ofstream out(tempFile,std::ios::binary|std::ios::trunc);
         if(!out.write(reinterpret_cast<const char*>(adjustedLand.data()),adjustedLand.size()))
             throw std::runtime_error("Cannot write canonical network land.dat");}
        auto result=originalW(tempFile,GENERIC_READ,share|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
                              (flags&~FILE_ATTRIBUTE_NORMAL)|FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,
                              templateFile);
        if(result==INVALID_HANDLE_VALUE)DeleteFileW(tempFile);
        else log("W:A network land.dat aligned with canonical wire map: "+std::to_string(landWidth)+"x"+
                 std::to_string(landHeight)+" -> "+std::to_string(canonicalMap->width)+"x"+
                 std::to_string(canonicalMap->height));
        return result;
    }
    if(!active)return INVALID_HANDLE_VALUE;
    if(!networkGameplayReady.load(std::memory_order_acquire)) {
        // waLV's serialized hole field remains zero for imported RGBA maps
        // even after the editor has baked holes into current.thm/land.dat.
        // Observe LAND's authoritative count and select the already-synced
        // editor map for game start without modifying the editor file itself.
        try {
            std::ifstream editorLand(path,std::ios::binary|std::ios::ate);const auto length=editorLand.tellg();
            if(editorLand&&length>=32&&length<=512ll*1024*1024) {
                std::vector<uint8_t> land(static_cast<size_t>(length));editorLand.seekg(0);
                if(!editorLand.read(reinterpret_cast<char*>(land.data()),length))throw std::runtime_error("Cannot read editor LAND");
                const auto holes=landU32(land,24);
                std::shared_ptr<const ImportedMap> pristine,current;
                {std::lock_guard lock(cache.mutex);pristine=cache.classificationBaseMap;current=cache.map;}
                bool authoritative{};
                if(holes&&classificationSeeded&&pristine) {
                    land=canonicalizeNetworkLand(std::move(land),*pristine->artwork(),pristine->height,false);
                    authoritative=true;
                }
                auto holed=holes&&pristine&&current
                    ?holeMapFromLand(land,*pristine,*current,authoritative)
                    :std::shared_ptr<const ImportedMap>{};
                std::lock_guard lock(cache.mutex);
                if(cache.placementClassificationSeeded&&cache.classificationBaseMap) {
                    cache.editorHoleCount=holes;
                    cache.landBaseMap=holes&&holed?holed:cache.classificationBaseMap;
                    if(holes&&holed)cache.map=holed;
                    log("W:A editor LAND selected "+std::string(holes&&holed?"hole-modified":"pristine")+
                        " RGBA game-start proxy (holes="+std::to_string(holes)+")");
                }
            }
        }catch(...){log("W:A editor LAND state observation skipped safely");}
        log("W:A editor land.dat retained at native dimensions while network game is not started");
        return INVALID_HANDLE_VALUE;
    }
    const auto sourcePath=active->source;Image original=*active->artwork();for(const auto p:original.pixels)if(p.a<128)return INVALID_HANDLE_VALUE;
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return INVALID_HANDLE_VALUE;
    const auto length=file.tellg();if(length<32||length>512ll*1024*1024)return INVALID_HANDLE_VALUE;
    std::vector<uint8_t> land(static_cast<size_t>(length));file.seekg(0);if(!file.read(reinterpret_cast<char*>(land.data()),length))return INVALID_HANDLE_VALUE;
    if(landU32(land,0)!=0x1a444e4c)return INVALID_HANDLE_VALUE;
    const bool topBorder=landU32(land,16)!=0;const auto holes=landU32(land,24);
    if(active->bordered||topBorder) {
        log("W:A land.dat retained at native dimensions: borders="+std::to_string(active->bordered)+
            " topBorder="+std::to_string(topBorder)+" holes="+std::to_string(holes));
        return INVALID_HANDLE_VALUE;
    }
    const auto rows=nativeProxyTopClearance(original);
    if(classificationSeeded&&!holes) {
        const auto classificationRows=(std::min)(rows,nativeProxyEditorTopClearance);
        land=restoreLandClassificationRows(land,original,(std::min)(classificationRows,original.height));
        log("Temporary editor placement strip restored from pristine RGBA collision before local padding");
    }
    auto adjustedLand=addLandTopRows(land,rows,!classificationSeeded);
    auto padded=addTransparentTop(original,rows);auto adjustedNative=resizeIndexedPngTop(active->nativePng,padded.height,0);
    auto rgba=writeRgbaPngMemory(padded);if(auto level=extractWaLevelMetadata(active->rgbaPng))rgba=attachWaLevelMetadata(rgba,*level);
    auto imported=std::make_shared<ImportedMap>(sourcePath,std::move(padded),std::move(rgba),std::move(adjustedNative));imported->discardArtwork();
    {std::lock_guard lock(cache.mutex);cache.map=std::move(imported);}
    wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
    if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"wkl",0,tempFile))throw std::runtime_error("Cannot create adjusted land.dat");
    {std::ofstream out(tempFile,std::ios::binary|std::ios::trunc);if(!out.write(reinterpret_cast<const char*>(adjustedLand.data()),adjustedLand.size()))throw std::runtime_error("Cannot write adjusted land.dat");}
    auto result=originalW(tempFile,GENERIC_READ,share|FILE_SHARE_DELETE,sa,OPEN_EXISTING,(flags&~FILE_ATTRIBUTE_NORMAL)|FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,templateFile);
    if(result==INVALID_HANDLE_VALUE)DeleteFileW(tempFile);else log("W:A borderless land.dat received "+std::to_string(rows)+
        "-pixel top teleport clearance after preserving "+std::to_string(holes)+" generated holes");
    return result;
}
HANDLE WINAPI hookW(LPCWSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD disposition,DWORD flags,HANDLE templateFile) {
    return openNative(path,access,share,sa,disposition,flags,templateFile);
}
HANDLE WINAPI hookA(LPCSTR path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD disposition,DWORD flags,HANDLE templateFile) {
    if(!path)return originalA(path,access,share,sa,disposition,flags,templateFile);
    try {
        const UINT codepage=AreFileApisANSI()?CP_ACP:CP_OEMCP;
        auto size=MultiByteToWideChar(codepage,0,path,-1,nullptr,0);
        if(!size)return originalA(path,access,share,sa,disposition,flags,templateFile);
        std::wstring wide(size,L'\0');MultiByteToWideChar(codepage,0,path,-1,wide.data(),size);
        return openNative(wide.c_str(),access,share,sa,disposition,flags,templateFile);
    }catch(...){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return INVALID_HANDLE_VALUE;}
}
void** findImport(HMODULE module,const char* name) {
    auto* base=reinterpret_cast<uint8_t*>(module);
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    auto dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!dir.VirtualAddress)return nullptr;
    auto* descriptor=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);
    for(;descriptor->Name;++descriptor) {
        if(!descriptor->OriginalFirstThunk)continue;
        auto* names=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+descriptor->OriginalFirstThunk);
        auto* slots=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+descriptor->FirstThunk);
        for(;names->u1.AddressOfData;++names,++slots) {
            if(IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))continue;
            auto* import=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if(strcmp(reinterpret_cast<char*>(import->Name),name)==0)return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}
bool replace(void** slot,void* target) {
    DWORD old{};
    if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old))return false;
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot),target);
    DWORD ignored{};VirtualProtect(slot,sizeof(void*),old,&ignored);return true;
}
bool installImports(HMODULE module) {
    auto a=findImport(module,"CreateFileA"),w=findImport(module,"CreateFileW");
    if(!a||!w){log("CreateFile imports not found");return false;}
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&hookW),&pinned))return false;
    originalA=reinterpret_cast<OpenA>(*a);originalW=reinterpret_cast<OpenW>(*w);
    if(!replace(a,reinterpret_cast<void*>(&hookA)))return false;
    if(!replace(w,reinterpret_cast<void*>(&hookW))){replace(a,reinterpret_cast<void*>(originalA));return false;}
    enabled=true;return true;
}
}
#ifdef WKRGBA_LOADER_TESTING
static HMODULE testHost{};
bool installTestImports(HMODULE module,const std::filesystem::path& root) {
    savedLevels=std::filesystem::weakly_canonical(root);gameRoot=savedLevels.parent_path();
    logPath=gameRoot/L"wkRGBA_loader_test.log";testHost=module;return installImports(module);
}
void uninstallTestImports() {
    if(testHost && enabled) {
        replace(findImport(testHost,"CreateFileA"),reinterpret_cast<void*>(originalA));
        replace(findImport(testHost,"CreateFileW"),reinterpret_cast<void*>(originalW));enabled=false;
    }
}
#endif
bool installed(){return enabled.load();}
void setNetworkGameplayReady(bool ready){networkGameplayReady.store(ready,std::memory_order_release);}
void setLegacyCompatibilityGameplay(bool active,bool restoreTop){
    legacyRestoreTop.store(active&&restoreTop,std::memory_order_release);
    legacyCompatibilityGameplay.store(active,std::memory_order_release);
    if(active)setNetworkCanonicalGameplay(false);
    log(!active?"Legacy network simulation compatibility disabled":
        restoreTop?"Legacy network simulation compatibility enabled with synchronized top restoration":
                   "Legacy network simulation compatibility enabled with exact W:A terrain");
}
std::shared_ptr<const ImportedMap> lastImport(){auto& s=store();std::lock_guard lock(s.mutex);return s.map;}
std::shared_ptr<const ImportedMap> networkImport(){
    auto& s=store();std::lock_guard lock(s.mutex);return s.landBaseMap?s.landBaseMap:s.map;
}
std::shared_ptr<const ImportedMap> networkPristineImport(){
    auto& s=store();std::lock_guard lock(s.mutex);return s.classificationBaseMap?s.classificationBaseMap:(s.landBaseMap?s.landBaseMap:s.map);
}
uint32_t networkEditorHoleCount(){auto& s=store();std::lock_guard lock(s.mutex);return s.editorHoleCount;}
void setNetworkCanonicalLand(std::vector<uint8_t> land){auto& s=store();std::lock_guard lock(s.mutex);s.networkLand=std::move(land);}
std::vector<uint8_t> canonicalizeNetworkLand(std::vector<uint8_t> land,const Image& source,
                                             uint32_t targetHeight,bool bordered) {
    source.validate();
    if(land.size()<32||landU32(land,0)!=0x1a444e4c||landU32(land,8)!=source.width||
       landU32(land,12)!=source.height||targetHeight<source.height||targetHeight>maxDimension)
        throw std::runtime_error("Editor LAND does not match the source RGBA map");
    bool seeded{};{auto& s=store();std::lock_guard lock(s.mutex);seeded=s.placementClassificationSeeded;}
    bool sourceHoleLocations{};
    if(seeded) {
        const auto rows=(std::min)(nativeProxyEditorTopClearance,source.height);
        // The editor's 128-row placement strip is temporary even after W:A
        // adds borders.  Restore the source inside the native eight-pixel
        // frame, preserving all four border planes byte-for-byte.
        land=restoreLandClassificationRows(land,source,rows,bordered);
        sourceHoleLocations=ensureFirstGenerationHoles(land,source);
    }
    if(targetHeight>source.height)
        land=addLandTopRows(land,targetHeight-source.height,sourceHoleLocations||!seeded);
    return land;
}
Image applyNetworkLandCollision(const std::vector<uint8_t>& land,const Image& artwork) {
    artwork.validate();Image result=artwork;
    const auto [plane,bytes]=firstLand8Plane(land,artwork.width,artwork.height);(void)bytes;
    for(size_t i=0;i<result.pixels.size();++i)if(!plane[i])result.pixels[i].a=0;
    return result;
}
bool setNetworkCanonicalGameplay(bool active){
    auto& s=store();std::lock_guard lock(s.mutex);
    if(!active) {
        s.networkCanonicalGameplay=false;
        if(s.landBaseMap)s.map=s.landBaseMap;
        return false;
    }
    auto base=s.landBaseMap?s.landBaseMap:s.map;
    if(!base||base->bordered||base->nativePng.empty())return false;
    auto image=*base->artwork();const auto rows=nativeProxyTopClearance(image);
    if(!rows)return false;
    auto padded=addTransparentTop(image,rows);
    auto native=resizeIndexedPngTop(base->nativePng,padded.height,0);
    auto rgba=writeRgbaPngMemory(padded);
    if(auto level=extractWaLevelMetadata(base->rgbaPng))rgba=attachWaLevelMetadata(rgba,*level);
    auto canonical=std::make_shared<ImportedMap>(base->source,std::move(padded),std::move(rgba),std::move(native));
    canonical->discardArtwork();canonical->bordered=false;
    s.map=std::move(canonical);s.networkCanonicalGameplay=true;
    log("Canonical network map activated before W:A terrain generation ("+std::to_string(rows)+" top rows)");
    return true;
}

bool setNetworkCanonicalGameplayExact(Image image,std::vector<uint8_t> rgbaPng,
                                      std::vector<uint8_t> nativePng){
    image.validate();
    if(nativePng.empty()||!pngDimensionsMatch(nativePng,image.width,image.height))return false;
    auto& s=store();std::lock_guard lock(s.mutex);
    auto base=s.landBaseMap?s.landBaseMap:s.map;if(!base)return false;
    auto canonical=std::make_shared<ImportedMap>(base->source,std::move(image),
                                                  std::move(rgbaPng),std::move(nativePng));
    // ImportedMap reads the border flag from these exact serialized bytes;
    // do not copy a potentially older editor flag from the source snapshot.
    canonical->discardArtwork();
    s.map=std::move(canonical);s.networkCanonicalGameplay=true;
    log("Exact serialized network map activated before W:A terrain generation");
    return true;
}

void __fastcall onEditorNativeState(uintptr_t self,uintptr_t) {
    using Original=void(__fastcall*)(uintptr_t,uintptr_t);
    reinterpret_cast<Original>(editorNativeStateHook.trampoline)(self,0);
    try {
        if(!self||!lastImport())return;
        auto* nativeMode=reinterpret_cast<uint32_t*>(self+0x40B0);
        if(!IsBadReadPtr(nativeMode,sizeof(*nativeMode))&&!IsBadWritePtr(nativeMode,sizeof(*nativeMode))) {
            const auto previous=*nativeMode;*nativeMode=1;
            if(previous!=1)log("RGBA editor native-colour mode corrected (was "+std::to_string(previous)+")");
        }
    }catch(const std::exception& error){log(std::string("RGBA editor-state hook failed: ")+error.what());}
    catch(...){log("RGBA editor-state hook failed");}
}
void acceptImage(Image image,const std::filesystem::path& source,std::vector<uint8_t> rgbaPng,
                 std::vector<uint8_t> nativePng,bool networkCanonical){
    image.validate();auto map=std::make_shared<ImportedMap>(source,std::move(image),std::move(rgbaPng),std::move(nativePng));
    map->discardArtwork();
    auto& s=store();std::lock_guard lock(s.mutex);
    clearThemeProxy(s);
    if(s.proxyKeeper!=INVALID_HANDLE_VALUE){CloseHandle(s.proxyKeeper);s.proxyKeeper=INVALID_HANDLE_VALUE;}
    s.cachedSource.clear();s.proxyPath.clear();s.cachedSize=0;s.borderedMode=false;s.map=std::move(map);s.landBaseMap=s.map;s.classificationBaseMap.reset();s.placementClassificationSeeded=false;s.editorHoleCount=0;
    s.networkCanonicalGameplay=networkCanonical;
}
void clearImport(){
    auto& s=store();std::lock_guard lock(s.mutex);
    clearThemeProxy(s);
    if(s.proxyKeeper!=INVALID_HANDLE_VALUE){CloseHandle(s.proxyKeeper);s.proxyKeeper=INVALID_HANDLE_VALUE;}
    s.cachedSource.clear();s.proxyPath.clear();s.cachedSize=0;s.borderedMode=false;s.map.reset();s.landBaseMap.reset();s.classificationBaseMap.reset();s.networkCanonicalGameplay=false;s.placementClassificationSeeded=false;s.editorHoleCount=0;s.networkLand.clear();
}
HANDLE openNative(const wchar_t* path,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD disposition,DWORD flags,HANDLE templateFile) {
    if(!path || inside)
        return originalW(path,access,share,sa,disposition,flags,templateFile);
    struct Guard { Guard(){inside=true;}~Guard(){inside=false;} } guard;
    try {
        if(isLandData(path) && disposition==OPEN_EXISTING && (access&GENERIC_READ) && !(access&(GENERIC_WRITE|DELETE))) {
            try {
                if(auto adjusted=serveLandData(path,share,sa,flags,templateFile);adjusted!=INVALID_HANDLE_VALUE)return adjusted;
            }catch(const std::exception& e){log(std::string("land.dat transform skipped safely: ")+e.what());}
            catch(...){log("land.dat transform skipped safely");}
        }
        if(isCurrentTheme(path) && disposition==OPEN_EXISTING && (access&GENERIC_READ) && !(access&(GENERIC_WRITE|DELETE))) {
            if(auto adjusted=serveCurrentTheme(path,share,sa,flags,templateFile);adjusted!=INVALID_HANDLE_VALUE)return adjusted;
        }
        if(!candidate(path))return originalW(path,access,share,sa,disposition,flags,templateFile);
        const bool mutating=disposition!=OPEN_EXISTING || (access&(GENERIC_WRITE|DELETE)) ||
                            (flags&FILE_FLAG_DELETE_ON_CLOSE);
        if(mutating) {
            // W:A's editor reopens the selected PNG for writing when borders,
            // holes and the placement state change.  It must update the native
            // indexed working copy, never the user's lossless RGBA source.
            // Keeping those writes on the proxy also makes the subsequent
            // editor/game reload observe exactly the state W:A just produced.
            if(rgbaHeader(path)) {
                const auto canonical=std::filesystem::weakly_canonical(path);
                auto& cache=store();std::lock_guard lock(cache.mutex);
                if(cache.map && cache.proxyKeeper!=INVALID_HANDLE_VALUE &&
                   cache.cachedSource==canonical && !cache.proxyPath.empty()) {
                    const auto proxyFlags=(flags&~FILE_ATTRIBUTE_NORMAL)|FILE_ATTRIBUTE_TEMPORARY;
                    HANDLE result=originalW(cache.proxyPath.c_str(),access,share|FILE_SHARE_DELETE,sa,
                                            disposition,proxyFlags,templateFile);
                    if(result!=INVALID_HANDLE_VALUE)log("RGBA editor write redirected to native working copy");
                    return result;
                }
                log("RGBA editor write requested before native working copy existed");
                SetLastError(ERROR_NOT_SUPPORTED);return INVALID_HANDLE_VALUE;
            }
            return originalW(path,access,share,sa,disposition,flags,templateFile);
        }
        if(!(access&GENERIC_READ) || !rgbaHeader(path)) {
            // Selecting a normal indexed map must retire the previous RGBA
            // association before the editor-load hook runs.
            if(access&GENERIC_READ) {
                auto& cache=store();std::lock_guard lock(cache.mutex);
                if(cache.map && cache.cachedSource!=std::filesystem::weakly_canonical(path)) {
                    if(cache.proxyKeeper!=INVALID_HANDLE_VALUE){CloseHandle(cache.proxyKeeper);cache.proxyKeeper=INVALID_HANDLE_VALUE;}
                    clearThemeProxy(cache);
                    cache.cachedSource.clear();cache.proxyPath.clear();cache.cachedSize=0;cache.borderedMode=false;cache.map.reset();cache.landBaseMap.reset();cache.classificationBaseMap.reset();cache.networkCanonicalGameplay=false;cache.placementClassificationSeeded=false;cache.editorHoleCount=0;
                    log("RGBA editor association cleared for indexed map");
                }
            }
            return originalW(path,access,share,sa,disposition,flags,templateFile);
        }
        const auto importBegin=GetTickCount64();log("RGBA candidate open begin");
        const auto canonical=std::filesystem::weakly_canonical(path);
        const auto sourceSize=std::filesystem::file_size(canonical);
        const auto sourceTime=std::filesystem::last_write_time(canonical);
        const auto editorMetadata=editorLevelMetadata();
        const bool bordered=editorBorderEnabled(editorMetadata).value_or(false);
        auto& cache=store();
        {
            std::lock_guard lock(cache.mutex);
            if(cache.map && cache.proxyKeeper!=INVALID_HANDLE_VALUE && cache.cachedSource==canonical &&
               cache.cachedSize==sourceSize && cache.cachedWriteTime==sourceTime &&
               cache.borderedMode==bordered && !cache.proxyPath.empty()) {
                HANDLE result=originalW(cache.proxyPath.c_str(),GENERIC_READ,share|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
                    FILE_ATTRIBUTE_TEMPORARY,templateFile);
                if(result!=INVALID_HANDLE_VALUE){log("RGBA import served from decoded/native proxy cache in "+std::to_string(GetTickCount64()-importBegin)+" ms");return result;}
            }
        }
        auto decoded=readRgbaPngWithBytes(canonical);
        auto waLevel=extractWaLevelMetadata(decoded.bytes);
        // A regular RGBA PNG has no waLV chunk, but W:A uses that chunk to
        // classify an imported image as a complete custom-level state. Without
        // it the holes are painted in the editor preview while SaveAsCurrent
        // serializes a zero hole count. Reuse the editor's own current 40-byte
        // state on the disposable proxy; dimensions remain those of the source
        // map, so the placement grid and the serialized hole count agree.
        const auto proxyLevel=waLevel?waLevel:editorMetadata;
        uint64_t air{},solid{};
        for(const auto pixel:decoded.image.pixels)(pixel.a<128?air:solid)++;
        log("RGBA collision coverage for "+canonical.filename().string()+": "+
            std::to_string(air)+" air, "+std::to_string(solid)+" solid pixels");
        // Keep the editor working copy at the source dimensions. In
        // particular, W:A's custom-map editor builds its 19x8 hole-placement
        // grid only after normalising the input to 1920x696. Giving it the
        // later gameplay-only top clearance (1920x760 for a full-size map)
        // makes that grid and the holes counter diverge. serveCurrentTheme()
        // adds the placement clearance after the editor has finished.
        if(!air&&!bordered)log("Fully opaque RGBA editor proxy kept at source dimensions; top placement clearance deferred until game start");
        auto imported=std::make_shared<ImportedMap>(canonical,std::move(decoded.image),std::move(decoded.bytes),std::vector<uint8_t>{});
        // The native importer accepts dimensions divisible by eight.
        if(imported->width%8 || imported->height%8)throw std::runtime_error("W:A map dimensions must be multiples of 8");
        wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
        if(!GetTempPathW(MAX_PATH,tempDir) || !GetTempFileNameW(tempDir,L"wkr",0,tempFile))throw std::runtime_error("Cannot create temporary native map");
        const auto levelSpan=proxyLevel?std::span<const uint8_t>(*proxyLevel):std::span<const uint8_t>{};
        const auto classificationRows=!air&&!bordered
            ?(std::min)(nativeProxyTopClearance(*imported->artwork()),nativeProxyEditorTopClearance):0;
        // Cache an exact, uncarved proxy for the wire/gameplay map first.
        writeNativePng(tempFile,*imported->artwork(),128,levelSpan);
        {
            std::ifstream proxy(tempFile,std::ios::binary|std::ios::ate);
            if(!proxy)throw std::runtime_error("Cannot cache native PNG bytes");
            const auto length=proxy.tellg();if(length<33 || length>128ll*1024*1024)throw std::runtime_error("Native PNG outside limits");
            imported->nativePng.resize(size_t(length));proxy.seekg(0);
            if(!proxy.read(reinterpret_cast<char*>(imported->nativePng.data()),length))throw std::runtime_error("Cannot read native PNG cache");
        }
        // Only the editor-facing working copy receives the classification
        // strip. W:A sees enough air during its early placement decision,
        // while the cached native map above remains pixel-complete.
        if(classificationRows) {
            writeNativePng(tempFile,*imported->artwork(),128,levelSpan,classificationRows);
            log("Fully opaque RGBA editor proxy seeded with "+std::to_string(classificationRows)+
                " top air rows for W:A automatic-placement classification");
        }
        imported->discardArtwork();
        HANDLE keeper=originalW(tempFile,DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
            FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,templateFile);
        if(keeper==INVALID_HANDLE_VALUE){DeleteFileW(tempFile);throw std::runtime_error("Cannot retain temporary native map");}
        HANDLE result=originalW(tempFile,GENERIC_READ,share|FILE_SHARE_DELETE,sa,OPEN_EXISTING,
            (flags&~FILE_ATTRIBUTE_NORMAL)|FILE_ATTRIBUTE_TEMPORARY,templateFile);
        if(result==INVALID_HANDLE_VALUE){CloseHandle(keeper);throw std::runtime_error("Cannot reopen temporary native map");}
        {
            std::lock_guard lock(cache.mutex);
            if(cache.proxyKeeper!=INVALID_HANDLE_VALUE)CloseHandle(cache.proxyKeeper);
            clearThemeProxy(cache);
            cache.map=std::move(imported);cache.landBaseMap=cache.map;cache.cachedSource=canonical;cache.cachedSize=sourceSize;
            cache.cachedWriteTime=sourceTime;cache.borderedMode=bordered;cache.networkCanonicalGameplay=false;
            cache.placementClassificationSeeded=classificationRows!=0;
            cache.classificationBaseMap=classificationRows?cache.map:std::shared_ptr<const ImportedMap>{};
            cache.editorHoleCount=0;
            cache.proxyPath=tempFile;cache.proxyKeeper=keeper;
        }
        log("RGBA import decoded once and redirected to cached native PNG in "+std::to_string(GetTickCount64()-importBegin)+" ms");
        return result;
    }catch(const std::exception& e){log(std::string("RGBA import rejected: ")+e.what());SetLastError(ERROR_INVALID_DATA);return INVALID_HANDLE_VALUE;}
    catch(...){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return INVALID_HANDLE_VALUE;}
}
bool install(HMODULE module) {
    try {
        wchar_t exe[32768]{};auto size=GetModuleFileNameW(module,exe,32768);
        if(!size || size==32768)return false;
        std::filesystem::path path(exe);
        if(lower(path.filename().wstring())!=L"wa.exe")return false;
        const auto runtimeDirectory=path.parent_path()/L"wkRGBA";
        // This directory is exclusively runtime output. Starting from an
        // empty directory prevents stale diagnostics from being mistaken for
        // current-session state and keeps the game folder tidy.
        clearRuntimeDirectory(runtimeDirectory);
        logPath=runtimeDirectory/L"wkRGBA_loader.log";
        std::ifstream f(path,std::ios::binary);std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),{});
        const auto digest=net::sha256(data);
        constexpr net::Digest expected{0x10,0xd8,0x17,0x2b,0x57,0x61,0xbb,0x5a,0x1c,0xb4,0xec,0x21,0x9f,0xda,0xca,0x50,
            0x15,0xc0,0xb8,0x6b,0x66,0x38,0xc8,0xdb,0xcc,0x8f,0x5d,0x06,0x18,0xc0,0xa8,0xb9};
        if(digest!=expected){log("Unsupported WA.exe SHA256; no hooks installed");return false;}
        savedLevels=std::filesystem::weakly_canonical(path.parent_path()/L"User"/L"SavedLevels");
        gameRoot=path.parent_path();
        experimental::configure(runtimeDirectory);
        labels::configure(module,runtimeDirectory);
        if(!installImports(module))return false;
        auto* base=reinterpret_cast<uint8_t*>(module);
        const bool editorHook=editorNativeStateHook.install(base+0x8A950,
            {0x55,0x8B,0xEC,0x83,0xE4,0xF8},reinterpret_cast<void*>(&onEditorNativeState));
        log(editorHook?"CMapEditor native-colour state hook installed":"CMapEditor native-colour state hook unavailable");
        if(auto** proc=findImport(module,"GetProcAddress")) {
            originalProc=reinterpret_cast<decltype(originalProc)>(*proc);
            replace(proc,reinterpret_cast<void*>(&hookProc));
        }
        probe::install(module,runtimeDirectory);
        replay::installHooks(module,runtimeDirectory);
        network::installHooks(module,runtimeDirectory);
        log("W:A 3.8.1 loader IAT hooks installed (CreateFileA/CreateFileW)");return true;
    }catch(...){return false;}
}
}
