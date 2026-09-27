#include "RenderProbe.h"
#include "ExperimentalScene.h"
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>
#include <set>
#include <tuple>

namespace wkrgba::probe {
namespace {
struct Surface { uint32_t vtable, width, height; const uint8_t* data; uint32_t extra; };
static_assert(sizeof(Surface)==0x14);
using Draw = DWORD (__fastcall*)(void*, DWORD*, Surface*, int, int, int, int, int, int, int);
Draw original{};
using DrawTiles = void (__thiscall*)(void*,int,int,int,uint32_t);
DrawTiles originalTiles{};
struct State {
    std::mutex mutex;
    std::filesystem::path log;
    std::set<std::tuple<uintptr_t,int,int,int,int,int,int,int>> seen;
    unsigned tileCalls{};
    bool trace{};
};
State& state(){static auto* s=new State;return *s;}
void __fastcall drawTiles(void* display,void*,int x,int y,int count,uint32_t flags) {
    try {
        auto& s=state();
        if(s.trace) {std::lock_guard lock(s.mutex);
        if(s.tileCalls++<32) {
            auto* p=static_cast<const uint8_t*>(display);
            auto field=[&](size_t offset){return *reinterpret_cast<const int32_t*>(p+offset);};
            std::ofstream out(s.log,std::ios::app);
            out<<"DrawTiledTerrain display="<<display<<" worldFixed="<<x<<','<<y
               <<" camera="<<field(0x3560)<<','<<field(0x3564)
               <<" map="<<field(0x4DDC)<<'x'<<field(0x4DE0)
               <<" tile="<<field(0x4DE4)<<'x'<<field(0x4DE8)
               <<" rows="<<count<<" flags="<<flags<<'\n';
        }}
    }catch(...){}
    if(experimental::beginTerrainPass(display,x,y,count,flags)) {
        originalTiles(display,x,y,count,flags);
        experimental::completeTerrainPass();
    } else originalTiles(display,x,y,count,flags);
}
DWORD __fastcall draw(void* context,DWORD* result,Surface* source,
    int x,int y,int sx,int sy,int width,int height,int flags) {
    // Logging failure must never prevent the native draw or change its result.
    try {
        auto& s=state();std::lock_guard lock(s.mutex);
        // Menu stars also use this slot. Keep their animation from exhausting
        // the bounded trace before any terrain surface is drawn.
        if(source && source->width>=64 && source->height>=32 && s.seen.size()<64 && s.seen.emplace(reinterpret_cast<uintptr_t>(source),x,y,sx,sy,width,height,flags).second) {
            std::ofstream out(s.log,std::ios::app);
            out<<"DrawLandscape context="<<context<<" source="<<source<<" vtable="<<std::hex<<source->vtable<<std::dec
               <<" surface="<<source->width<<'x'<<source->height<<" destination="<<x<<','<<y
               <<" sourceOrigin="<<sx<<','<<sy<<" size="<<width<<'x'<<height<<" flags="<<flags<<'\n';
        }
    }catch(...){}
    return original(context,result,source,x,y,sx,sy,width,height,flags);
}
}
bool install(HMODULE host,const std::filesystem::path& directory) noexcept {
    try {
        const bool tracing=std::filesystem::exists(directory/L"wkRGBA_trace");
        auto base=reinterpret_cast<uintptr_t>(host);
        auto** slot=reinterpret_cast<void**>(base+0x262EC8+23*sizeof(void*));
        auto* expected=reinterpret_cast<void*>(base+0x1A2790);
        auto** tileSlot=reinterpret_cast<void**>(base+0x26A270);
        auto* expectedTiles=reinterpret_cast<void*>(base+0x16C5A0);
        if(*tileSlot!=expectedTiles || (tracing&&*slot!=expected))return false;
        auto& s=state();s.log=directory/L"wkRGBA_render_probe.log";s.trace=tracing;
        DWORD old{},ignored{};
        if(tracing) {
            original=reinterpret_cast<Draw>(expected);
            if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old))return false;
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(slot),reinterpret_cast<void*>(&draw));
            VirtualProtect(slot,sizeof(void*),old,&ignored);
        }
        originalTiles=reinterpret_cast<DrawTiles>(expectedTiles);
        if(VirtualProtect(tileSlot,sizeof(void*),PAGE_READWRITE,&old)) {
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(tileSlot),reinterpret_cast<void*>(&drawTiles));
            VirtualProtect(tileSlot,sizeof(void*),old,&ignored);
        }
        if(tracing){std::ofstream out(s.log,std::ios::app);out<<"DrawLandscape vtable observation installed; native rendering unchanged\n";}
        return true;
    }catch(...){return false;}
}
}
