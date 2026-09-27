#include "ExperimentalScene.h"
#include "D3D9Terrain.h"
#include "NativeCompositor.h"
#include "Loader.h"
#include "Png.h"
#include "WormLabels.h"
#include <array>
#include <atomic>
#include <fstream>
#include <memory>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <wrl/client.h>

namespace wkrgba::experimental {
namespace {
using Create9=IDirect3D9* (WINAPI*)(UINT);
using CreateDevice=HRESULT (WINAPI*)(IDirect3D9*,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS*,IDirect3DDevice9**);
using Present=HRESULT (WINAPI*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
using Reset=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
using EndScene=HRESULT (WINAPI*)(IDirect3DDevice9*);
using CreatePixelShader=HRESULT (WINAPI*)(IDirect3DDevice9*,const DWORD*,IDirect3DPixelShader9**);
using DrawPrimitive=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,UINT);
using DrawIndexedPrimitive=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT);
using DrawPrimitiveUP=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRIMITIVETYPE,UINT,const void*,UINT);
Create9 create9{};CreateDevice createDevice{};Present present{};Reset reset{};EndScene endScene{};
CreatePixelShader createPixelShader{};DrawPrimitive drawPrimitive{};DrawIndexedPrimitive drawIndexedPrimitive{};DrawPrimitiveUP drawPrimitiveUP{};
bool enabled{};
bool captureFinal{};
bool collisionPolarityKnown{};
bool knownCollisionInverted{};
uint64_t finalCaptureStart{};
uint32_t capturedFinalFrames{};
std::atomic<bool> presented{false};
std::filesystem::path logPath;
void log(const std::string& text) noexcept {try{std::ofstream f(logPath,std::ios::app);f<<"t="<<GetTickCount64()<<" "<<text<<'\n';}catch(...){}}
uint64_t ticks() noexcept {LARGE_INTEGER value{};QueryPerformanceCounter(&value);return uint64_t(value.QuadPart);}
struct Timing {
    uint64_t expand{},composite{},present{},maxExpand{},maxComposite{},maxPresent{};uint32_t frames{};bool reported{};
    uint64_t foreground{},upload{},draw{},maxForeground{},maxUpload{},maxDraw{};
} timing;
struct FrameTelemetry {
    uint64_t interval{},maxInterval{},render{},maxRender{},presentCall{},maxPresentCall{},endSceneCall{},maxEndSceneCall{};
    uint32_t frames{},movingFrames{},over16{},over20{},over25{};
};
FrameTelemetry rgbaFullTelemetry,rgbaBlackTelemetry,nativeTelemetry;
uint64_t previousPresentEntry{},renderTicksSincePresent{},endSceneTicksSincePresent{};
int64_t previousCameraLeft{},previousCameraTop{};bool previousCameraValid{};
void addTiming(uint64_t& total,uint64_t& maximum,uint64_t elapsed) noexcept {total+=elapsed;maximum=(std::max)(maximum,elapsed);}
double milliseconds(uint64_t value) noexcept {
    static const double frequency=[] {LARGE_INTEGER result{};QueryPerformanceFrequency(&result);return double(result.QuadPart);}();
    return value*1000.0/frequency;
}
void recordFrameTelemetry(FrameTelemetry& bucket,const char* name,uint64_t interval,uint64_t render,
                          uint64_t presentCall,uint64_t endSceneCall,bool moving) noexcept {
    addTiming(bucket.interval,bucket.maxInterval,interval);addTiming(bucket.render,bucket.maxRender,render);
    addTiming(bucket.presentCall,bucket.maxPresentCall,presentCall);addTiming(bucket.endSceneCall,bucket.maxEndSceneCall,endSceneCall);
    ++bucket.frames;bucket.movingFrames+=moving;
    const auto intervalMs=milliseconds(interval);bucket.over16+=intervalMs>16.7;bucket.over20+=intervalMs>20;bucket.over25+=intervalMs>25;
    if(bucket.frames<300)return;
    try {
        std::ostringstream message;message<<"Frame telemetry "<<name<<": frames="<<bucket.frames<<" moving="<<bucket.movingFrames
            <<", interval avg="<<milliseconds(bucket.interval)/bucket.frames<<" ms max="<<milliseconds(bucket.maxInterval)
            <<" >16.7="<<bucket.over16<<" >20="<<bucket.over20<<" >25="<<bucket.over25
            <<", wkRGBA avg="<<milliseconds(bucket.render)/bucket.frames<<" ms max="<<milliseconds(bucket.maxRender)
            <<", EndScene avg="<<milliseconds(bucket.endSceneCall)/bucket.frames<<" ms max="<<milliseconds(bucket.maxEndSceneCall)
            <<", Present avg="<<milliseconds(bucket.presentCall)/bucket.frames<<" ms max="<<milliseconds(bucket.maxPresentCall);
        log(message.str());
    }catch(...){}
    bucket={};
}
void captureBackbuffer(IDirect3DDevice9* device,const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDirect3DSurface9> backbuffer;
    auto hr=device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&backbuffer);
    if(FAILED(hr))throw std::runtime_error("Cannot acquire final backbuffer");
    D3DSURFACE_DESC desc{};if(FAILED(backbuffer->GetDesc(&desc)))throw std::runtime_error("Cannot inspect final backbuffer");
    if(desc.Format!=D3DFMT_A8R8G8B8&&desc.Format!=D3DFMT_X8R8G8B8)
        throw std::runtime_error("Unsupported final backbuffer format");
    ComPtr<IDirect3DSurface9> resolved;IDirect3DSurface9* source=backbuffer.Get();
    if(desc.MultiSampleType!=D3DMULTISAMPLE_NONE) {
        if(FAILED(device->CreateRenderTarget(desc.Width,desc.Height,desc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&resolved,nullptr)) ||
           FAILED(device->StretchRect(backbuffer.Get(),nullptr,resolved.Get(),nullptr,D3DTEXF_NONE)))
            throw std::runtime_error("Cannot resolve final backbuffer");
        source=resolved.Get();
    }
    ComPtr<IDirect3DSurface9> system;
    if(FAILED(device->CreateOffscreenPlainSurface(desc.Width,desc.Height,desc.Format,D3DPOOL_SYSTEMMEM,&system,nullptr)) ||
       FAILED(device->GetRenderTargetData(source,system.Get())))throw std::runtime_error("Cannot read final backbuffer");
    D3DLOCKED_RECT lock{};if(FAILED(system->LockRect(&lock,nullptr,D3DLOCK_READONLY)))throw std::runtime_error("Cannot lock final backbuffer");
    Image image{desc.Width,desc.Height,{}};image.pixels.resize(checkedArea(desc.Width,desc.Height));
    for(uint32_t y=0;y<desc.Height;++y) {
        const auto* row=static_cast<const uint8_t*>(lock.pBits)+size_t(y)*lock.Pitch;
        for(uint32_t x=0;x<desc.Width;++x)image.pixels[size_t(y)*desc.Width+x]={row[x*4+2],row[x*4+1],row[x*4],255};
    }
    system->UnlockRect();writeRgbaPng(path,image);
}
template<class T> T field(const void* p,size_t offset) {return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(p)+offset);}
bool patch(void** slot,void* hook) {
    if(*slot==hook)return true;
    DWORD old{};if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old))return false;
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot),hook);
    DWORD ignored{};VirtualProtect(slot,sizeof(void*),old,&ignored);return true;
}
struct Frame {
    IDirect3DDevice9* device{}; // borrowed; cleared by reset, never used outside device callbacks
    void* display{};
    uint8_t* pixels{};
    uint32_t width{},height{},stride{};
    float left{},top{};
    bool pending{};
    bool capturing{};
    bool blackBackdrop{};
    uint8_t blackPaletteIndex{};
    bool blackPaletteReady{};
    bool finalBlackoutPending{};
    bool renderedThisPresent{};
    Image background;
    std::vector<uint8_t> backdrop;
    std::vector<uint8_t> visibility;
    std::vector<uint8_t> expectedBase;
    std::vector<uint8_t> blackout;
    std::vector<uint8_t> menuVisited;
    std::vector<uint8_t> menuInk;
    std::vector<uint8_t> hudBlackout;
    bool hudBlackoutReady{};
    uint32_t hudBlackoutBuildPasses{};
    std::array<Pixel,256> palette{};
    std::shared_ptr<const loader::ImportedMap> imported;
    std::shared_ptr<const Image> artwork;
    std::vector<uint8_t> opaqueMask;
    std::vector<uint8_t> solidMask;
    std::vector<uint8_t> edgeCandidates;
    std::vector<uint8_t> edgeRows;
    uint32_t opaqueStride{},solidStride{};
    void* tileSet{};
    const uint8_t* collisionPixels{};
    bool collisionReady{},collisionInverted{};
    D3D9Terrain backGpu;
    NativeCompositor nativeGpu;
    bool integrated{};
    bool logTerrainGeometry{true};
    void discard(){pending=false;capturing=false;}
    void releaseDevice(){discard();display=nullptr;pixels=nullptr;finalBlackoutPending=false;backGpu.clear();nativeGpu.resetResources();device=nullptr;backdrop.clear();visibility.clear();expectedBase.clear();blackout.clear();menuVisited.clear();menuInk.clear();hudBlackout.clear();hudBlackoutReady=false;hudBlackoutBuildPasses=0;collisionPixels=nullptr;collisionReady=false;blackPaletteReady=false;blackPaletteIndex=0;integrated=false;logTerrainGeometry=true;}
};
Frame& frame(){static auto* f=new Frame;return *f;}
void readPalette(std::array<Pixel,256>& out,const void* display) {
    const auto* source=static_cast<const uint8_t*>(display)+0x358D;
    for(size_t i=0;i<out.size();++i)out[i]={source[i*4],source[i*4+1],source[i*4+2],255};
}
struct NativeTerrainView {
    void* display{};
    uint32_t width{},height{},tileWidth{},tileHeight{},columns{},rows{};
    void** bitmaps{};
    const uint8_t* palette{};

    NativeTerrainView(void* d,uint32_t w,uint32_t h):display(d),width(w),height(h) {
        tileWidth=field<uint32_t>(display,0x4DE4);tileHeight=field<uint32_t>(display,0x4DE8);
        auto* set=field<void*>(display,0x4DD8);
        if(!set || !tileWidth || !tileHeight || tileWidth>32768 || tileHeight>32768)
            throw std::runtime_error("Invalid native tile grid");
        bitmaps=field<void**>(set,4);
        if(!bitmaps)throw std::runtime_error("Missing native tile array");
        columns=(width+tileWidth-1)/tileWidth;rows=(height+tileHeight-1)/tileHeight;
        palette=static_cast<const uint8_t*>(display)+0x358D;
    }
    const uint8_t* row(uint32_t x,uint32_t y,uint32_t& available) const {
        if(x>=width || y>=height)throw std::runtime_error("Native terrain coordinate outside map");
        auto* bitmap=bitmaps[size_t(y/tileHeight)*columns+x/tileWidth];
        if(!bitmap)throw std::runtime_error("Missing native tile");
        auto* surface=field<void*>(bitmap,4);
        if(!surface)throw std::runtime_error("Missing native tile surface");
        const auto sw=field<uint32_t>(surface,4),sh=field<uint32_t>(surface,8);
        auto* pixels=field<const uint8_t*>(surface,12);
        const auto localX=x%tileWidth,localY=y%tileHeight;
        if(!pixels || localX>=sw || localY>=sh || sw>32768 || sh>32768)
            throw std::runtime_error("Invalid native tile surface");
        available=(std::min)({width-x,tileWidth-localX,sw-localX});
        return pixels+size_t(localY)*sw+localX;
    }
};
struct LiveTerrainView {
    const uint8_t* pixels{};uint32_t width{},height{},stride{};const uint8_t* palette{};
    LiveTerrainView(void* display,uint32_t expectedWidth,uint32_t expectedHeight) {
        auto* base=static_cast<const uint8_t*>(static_cast<void*>(GetModuleHandleW(nullptr)));
        auto* session=base?field<void*>(base,0x3A0884):nullptr;
        auto* runtime=session?field<void*>(session,0xA0):nullptr;
        auto* landscape=runtime?field<void*>(runtime,0x4CC):nullptr;
        auto* grid=landscape?field<void*>(landscape,0x910):nullptr;
        if(!grid || field<uint32_t>(grid,0x0C)!=8)throw std::runtime_error("Live terrain surface is unavailable");
        pixels=field<const uint8_t*>(grid,0x08);stride=field<uint32_t>(grid,0x10);
        width=field<uint32_t>(grid,0x14);height=field<uint32_t>(grid,0x18);
        if(!pixels || width!=expectedWidth || height!=expectedHeight || stride<width || stride>maxDimension)
            throw std::runtime_error("Live terrain dimensions do not match RGBA map");
        palette=static_cast<const uint8_t*>(display)+0x358D;
    }
    const uint8_t* row(uint32_t y) const {return pixels+size_t(y)*stride;}
};
struct CollisionTerrainView {
    const uint8_t* pixels{};uint32_t stride{};
    CollisionTerrainView(uint32_t expectedWidth,uint32_t expectedHeight) {
        auto* base=static_cast<const uint8_t*>(static_cast<void*>(GetModuleHandleW(nullptr)));
        auto* session=base?field<void*>(base,0x3A0884):nullptr;
        auto* runtime=session?field<void*>(session,0xA0):nullptr;
        auto* landscape=runtime?field<void*>(runtime,0x4CC):nullptr;
        // Landscape::getCollisionHelper() returns a wrapper whose backing store is
        // Landscape+0x914.  This is the mutable 1-bpp land mask updated by terrain
        // edits; GameWorld+0x380 is a different collision/spatial grid which can
        // remain unchanged after an explosion.
        auto* grid=landscape?field<void*>(landscape,0x914):nullptr;
        if(!grid || field<uint32_t>(grid,0x0C)!=1)throw std::runtime_error("Terrain collision surface is unavailable");
        pixels=field<const uint8_t*>(grid,0x08);stride=field<uint32_t>(grid,0x10);
        const auto requiredStride=uint32_t((((uint64_t(expectedWidth)+7)/8)+3)&~uint64_t(3));
        if(!pixels || field<uint32_t>(grid,0x14)!=expectedWidth || field<uint32_t>(grid,0x18)!=expectedHeight ||
           stride<requiredStride || stride>maxDimension/8+4)
            throw std::runtime_error("Terrain collision stride does not cover RGBA map");
    }
    bool raw(uint32_t x,uint32_t y) const {
        return ((pixels[size_t(y)*stride+(x>>3)]>>(x&7))&1)!=0;
    }
    bool solid(uint32_t x,uint32_t y,bool inverted) const {
        return raw(x,y)!=inverted;
    }
};
bool calibrateCollision(const Image& source,const CollisionTerrainView& collision) {
    const uint64_t total=uint64_t(source.width)*source.height;
    const uint64_t samples=(std::min)(total,uint64_t(1'000'000));
    uint64_t normal{},inverted{},solidSamples{};const auto topClearance=nativeProxyTopClearance(source);
    for(uint64_t i=0;i<samples;++i) {
        const auto at=i*total/samples;const auto x=uint32_t(at%source.width),y=uint32_t(at/source.width);
        // W:A may add native terrain (most notably the editor's yellow/black
        // border) in pixels that were transparent in the RGBA source.  Such
        // additions are valid and must not make the RGBA association fail.
        // Original solid pixels, however, still provide an unambiguous sample
        // for determining which collision bit means solid.
        if(source.pixels[size_t(at)].a<128 || x<nativeProxyBorder || y<topClearance ||
           x+nativeProxyBorder>=source.width || y+nativeProxyBorder>=source.height)continue;
        ++solidSamples;
        const bool raw=collision.raw(x,y);
        normal+=raw;inverted+=!raw;
    }
    const auto best=(std::max)(normal,inverted);
    if(!solidSamples || best*100<solidSamples*90)
        throw std::runtime_error("Physics collision grid does not correlate with RGBA terrain");
    std::ostringstream message;message<<"Collision polarity calibrated: bit "<<(inverted>normal?0:1)
        <<" is solid ("<<best<<'/'<<solidSamples<<" original-solid samples matched; native additions allowed)";log(message.str());
    return inverted>normal;
}
bool matchesNativeProxy(const Image& source,const NativeTerrainView& native) {
    source.validate();
    const uint64_t total=uint64_t(source.width)*source.height;
    const uint64_t samples=(std::min)(total,uint64_t(1'000'000));
    uint64_t solidSamples{},matched{};const auto topClearance=nativeProxyTopClearance(source);
    for(uint64_t i=0;i<samples;++i) {
        const auto at=i*total/samples;const auto x=uint32_t(at%source.width),y=uint32_t(at/source.width);
        const auto p=source.pixels[size_t(at)];
        // Borders and other native editor additions occupy source-transparent
        // pixels.  Ignore those pixels and identify the map from the colours
        // of its original solid terrain instead.
        // W:A stamps indestructible borders into an exact 8-pixel edge band,
        // including over opaque imported artwork.  The interior remains the
        // strong identity signal for the RGBA association.
        if(p.a<128 || x<nativeProxyBorder || y<topClearance ||
           x+nativeProxyBorder>=source.width || y+nativeProxyBorder>=source.height)continue;
        ++solidSamples;
        uint32_t available{};const auto index=*native.row(x,y,available);
        auto* n=native.palette+size_t(index)*4;
        const auto expected=nativeProxyColour(p);
        matched+=index && n[0]==expected.r && n[1]==expected.g && n[2]==expected.b;
    }
    // A narrow native border can overwrite a small number of edge samples.
    // Requiring 95% retains a strong map identity check while accepting that
    // deliberate W:A transformation.
    return solidSamples && matched*100>=solidSamples*95;
}
bool matchesProxyColour(Pixel source,uint8_t index,const std::array<Pixel,256>& palette) noexcept {
    if(source.a<128)return index==0;
    if(!index)return false;const auto native=palette[index];
    const auto expected=nativeProxyColour(source);
    return native.r==expected.r && native.g==expected.g && native.b==expected.b;
}
void compositeArtwork(Image& frame,const std::vector<uint8_t>& backdrop,const Image& source,const LiveTerrainView& live,const CollisionTerrainView& collision,
                      const std::array<Pixel,256>& palette,
                      const std::vector<uint8_t>& opaqueMask,uint32_t opaqueStride,
                      bool collisionInverted,
                      int64_t left,int64_t top) {
    const auto area=checkedArea(frame.width,frame.height);if(backdrop.size()!=area)throw std::runtime_error("Backdrop size mismatch");
    if(frame.pixels.size()!=area)frame.pixels.resize(area);
    for(uint32_t dy=0;dy<frame.height;++dy) {
        const int64_t sourceY=int64_t(dy)-top;auto* destinationRow=frame.pixels.data()+size_t(dy)*frame.width;
        const auto* backdropRow=backdrop.data()+size_t(dy)*frame.width;
        const auto paintBackground=[&](uint32_t from,uint32_t to){for(uint32_t x=from;x<to;++x)destinationRow[x]=palette[backdropRow[x]];};
        if(sourceY<0||sourceY>=source.height){paintBackground(0,frame.width);continue;}
        const auto sy=uint32_t(sourceY);
        const auto screenStart64=(std::max)(int64_t(0),left);
        const auto screenEnd64=(std::min)(int64_t(frame.width),left+source.width);
        if(screenEnd64<=screenStart64){paintBackground(0,frame.width);continue;}
        const auto screenStart=uint32_t(screenStart64),screenEnd=uint32_t(screenEnd64);
        paintBackground(0,screenStart);paintBackground(screenEnd,frame.width);
        const auto x0=uint32_t(int64_t(screenStart)-left),x1=uint32_t(int64_t(screenEnd)-left);
        const auto* indices=live.row(sy);const auto* sourceRow=source.pixels.data()+size_t(sy)*source.width;
        const auto* collisionRow=collision.pixels+size_t(sy)*collision.stride;
        const auto render=[&](uint32_t sx) {
            const auto dx=uint32_t(left+sx);auto& destination=destinationRow[dx];
            if(!collision.solid(sx,sy,collisionInverted)){destination=palette[backdropRow[dx]];return;}
            const auto original=sourceRow[sx];const auto output=original.a>=128?original:palette[indices[sx]];
            destination=output.a==255?output:overOpaque(output,palette[backdropRow[dx]]);
        };
        uint32_t sx=x0,end=x1;
        for(;sx<end && (sx&7);++sx)render(sx);
        for(;sx+8<=end;sx+=8) {
            uint8_t bits=collisionRow[sx>>3];if(collisionInverted)bits=uint8_t(~bits);
            if(!bits){const auto dx=uint32_t(left+sx);for(uint32_t i=0;i<8;++i)destinationRow[dx+i]=palette[backdropRow[dx+i]];continue;}
            if(bits==0xff && opaqueStride && opaqueMask[size_t(sy)*opaqueStride+(sx>>3)]==0xff) {
                std::memcpy(destinationRow+size_t(left+sx),sourceRow+sx,8*sizeof(Pixel));continue;
            }
            for(uint32_t bit=0;bit<8;++bit)render(sx+bit);
        }
        for(;sx<end;++sx)render(sx);
    }
}
void renderPending(IDirect3DDevice9* d) noexcept {
    auto& f=frame();
    if(!f.pending||f.device!=d)return;
    const auto presentBegin=ticks();f.pending=false;
    try {
        CollisionTerrainView collision(f.artwork->width,f.artwork->height);
        const auto area=checkedArea(f.width,f.height);if(f.visibility.size()!=area)f.visibility.resize(area);
        static const auto expandBits=[] {
            std::array<std::array<uint8_t,8>,256> table{};
            for(uint32_t bits=0;bits<256;++bits)for(uint32_t bit=0;bit<8;++bit)table[bits][bit]=(bits&(1u<<bit))?255:0;
            return table;
        }();
        for(uint32_t y=0;y<f.height;++y) {
            auto* output=f.visibility.data()+size_t(y)*f.width;
            const auto* current=f.pixels+size_t(y)*f.stride;
            const auto* background=f.expectedBase.data()+size_t(y)*f.width;
            const int64_t sy=int64_t(y)-int64_t(f.top);
            if(sy<0||sy>=f.artwork->height){std::memset(output,0,f.width);continue;}
            std::memset(output,0,f.width);
            const int64_t start64=(std::max)(int64_t(0),int64_t(f.left));
            const int64_t end64=(std::min)(int64_t(f.width),int64_t(f.left)+f.artwork->width);
            if(end64<=start64)continue;
            uint32_t dx=uint32_t(start64),sx=uint32_t(start64-int64_t(f.left)),end=uint32_t(end64);
            const auto* collisionRow=collision.pixels+size_t(sy)*collision.stride;
            const auto one=[&](uint32_t screenX,uint32_t sourceX) {
                output[screenX]=(current[screenX]==background[screenX]&&collision.solid(sourceX,uint32_t(sy),f.collisionInverted))?255:0;
            };
            for(;dx<end&&(sx&7);++dx,++sx)one(dx,sx);
            for(;dx+8<=end;dx+=8,sx+=8) {
                uint8_t bits=collisionRow[sx>>3];if(f.collisionInverted)bits=uint8_t(~bits);
                uint64_t now{},behind{};std::memcpy(&now,current+dx,8);std::memcpy(&behind,background+dx,8);
                if(now==behind)std::memcpy(output+dx,expandBits[bits].data(),8);
                else for(uint32_t bit=0;bit<8;++bit)output[dx+bit]=((bits&(1u<<bit))&&current[dx+bit]==background[dx+bit])?255:0;
            }
            for(;dx<end;++dx,++sx)one(dx,sx);
        }
        labels::protect(f.visibility,nullptr,f.width,f.height,int64_t(f.left),int64_t(f.top),
                        field<int32_t>(f.display,0x3560),field<int32_t>(f.display,0x3564),
                        f.imported&&f.imported->bordered);
        const auto foregroundEnd=ticks();addTiming(timing.foreground,timing.maxForeground,foregroundEnd-presentBegin);
        // The native scene already contains the restored background and all
        // foreground sprites. Overlay cached RGBA tiles only where the live
        // collision mask still contains terrain and no foreground pixel exists.
        const auto uploadEnd=ticks();addTiming(timing.upload,timing.maxUpload,uploadEnd-foregroundEnd);
        f.backGpu.drawMasked(d,*f.artwork,f.visibility,nullptr,f.width,f.height,int64_t(f.left),int64_t(f.top));
        f.finalBlackoutPending=false;
        const auto drawEnd=ticks();addTiming(timing.draw,timing.maxDraw,drawEnd-uploadEnd);presented=true;f.renderedThisPresent=true;
        static bool reported=false;if(!reported){log("RGBA composited inside W:A Direct3D scene");reported=true;}
    }catch(const std::exception& e){log(e.what());enabled=false;presented=false;}
    f.discard();
    const auto elapsed=ticks()-presentBegin;addTiming(timing.present,timing.maxPresent,elapsed);
    renderTicksSincePresent+=elapsed;
    if(++timing.frames==300&&!timing.reported) {
        timing.reported=true;std::ostringstream message;message<<"Renderer timing over 300 frames: expand avg="
            <<milliseconds(timing.expand)/timing.frames<<" ms max="<<milliseconds(timing.maxExpand)
            <<", composite avg="<<milliseconds(timing.composite)/timing.frames<<" ms max="<<milliseconds(timing.maxComposite)
            <<", in-scene render avg="<<milliseconds(timing.present)/timing.frames<<" ms max="<<milliseconds(timing.maxPresent)
            <<" (foreground="<<milliseconds(timing.foreground)/timing.frames<<", upload="<<milliseconds(timing.upload)/timing.frames
            <<", draw="<<milliseconds(timing.draw)/timing.frames<<")";
        log(message.str());
    }
}
HRESULT WINAPI onEndScene(IDirect3DDevice9* d) {
    renderPending(d);
    const auto begin=ticks();const auto result=endScene(d);endSceneTicksSincePresent+=ticks()-begin;return result;
}
HRESULT WINAPI onPresent(IDirect3DDevice9* d,const RECT* src,const RECT* dst,HWND window,const RGNDATA* dirty) {
    const auto presentEntry=ticks();const auto interval=previousPresentEntry?presentEntry-previousPresentEntry:0;previousPresentEntry=presentEntry;
    // Fallback for renderer paths which present without a matching EndScene.
    if(frame().pending&&frame().device==d&&SUCCEEDED(d->BeginScene())) {
        renderPending(d);endScene(d);
    }
    // W:A's shader renderer can compose worm labels after the terrain
    // EndScene. Repair their black interiors on the final backbuffer, after
    // every game pass and immediately before Present.
    auto& f=frame();
    if(captureFinal&&f.blackBackdrop&&f.renderedThisPresent&&!finalCaptureStart)finalCaptureStart=GetTickCount64();
    const bool saveFinal=captureFinal&&f.blackBackdrop&&f.renderedThisPresent&&capturedFinalFrames<4&&finalCaptureStart&&
        GetTickCount64()-finalCaptureStart>=uint64_t(capturedFinalFrames+1)*1500;
    const auto captureIndex=capturedFinalFrames;
    if(saveFinal)try {captureBackbuffer(d,logPath.parent_path()/(L"wkRGBA_final_"+std::to_wstring(captureIndex)+L"_before.png"));}
        catch(const std::exception& e){log(std::string("Final pre-mask capture failed: ")+e.what());}
    if(f.finalBlackoutPending&&f.device==d) {
        f.finalBlackoutPending=false;
        if(saveFinal)try {
            const auto area=checkedArea(f.width,f.height);Image indexed{f.width,f.height,{}};Image mask{f.width,f.height,{}};
            indexed.pixels.resize(area);mask.pixels.resize(area);
            for(uint32_t y=0;y<f.height;++y)for(uint32_t x=0;x<f.width;++x) {
                const auto at=size_t(y)*f.width+x;indexed.pixels[at]=f.palette[f.pixels[size_t(y)*f.stride+x]];
                const auto value=f.blackout[at];mask.pixels[at]={value,value,value,255};
            }
            const auto root=logPath.parent_path();
            writeRgbaPng(root/(L"wkRGBA_final_"+std::to_wstring(captureIndex)+L"_indexed.png"),indexed);
            writeRgbaPng(root/(L"wkRGBA_final_"+std::to_wstring(captureIndex)+L"_blackout.png"),mask);
        } catch(const std::exception& e){log(std::string("Final indexed capture failed: ")+e.what());}
        if(SUCCEEDED(d->BeginScene())) {
            try {f.backGpu.drawFinalBlackout(d,f.blackout);} catch(const std::exception& e){log(std::string("Final UI blackout failed: ")+e.what());}
            endScene(d);
        }
    }
    if(saveFinal) {
        try {captureBackbuffer(d,logPath.parent_path()/(L"wkRGBA_final_"+std::to_wstring(captureIndex)+L"_after.png"));
            log("Captured final backbuffer pair "+std::to_string(captureIndex));}
        catch(const std::exception& e){log(std::string("Final post-mask capture failed: ")+e.what());}
        ++capturedFinalFrames;
    }
    const bool rgbaFrame=f.renderedThisPresent;
    const auto cameraLeft=int64_t(f.left),cameraTop=int64_t(f.top);
    const bool moving=rgbaFrame&&previousCameraValid&&(cameraLeft!=previousCameraLeft||cameraTop!=previousCameraTop);
    if(rgbaFrame){previousCameraLeft=cameraLeft;previousCameraTop=cameraTop;previousCameraValid=true;}
    const auto presentBegin=ticks();const auto result=present(d,src,dst,window,dirty);const auto presentElapsed=ticks()-presentBegin;
    if(interval) {
        if(rgbaFrame)recordFrameTelemetry(f.blackBackdrop?rgbaBlackTelemetry:rgbaFullTelemetry,
            f.blackBackdrop?"rgba-black":"rgba-full",interval,renderTicksSincePresent,presentElapsed,endSceneTicksSincePresent,moving);
        else recordFrameTelemetry(nativeTelemetry,"native",interval,0,presentElapsed,endSceneTicksSincePresent,false);
    }
    f.renderedThisPresent=false;renderTicksSincePresent=0;endSceneTicksSincePresent=0;return result;
}
HRESULT WINAPI onReset(IDirect3DDevice9* d,D3DPRESENT_PARAMETERS* p) {
    // DEFAULT-pool textures must be released, but the decoded PNG and its
    // association with the mutable native terrain must survive device loss.
    // Revalidating against an already destroyed proxy would reject it.
    log("D3D9 reset begin");presented=false;frame().releaseDevice();auto hr=reset(d,p);
    if(SUCCEEDED(hr)){frame().device=d;log("D3D9 reset completed");}else log("D3D9 reset failed");return hr;
}
HRESULT WINAPI onCreatePixelShader(IDirect3DDevice9* d,const DWORD* function,IDirect3DPixelShader9** out) {
    const auto result=createPixelShader(d,function,out);
    if(SUCCEEDED(result)&&out&&*out&&frame().nativeGpu.observePixelShader(d,function,*out))
        log("W:A palette shader identified; single-pass RGBA compositor ready");
    return result;
}
bool beginIntegratedDraw(IDirect3DDevice9* d) noexcept {
    auto& f=frame();if(!f.integrated)return false;
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> selected;
    if(FAILED(d->GetPixelShader(&selected)))return false;
    try {
        if(!f.nativeGpu.isNativeDraw(selected.Get()))return false;
        f.nativeGpu.buildFinalMask(f.pixels,f.stride,f.width,f.height,f.visibility);
        f.blackout.resize(size_t(f.width)*f.height);
        labels::protect(f.visibility,&f.blackout,f.width,f.height,int64_t(f.left),int64_t(f.top),
                        field<int32_t>(f.display,0x3560),field<int32_t>(f.display,0x3564),
                        f.imported&&f.imported->bordered);
        f.nativeGpu.uploadFinalMask(d,f.visibility,f.width,f.height);
        if(!f.nativeGpu.beginNativeDraw(d,selected.Get()))return false;
        // The native palette conversion is one fullscreen draw. Consume the
        // prepared frame so a later menu/preview pass cannot reuse stale map
        // coordinates when no terrain pass preceded it.
        frame().integrated=false;return true;
    }
    catch(const std::exception& e){log(std::string("Native compositor bind failed: ")+e.what());frame().integrated=false;return false;}
}
HRESULT WINAPI onDrawPrimitive(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,UINT start,UINT count) {
    const bool integrated=beginIntegratedDraw(d);const auto result=drawPrimitive(d,type,start,count);
    if(integrated)frame().nativeGpu.endNativeDraw(d);return result;
}
HRESULT WINAPI onDrawIndexedPrimitive(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,INT base,UINT minimum,UINT vertices,UINT start,UINT count) {
    const bool integrated=beginIntegratedDraw(d);const auto result=drawIndexedPrimitive(d,type,base,minimum,vertices,start,count);
    if(integrated)frame().nativeGpu.endNativeDraw(d);return result;
}
HRESULT WINAPI onDrawPrimitiveUP(IDirect3DDevice9* d,D3DPRIMITIVETYPE type,UINT count,const void* vertices,UINT stride) {
    const bool integrated=beginIntegratedDraw(d);const auto result=drawPrimitiveUP(d,type,count,vertices,stride);
    if(integrated)frame().nativeGpu.endNativeDraw(d);return result;
}
HRESULT WINAPI onCreateDevice(IDirect3D9* api,UINT adapter,D3DDEVTYPE type,HWND window,DWORD flags,D3DPRESENT_PARAMETERS* params,IDirect3DDevice9** out) {
    auto hr=createDevice(api,adapter,type,window,flags,params,out);
    if(SUCCEEDED(hr) && out && *out && enabled) {
        auto** v=*reinterpret_cast<void***>(*out);
        if(!present)present=reinterpret_cast<Present>(v[17]);
        if(!reset)reset=reinterpret_cast<Reset>(v[16]);
        if(!endScene)endScene=reinterpret_cast<EndScene>(v[42]);
        if(!createPixelShader)createPixelShader=reinterpret_cast<CreatePixelShader>(v[106]);
        if(!drawPrimitive)drawPrimitive=reinterpret_cast<DrawPrimitive>(v[81]);
        if(!drawIndexedPrimitive)drawIndexedPrimitive=reinterpret_cast<DrawIndexedPrimitive>(v[82]);
        if(!drawPrimitiveUP)drawPrimitiveUP=reinterpret_cast<DrawPrimitiveUP>(v[83]);
        if((v[17]==reinterpret_cast<void*>(present)||v[17]==reinterpret_cast<void*>(&onPresent)) &&
           (v[16]==reinterpret_cast<void*>(reset)||v[16]==reinterpret_cast<void*>(&onReset)) &&
           (v[42]==reinterpret_cast<void*>(endScene)||v[42]==reinterpret_cast<void*>(&onEndScene)) &&
           (v[106]==reinterpret_cast<void*>(createPixelShader)||v[106]==reinterpret_cast<void*>(&onCreatePixelShader)) &&
           (v[81]==reinterpret_cast<void*>(drawPrimitive)||v[81]==reinterpret_cast<void*>(&onDrawPrimitive)) &&
           (v[82]==reinterpret_cast<void*>(drawIndexedPrimitive)||v[82]==reinterpret_cast<void*>(&onDrawIndexedPrimitive)) &&
           (v[83]==reinterpret_cast<void*>(drawPrimitiveUP)||v[83]==reinterpret_cast<void*>(&onDrawPrimitiveUP)) &&
           patch(v+17,reinterpret_cast<void*>(&onPresent)) && patch(v+16,reinterpret_cast<void*>(&onReset)) &&
           patch(v+42,reinterpret_cast<void*>(&onEndScene)) && patch(v+106,reinterpret_cast<void*>(&onCreatePixelShader)) &&
           patch(v+81,reinterpret_cast<void*>(&onDrawPrimitive)) && patch(v+82,reinterpret_cast<void*>(&onDrawIndexedPrimitive)) &&
           patch(v+83,reinterpret_cast<void*>(&onDrawPrimitiveUP))) {
            frame().device=*out;log("Experimental D3D9 device acquired");
        }
    }
    return hr;
}
IDirect3D9* WINAPI onCreate9(UINT version) {
    auto* result=create9(version);
    if(result && enabled) {
        auto** v=*reinterpret_cast<void***>(result);
        if(!createDevice)createDevice=reinterpret_cast<CreateDevice>(v[16]);
        if(v[16]==reinterpret_cast<void*>(createDevice))patch(v+16,reinterpret_cast<void*>(&onCreateDevice));
    }
    return result;
}
}
void configure(const std::filesystem::path& dir) {
    enabled=true;
    captureFinal=std::filesystem::exists(dir/L"wkRGBA_capture_final");finalCaptureStart=0;capturedFinalFrames=0;
    logPath=dir/L"wkRGBA_renderer.log";
    log("RGBA scene renderer enabled");
}
bool hasPresented() noexcept {return presented.load();}
bool blackBackdropActive() noexcept {return frame().blackBackdrop;}
uint8_t opaqueBlackPaletteIndex() noexcept {return frame().blackPaletteIndex;}
FARPROC resolve(HMODULE library,LPCSTR name,FARPROC original) noexcept {
    if(enabled && uintptr_t(name)>65535 && library==GetModuleHandleW(L"d3d9.dll") &&
       std::strcmp(name,"Direct3DCreate9")==0 && original) {
        create9=reinterpret_cast<Create9>(original);log("Direct3DCreate9 intercepted");
        return reinterpret_cast<FARPROC>(&onCreate9);
    }
    return original;
}
bool beginTerrainPass(void* display,int x,int y,int /*rows*/,unsigned /*flags*/) noexcept {
    try {
        auto& f=frame();
        if(!enabled || !f.device || f.pending || f.capturing)return false;
        auto imported=loader::lastImport();if(!imported)return false;
        auto mw=field<uint32_t>(display,0x4DDC),mh=field<uint32_t>(display,0x4DE0);
        // InitBorders changes both the tiled-draw flags and the row span.  They
        // describe how W:A is drawing the native layer, not which map owns the
        // layer, and rejecting them makes the renderer fall back to the indexed
        // proxy.  Dimensions plus the validated terrain/grid objects below are
        // the stable identity for this dedicated DrawTiledTerrain hook.
        if(mw!=imported->width || mh!=imported->height)return false;
        auto* tiles=field<void*>(display,0x4DD8);
        NativeTerrainView native(display,mw,mh);
        if(f.imported!=imported) {
            const auto associationBegin=GetTickCount64();log("RGBA artwork association begin");
            auto artwork=imported->artwork();
            // lastImport is retired as soon as W:A opens an indexed map, and
            // dimensions were checked above.  Those are the durable identity
            // signals.  Pixel matching is not: editor borders and generated
            // holes deliberately rewrite the indexed proxy before gameplay.
            f.imported=imported;f.artwork=std::move(artwork);f.tileSet=tiles;f.collisionPixels=nullptr;f.collisionReady=false;f.backGpu.clear();
            f.logTerrainGeometry=true;
            f.blackPaletteReady=false;f.blackPaletteIndex=0;
            f.opaqueStride=(f.artwork->width+7)/8;f.opaqueMask.assign(size_t(f.opaqueStride)*f.artwork->height,0);
            f.solidStride=f.opaqueStride;f.solidMask.assign(size_t(f.solidStride)*f.artwork->height,0);
            for(uint32_t sy=0;sy<f.artwork->height;++sy)for(uint32_t sx=0;sx<f.artwork->width;++sx) {
                const auto at=size_t(sy)*f.artwork->width+sx;const auto p=f.artwork->pixels[at];
                if(p.a==255)f.opaqueMask[size_t(sy)*f.opaqueStride+(sx>>3)]|=uint8_t(1u<<(sx&7));
                if(p.a>=128)f.solidMask[size_t(sy)*f.solidStride+(sx>>3)]|=uint8_t(1u<<(sx&7));
            }
            log("RGBA artwork associated with current native terrain in "+std::to_string(GetTickCount64()-associationBegin)+" ms (editor mutations allowed)");
        } else if(f.tileSet!=tiles) {
            // W:A may rebuild its native tile set after terrain edits or a
            // renderer transition. The imported artwork is still the same;
            // comparing the already modified native terrain with the pristine
            // PNG would reject it and fall back to the 112-colour proxy.
            f.tileSet=tiles;f.collisionPixels=nullptr;f.collisionReady=false;
            log("Native tile set changed; preserved existing RGBA artwork association");
        }
        auto* grid=field<void*>(display,0x3D9C);if(!grid || field<uint32_t>(grid,0xC)!=8)return false;
        // DisplayBitGrid keeps the physical surface dimensions at +14/+18.
        // +1C..+28 is W:A's mutable clipping rectangle; HUDs, chat and menus
        // temporarily change its bottom edge without resizing the framebuffer.
        // Treating clipRight/clipBottom as width/height truncated the capture
        // and shifted every foreground overlay drawn below that edge.
        auto w=field<uint32_t>(grid,0x14),h=field<uint32_t>(grid,0x18),stride=field<uint32_t>(grid,0x10);
        const auto clipLeft=field<uint32_t>(grid,0x1C),clipTop=field<uint32_t>(grid,0x20);
        const auto clipRight=field<uint32_t>(grid,0x24),clipBottom=field<uint32_t>(grid,0x28);
        auto* pixels=field<uint8_t*>(grid,8);
        if(!pixels || !w || !h || w>8192 || h>8192 || stride<w || stride>32768 ||
           clipLeft>clipRight || clipRight>w || clipTop>clipBottom || clipBottom>h)return false;
        const auto expandBegin=ticks();readPalette(f.palette,display);
        f.integrated=f.nativeGpu.available();
        const auto area=checkedArea(w,h);
        if(f.integrated) {
            f.nativeGpu.captureBackdrop(f.device,pixels,stride,w,h);
            uint64_t blackCount{},samples{};
            for(uint32_t row=0;row<h;row+=8)for(uint32_t column=0;column<w;column+=8) {
                const auto p=f.palette[pixels[size_t(row)*stride+column]];
                blackCount+=p.r<=8&&p.g<=8&&p.b<=8;++samples;
            }
            f.blackBackdrop=samples&&blackCount*100>=samples*95;
            f.finalBlackoutPending=false;
        } else {
            if(f.backdrop.size()!=area)f.backdrop.resize(area);
            for(uint32_t row=0;row<h;++row)std::memcpy(f.backdrop.data()+size_t(row)*w,pixels+size_t(row)*stride,w);
        }
        // Use a black palette entry that the current backdrop does not use.
        // It renders as the same pure black, but current!=expected keeps the
        // RGBA terrain compositor from painting over textbox interiors while
        // the camera or pointer is moving.
        if(!f.blackPaletteReady) {
            std::array<uint32_t,256> usage{};
            if(f.integrated)for(uint32_t row=0;row<h;++row)for(uint32_t column=0;column<w;++column)
                ++usage[pixels[size_t(row)*stride+column]];
            else for(const auto index:f.backdrop)++usage[index];
            unsigned bestScore=~0u;f.blackPaletteIndex=0;
            for(uint32_t index=1;index<f.palette.size();++index)if(!usage[index]) {
                const auto p=f.palette[index];const unsigned score=unsigned(p.r)+p.g+p.b;
                if(score<bestScore){bestScore=score;f.blackPaletteIndex=uint8_t(index);}
            }
            if(!f.blackPaletteIndex) {
                uint32_t leastUsage=~0u;
                for(uint32_t index=1;index<f.palette.size();++index) {
                    const auto p=f.palette[index];const unsigned score=unsigned(p.r)+p.g+p.b;
                    if(score<bestScore||(score==bestScore&&usage[index]<leastUsage)) {
                        bestScore=score;leastUsage=usage[index];f.blackPaletteIndex=uint8_t(index);
                    }
                }
            }
            f.blackPaletteReady=f.blackPaletteIndex!=0;
        }
        const auto expandElapsed=ticks()-expandBegin;addTiming(timing.expand,timing.maxExpand,expandElapsed);
        if(f.integrated)renderTicksSincePresent+=expandElapsed;
        const int64_t mapLeft=int64_t(field<int32_t>(display,0x3560))+int64_t(x>>16);
        const int64_t mapTop=int64_t(field<int32_t>(display,0x3564))+int64_t(y>>16);
        if(f.logTerrainGeometry) {
            std::ostringstream geometry;geometry<<"Terrain geometry: map="<<mw<<'x'<<mh<<", framebuffer="<<w<<'x'<<h
                <<", clip="<<clipLeft<<','<<clipTop<<'-'<<clipRight<<','<<clipBottom
                <<", origin="<<mapLeft<<','<<mapTop<<", bordered="<<(imported->bordered?1:0)
                <<", integrated="<<(f.integrated?1:0);
            log(geometry.str());f.logTerrainGeometry=false;
        }
        labels::ensureInstalled();
        f.display=display;f.pixels=pixels;f.width=w;f.height=h;f.stride=stride;
        f.left=float(mapLeft);f.top=float(mapTop);f.capturing=true;return true;
    }catch(const std::exception& e){log(e.what());return false;}
}
void completeTerrainPass() noexcept {
    auto& f=frame();if(!f.capturing)return;f.capturing=false;
    const auto compositeBegin=ticks();
    try {
        if(!f.imported || !f.display || !f.pixels)return;
        if(!f.artwork)return;
        LiveTerrainView live(f.display,f.artwork->width,f.artwork->height);
        CollisionTerrainView collision(f.artwork->width,f.artwork->height);
        if(!f.collisionReady || f.collisionPixels!=collision.pixels) {
            const auto calibrationBegin=GetTickCount64();log("Collision calibration begin");
            if(f.imported->bordered&&collisionPolarityKnown) {
                f.collisionInverted=knownCollisionInverted;
                log(std::string("Bordered map reused verified W:A collision polarity: bit ")+
                    (f.collisionInverted?"0":"1")+" is solid");
            } else try {
                f.collisionInverted=calibrateCollision(*f.artwork,collision);
                knownCollisionInverted=f.collisionInverted;collisionPolarityKnown=true;
            } catch(const std::exception&) {
                if(!f.imported->bordered)throw;
                // wkRGBA targets W:A 3.8.1, whose Landscape+0x914 mask uses
                // one bits for solid land. During bordered-map startup W:A
                // can expose the grid while InitBorders is still rewriting
                // it, making content-based calibration temporarily invalid.
                f.collisionInverted=false;knownCollisionInverted=false;collisionPolarityKnown=true;
                log("Bordered map used W:A 3.8.1 collision polarity: bit 1 is solid");
            }
            f.collisionPixels=collision.pixels;f.collisionReady=true;
            log("Collision calibration completed in "+std::to_string(GetTickCount64()-calibrationBegin)+" ms");
        }
        if(f.integrated) {
            // Capture the post-terrain indexed framebuffer. These indices are
            // already remapped into the global scene palette, exactly like the
            // final image texture sampled by the pixel shader. Local landscape
            // indices are not stable when editor borders compact the palette.
            f.nativeGpu.captureTerrain(f.device,f.pixels,f.stride,f.width,f.height);
            f.nativeGpu.updateArtwork(f.device,*f.artwork,collision.pixels,collision.stride,f.collisionInverted,
                                      live.pixels,live.stride,f.palette,f.imported->bordered,
                                      int64_t(f.left),int64_t(f.top),f.width,f.height);
            f.pending=false;f.renderedThisPresent=true;presented=true;
            static bool reported=false;if(!reported){log("RGBA integrated into W:A's native palette pass");reported=true;}
            const auto compositeElapsed=ticks()-compositeBegin;addTiming(timing.composite,timing.maxComposite,compositeElapsed);
            renderTicksSincePresent+=compositeElapsed;
            return;
        }
        // Remove unchanged native 8-bit terrain while preserving the exact
        // backdrop. Pixels whose native colour no longer matches the original
        // proxy are W:A's own crater rim/scorch or newly added terrain; retain
        // those pixels so the RGBA overlay reveals the same destruction edge
        // used by classic 112-colour maps.
        const auto area=size_t(f.width)*f.height;
        if(f.expectedBase.size()!=area)f.expectedBase.resize(area);
        std::memcpy(f.expectedBase.data(),f.backdrop.data(),area);
        uint64_t blackCount{},samples{};
        for(size_t at=0;at<area;at+=64){const auto p=f.palette[f.backdrop[at]];blackCount+=(p.r<=8&&p.g<=8&&p.b<=8);++samples;}
        f.blackBackdrop=blackCount*100>=samples*95;
        const int64_t left=int64_t(f.left),top=int64_t(f.top);
        const uint32_t visibleStart=uint32_t((std::max)(int64_t(0),left));
        const uint32_t visibleEnd=uint32_t((std::max)(int64_t(0),(std::min)(int64_t(f.width),left+f.artwork->width)));
        uint32_t alignedStart=visibleStart;
        if(visibleEnd>visibleStart)alignedStart+=(8-(uint32_t(int64_t(visibleStart)-left)&7))&7;
        alignedStart=(std::min)(alignedStart,visibleEnd);
        const uint32_t fullBlocks=(visibleEnd-alignedStart)/8;
        const uint32_t firstBlock=fullBlocks?uint32_t(int64_t(alignedStart)-left)/8:0;
        f.edgeCandidates.assign(size_t(f.height)*fullBlocks,0);
        f.edgeRows.assign(f.height,0);bool anyMutation=false;
        if(fullBlocks) {
            constexpr int radius=4;
            const int64_t visibleTop=(std::max)(int64_t(0),top),visibleBottom=(std::min)(int64_t(f.height),top+f.artwork->height);
            const int64_t scanTop=(std::max)(int64_t(0),visibleTop-radius),scanBottom=(std::min)(int64_t(f.height),visibleBottom+radius);
            const uint32_t mapBlocks=f.solidStride;
            const uint32_t scanFirst=firstBlock?firstBlock-1:0,scanLast=(std::min)(mapBlocks-1,firstBlock+fullBlocks);
            for(int64_t screenY=scanTop;screenY<scanBottom;++screenY) {
                const int64_t sy=screenY-top;if(sy<0||sy>=f.artwork->height)continue;
                const auto* collisionRow=collision.pixels+size_t(sy)*collision.stride;
                const auto* originalRow=f.solidMask.data()+size_t(sy)*f.solidStride;
                for(uint32_t block=scanFirst;block<=scanLast;++block) {
                    uint8_t current=collisionRow[block];if(f.collisionInverted)current=uint8_t(~current);
                    if(current==originalRow[block])continue;
                    anyMutation=true;
                    const int firstColumn=(std::max)(0,int(block)-int(firstBlock)-1);
                    const int lastColumn=(std::min)(int(fullBlocks)-1,int(block)-int(firstBlock)+1);
                    const int64_t firstY=(std::max)(visibleTop,screenY-radius),lastY=(std::min)(visibleBottom-1,screenY+radius);
                    for(int64_t targetY=firstY;targetY<=lastY;++targetY)
                        for(int column=firstColumn;column<=lastColumn;++column) {
                            f.edgeCandidates[size_t(targetY)*fullBlocks+size_t(column)]=1;
                            f.edgeRows[size_t(targetY)]=1;
                        }
                }
            }
        }
        if(!anyMutation) {
            for(uint32_t y=0;y<f.height;++y)
                std::memcpy(f.pixels+size_t(y)*f.stride,f.expectedBase.data()+size_t(y)*f.width,f.width);
            f.pending=true;
            const auto compositeElapsed=ticks()-compositeBegin;addTiming(timing.composite,timing.maxComposite,compositeElapsed);
            return;
        }
        for(uint32_t y=0;y<f.height;++y) {
            auto* nativeRow=f.pixels+size_t(y)*f.stride;const auto* backdropRow=f.expectedBase.data()+size_t(y)*f.width;
            if(!f.edgeRows[y]){std::memcpy(nativeRow,backdropRow,f.width);continue;}
            const int64_t sy=int64_t(y)-top;
            if(sy<0||sy>=f.artwork->height){std::memcpy(nativeRow,backdropRow,f.width);continue;}
            const auto start64=(std::max)(int64_t(0),left),end64=(std::min)(int64_t(f.width),left+f.artwork->width);
            if(end64<=start64){std::memcpy(nativeRow,backdropRow,f.width);continue;}
            const auto start=uint32_t(start64),end=uint32_t(end64);std::memcpy(nativeRow,backdropRow,start);
            if(end<f.width)std::memcpy(nativeRow+end,backdropRow+end,f.width-end);
            uint32_t x=start,sx=uint32_t(start64-left);
            const auto* collisionRow=collision.pixels+size_t(sy)*collision.stride;
            const auto clearUnchanged=[&](uint32_t screenX,uint32_t sourceX) {
                if(!collision.solid(sourceX,uint32_t(sy),f.collisionInverted)||
                   matchesProxyColour(f.artwork->pixels[size_t(sy)*f.artwork->width+sourceX],nativeRow[screenX],f.palette))
                    nativeRow[screenX]=backdropRow[screenX];
            };
            for(;x<end&&(sx&7);++x,++sx)clearUnchanged(x,sx);
            uint32_t blockColumn{};
            for(;x+8<=end;x+=8,sx+=8,++blockColumn) {
                if(blockColumn>=fullBlocks || !f.edgeCandidates[size_t(y)*fullBlocks+blockColumn]) {
                    std::memcpy(nativeRow+x,backdropRow+x,8);continue;
                }
                uint8_t bits=collisionRow[sx>>3];if(f.collisionInverted)bits=uint8_t(~bits);
                if(!bits){std::memcpy(nativeRow+x,backdropRow+x,8);continue;}
                for(uint32_t bit=0;bit<8;++bit)clearUnchanged(x+bit,sx+bit);
            }
            for(;x<end;++x,++sx)clearUnchanged(x,sx);
        }
        f.pending=true;
    }catch(const std::exception& e){log(e.what());}
    const auto compositeElapsed=ticks()-compositeBegin;addTiming(timing.composite,timing.maxComposite,compositeElapsed);
}
}
