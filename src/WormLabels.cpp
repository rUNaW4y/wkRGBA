#include "WormLabels.h"
#include "ExperimentalScene.h"
#include "InlineHook.h"
#include <cstdint>
#include <fstream>
#include <vector>
#include <algorithm>
#include <intrin.h>

namespace wkrgba::labels {
namespace {
struct BitmapTextbox;
struct BitmapImage {
    void* vtable;
    int32_t unknown4;
    uint8_t* data;
    int32_t bitDepth,rowSize,maxWidth,maxHeight,unknown1c,unknown20,width,height;
};
using SetText=BitmapImage* (__stdcall*)(BitmapTextbox*,char*,int,int,int,int*,int*,int);
HMODULE hostModule{};std::filesystem::path logPath;InlineHook textHook,drawHook,stippleHook;SetText original{};bool attempted{};
void* originalDraw{};
void* originalStipple{};
using DrawScaled=DWORD (__thiscall*)(void*,int,int,BitmapImage*,int,int,int,int,int);
DrawScaled originalScaled{};
struct LabelDraw {const uint8_t* pixels{};uint32_t width{},height{},stride{};int32_t xFixed{},yFixed{};};
std::vector<LabelDraw> frameLabels;
struct HudDraw {int32_t xFixed{},yFixed{};uint32_t width{},height{};};
std::vector<HudDraw> frameHud;
struct PanelDraw {int32_t xFixed{},yFixed{};uint32_t width{},height{};};
std::vector<PanelDraw> framePanels;
struct ScaledDraw {
    const uint8_t* pixels{};uint32_t stride{};
    int32_t left{},top{},sourceX{},sourceY{};uint32_t width{},height{};
    bool blackRectangle{},blackSilhouette{};
};
std::vector<ScaledDraw> frameScaled;
std::vector<const BitmapImage*> frameBlackImages;
struct StippleDraw {
    const uint8_t* pixels{};uint32_t stride{};
    int32_t left{},top{},sourceX{},sourceY{};uint32_t width{},height{},mode{},parity{};
};
std::vector<StippleDraw> frameStipples;
void log(const char* message) noexcept {try{std::ofstream out(logPath,std::ios::app);out<<message<<'\n';}catch(...) {}}

void makeInteriorOpaque(BitmapImage* image,uint8_t black) {
    if(!image||!black||image->bitDepth!=8||!image->data||image->width<3||image->height<3||
       image->width>2048||image->height>256||image->rowSize<image->width||image->rowSize>8192)return;
    const uint32_t width=uint32_t(image->width),height=uint32_t(image->height);const size_t area=size_t(width)*height;
    struct CachedMask {uint64_t hash{};uint32_t width{},height{};std::vector<uint32_t> interior;};
    static thread_local std::vector<CachedMask> cache;
    uint64_t hash=1469598103934665603ull;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        hash^=image->data[size_t(y)*image->rowSize+x];hash*=1099511628211ull;
    }
    for(auto& cached:cache)if(cached.hash==hash&&cached.width==width&&cached.height==height) {
        for(const auto at:cached.interior)image->data[size_t(at/width)*image->rowSize+at%width]=black;
        return;
    }
    std::vector<uint8_t> exterior(area,0);std::vector<uint32_t> queue;queue.reserve(width*2+height*2);
    const auto add=[&](uint32_t x,uint32_t y) {
        const auto at=size_t(y)*width+x;if(!exterior[at]&&image->data[size_t(y)*image->rowSize+x]==0){exterior[at]=1;queue.push_back(uint32_t(at));}
    };
    for(uint32_t x=0;x<width;++x){add(x,0);add(x,height-1);}for(uint32_t y=1;y+1<height;++y){add(0,y);add(width-1,y);}
    for(size_t head=0;head<queue.size();++head) {
        const uint32_t at=queue[head],x=at%width,y=at/width;
        if(x)add(x-1,y);if(x+1<width)add(x+1,y);if(y)add(x,y-1);if(y+1<height)add(x,y+1);
    }
    CachedMask cached{hash,width,height,{}};
    for(uint32_t y=1;y+1<height;++y)for(uint32_t x=1;x+1<width;++x) {
        const auto at=size_t(y)*width+x;if(!exterior[at]&&image->data[size_t(y)*image->rowSize+x]==0)
            {image->data[size_t(y)*image->rowSize+x]=black;cached.interior.push_back(uint32_t(at));}
    }
    if(cache.size()>=128)cache.erase(cache.begin());
    cache.push_back(std::move(cached));
}
BitmapImage* __stdcall hooked(BitmapTextbox* box,char* text,int textColor,int color1,int color2,int* width,int* height,int opacity) {
    auto* result=original(box,text,textColor,color1,color2,width,height,opacity);
    if(experimental::blackBackdropActive()) {
        makeInteriorOpaque(result,experimental::opaqueBlackPaletteIndex());
        if(result&&frameBlackImages.size()<256)frameBlackImages.push_back(result);
    }
    return result;
}

void __cdecl observeQueuedLabel(const uint32_t* saved) noexcept {
    try {
        if(!saved||frameLabels.size()>=192)return;
        // pushfd + pushad layout used by hookedDraw. The untouched entry
        // stack starts at saved[9], while saved[6] is the entry ECX value.
        const auto* stack=saved+9;
        const auto base=reinterpret_cast<uintptr_t>(hostModule);
        const auto returnAddress=uintptr_t(stack[0]);
        const bool chat=returnAddress==base+0x1049DC;
        const bool worm=returnAddress==base+0x1100F4||returnAddress==base+0x110173||returnAddress==base+0x110247;
        if(!chat&&!worm)return;
        auto* image=reinterpret_cast<const BitmapImage*>(stack[5]);
        const int32_t sourceWidth=int32_t(stack[6]),sourceHeight=int32_t(saved[6]);
        if(!image||image->bitDepth!=8||!image->data||image->width<1||image->height<1||
           image->width>2048||image->height>256||image->rowSize<image->width||image->rowSize>8192||
           sourceWidth<1||sourceHeight<1||sourceWidth>image->width||sourceHeight>image->height)return;
        frameLabels.push_back({image->data,uint32_t(sourceWidth),uint32_t(sourceHeight),uint32_t(image->rowSize),
                               int32_t(stack[3]),int32_t(stack[4])});
    } catch(...) {}
}

DWORD __fastcall hookedScaled(void* display,void*,int x,int y,BitmapImage* image,
                               int sx,int sy,int right,int bottom,int flags) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const auto base=reinterpret_cast<uintptr_t>(hostModule);
    bool blackRectangle=false;
    bool blackSilhouette=false;
    if(experimental::blackBackdropActive()&&image&&sx>=0&&sy>=0&&right>sx&&bottom>sy&&
       right<=image->maxWidth&&bottom<=image->maxHeight) {
        const uint32_t width=uint32_t(right-sx),height=uint32_t(bottom-sy);
        bool weaponPanelDraw=false,escPanelDraw=false;
        try {
            auto* session=*reinterpret_cast<void**>(base+0x3A0884);
            auto* runtime=session?*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(session)+0xA0):nullptr;
            auto* world=runtime?*reinterpret_cast<uint8_t**>(runtime+0x488):nullptr;
            auto* panel=world?*reinterpret_cast<uint8_t**>(world+0x548):nullptr;
            auto* panelImage=world?*reinterpret_cast<BitmapImage**>(world+0x138):nullptr;
            const auto panelWidth=panel?*reinterpret_cast<int32_t*>(panel+0x4C):0;
            const auto panelHeight=panel?*reinterpret_cast<int32_t*>(panel+0x50):0;
            weaponPanelDraw=panel&&panelImage==image&&*reinterpret_cast<int32_t*>(panel+0x1D4)!=0&&
                panelWidth>0&&panelHeight>0&&width==uint32_t(panelWidth)&&height==uint32_t(panelHeight);
            auto* menuImage=runtime?*reinterpret_cast<BitmapImage**>(runtime+0x2C):nullptr;
            auto* confirmImage=runtime?*reinterpret_cast<BitmapImage**>(runtime+0x34):nullptr;
            const auto escAnim=runtime?*reinterpret_cast<int32_t*>(runtime+0x424):0;
            const auto confirmAnim=runtime?*reinterpret_cast<int32_t*>(runtime+0x42C):0;
            const auto menuWidth=runtime?*reinterpret_cast<int32_t*>(runtime+0x440):0;
            const auto menuHeight=runtime?*reinterpret_cast<int32_t*>(runtime+0x444):0;
            const auto confirmWidth=runtime?*reinterpret_cast<int32_t*>(runtime+0x448):0;
            const auto confirmHeight=runtime?*reinterpret_cast<int32_t*>(runtime+0x44C):0;
            escPanelDraw=(escAnim&&image==menuImage&&menuWidth>0&&menuHeight>0&&width==uint32_t(menuWidth)&&height==uint32_t(menuHeight))||
                (confirmAnim&&image==confirmImage&&confirmWidth>0&&confirmHeight>0&&width==uint32_t(confirmWidth)&&height==uint32_t(confirmHeight));
        }catch(...) {weaponPanelDraw=false;escPanelDraw=false;}
        if(weaponPanelDraw||escPanelDraw)makeInteriorOpaque(image,experimental::opaqueBlackPaletteIndex());
        blackRectangle=weaponPanelDraw||escPanelDraw;
        blackSilhouette=std::find(frameBlackImages.begin(),frameBlackImages.end(),image)!=frameBlackImages.end();
        if(weaponPanelDraw&&framePanels.size()<4)framePanels.push_back({x,y,width,height});
        else if(flags==0&&width<=512&&height<=64&&frameHud.size()<64)frameHud.push_back({x,y,width,height});
    }
    if(image&&image->bitDepth==8&&image->data&&image->rowSize>=image->maxWidth&&
       image->maxWidth>0&&image->maxHeight>0&&image->maxWidth<=2048&&image->maxHeight<=2048&&
       sx>=0&&sy>=0&&right>sx&&bottom>sy&&right<=image->maxWidth&&bottom<=image->maxHeight) {
        const uint32_t width=uint32_t(right-sx),height=uint32_t(bottom-sy);
        const auto* bytes=static_cast<const uint8_t*>(display);
        const int32_t cameraX=*reinterpret_cast<const int32_t*>(bytes+0x3560);
        const int32_t cameraY=*reinterpret_cast<const int32_t*>(bytes+0x3564);
        auto* layer=*reinterpret_cast<uint8_t* const*>(bytes+0x3D9C);
        const uint32_t viewportWidth=layer?*reinterpret_cast<const uint32_t*>(layer+0x14):0;
        const int64_t centerX=int64_t(cameraX)+int64_t(x>>16);
        const bool hudBackground=experimental::blackBackdropActive()&&caller==base+0x14286E&&flags==0&&viewportWidth&&
            ((centerX<int64_t(viewportWidth)*15/100&&width==56&&height==42)||
             (centerX<int64_t(viewportWidth)*15/100&&height==17&&width>=40&&width<=64)||
             (centerX>=int64_t(viewportWidth)*35/100&&centerX<=int64_t(viewportWidth)*70/100&&height>=12&&height<=32&&width>=20)||
             (centerX>int64_t(viewportWidth)*70/100&&height==17&&width>=100));
        if((blackRectangle||blackSilhouette||hudBackground)&&frameScaled.size()<512)
            frameScaled.push_back({image->data,uint32_t(image->rowSize),
                                   cameraX+(x>>16)-int32_t(width/2),cameraY+(y>>16)-int32_t(height/2),
                                   sx,sy,width,height,blackRectangle,blackSilhouette||hudBackground});
    }
    return originalScaled(display,x,y,image,sx,sy,right,bottom,flags);
}

// Bitmap draw wrapper at W:A 0x547F00. It only observes the three calls made by
// DrawWormTextboxes, then tail-jumps to the original custom-convention routine
// with every register, flag and stack argument unchanged.
__declspec(naked) void hookedDraw() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        push eax
        call observeQueuedLabel
        add esp,4
        popad
        popfd
        jmp dword ptr [originalDraw]
    }
}

void __cdecl observeStipple(const uint32_t* saved) noexcept {
    try {
        if(!saved||frameStipples.size()>=128)return;
        const auto* stack=saved+9;
        auto* source=reinterpret_cast<const BitmapImage*>(stack[5]);
        const int32_t left=int32_t(stack[2]),top=int32_t(stack[3]);
        const int32_t width=int32_t(stack[4]),height=int32_t(saved[7]); // entry EAX
        const int32_t sourceX=int32_t(stack[6]),sourceY=int32_t(stack[7]);
        const uint32_t mode=stack[8];
        if(!source||source->bitDepth!=8||!source->data||
           source->maxWidth<=0||source->maxHeight<=0||source->maxWidth>32768||source->maxHeight>32768||
           source->rowSize<source->maxWidth||source->rowSize>32768||width<=0||height<=0||
           sourceX<0||sourceY<0||sourceX+width>source->maxWidth||sourceY+height>source->maxHeight)return;
        const auto base=reinterpret_cast<uintptr_t>(hostModule);
        const uint32_t parity=*reinterpret_cast<const uint32_t*>(base+0x3A087C);
        frameStipples.push_back({source->data,uint32_t(source->rowSize),left,top,sourceX,sourceY,
                                 uint32_t(width),uint32_t(height),mode,parity});
    } catch(...) {}
}

// Teleport-placement worms use DisplayGfx::BlitStippled rather than the normal
// sprite blitter. Observe that exact path and preserve only the checkerboard
// pixels W:A actually writes.
__declspec(naked) void hookedStipple() {
    __asm {
        pushfd
        pushad
        mov eax,esp
        push eax
        call observeStipple
        add esp,4
        popad
        popfd
        jmp dword ptr [originalStipple]
    }
}
}
void configure(HMODULE host,const std::filesystem::path& directory) noexcept {
    hostModule=host;logPath=directory/L"wkRGBA_labels.log";
}
void ensureInstalled() noexcept {
    if(attempted)return;attempted=true;
    try {
        if(!hostModule){log("Textbox hook skipped: host unavailable");return;}
        auto* address=reinterpret_cast<uint8_t*>(hostModule)+0xFB070;
        const std::vector<uint8_t> expected{0x6A,0xFF,0x68,0x48,0xB7,0x60,0x00};
        if(!textHook.install(address,expected,reinterpret_cast<void*>(&hooked))){log("Textbox hook installation failed");return;}
        original=reinterpret_cast<SetText>(textHook.trampoline);
        auto* drawAddress=reinterpret_cast<uint8_t*>(hostModule)+0x147F00;
        const std::vector<uint8_t> drawExpected{0x8B,0x54,0x24,0x18,0x50};
        if(!drawHook.install(drawAddress,drawExpected,reinterpret_cast<void*>(&hookedDraw))) {
            log("Worm textbox queue hook installation failed");return;
        }
        originalDraw=drawHook.trampoline;
        auto* stippleAddress=reinterpret_cast<uint8_t*>(hostModule)+0x16AEF0;
        const std::vector<uint8_t> stippleExpected{0x51,0x83,0x7C,0x24,0x14,0x00};
        if(!stippleHook.install(stippleAddress,stippleExpected,reinterpret_cast<void*>(&hookedStipple))) {
            log("Teleport stipple hook installation failed");return;
        }
        originalStipple=stippleHook.trampoline;
        auto base=reinterpret_cast<uintptr_t>(hostModule);
        auto** scaledSlot=reinterpret_cast<void**>(base+0x26A218+20*sizeof(void*));
        auto* expectedScaled=reinterpret_cast<void*>(base+0x16B660);
        if(*scaledSlot!=expectedScaled&&*scaledSlot!=reinterpret_cast<void*>(&hookedScaled)) {
            log("ESC canvas hook installation failed");return;
        }
        originalScaled=reinterpret_cast<DrawScaled>(expectedScaled);
        DWORD old{},ignored{};
        if(!VirtualProtect(scaledSlot,sizeof(void*),PAGE_READWRITE,&old)){log("ESC canvas hook protection failed");return;}
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(scaledSlot),reinterpret_cast<void*>(&hookedScaled));
        VirtualProtect(scaledSlot,sizeof(void*),old,&ignored);
        log(textHook.chained?"Textbox and exact queue hooks installed after an existing text detour":"Textbox and exact queue hooks installed");
    } catch(...) {log("Textbox hook installation raised an exception");}
}
void protect(std::vector<uint8_t>& visibility,std::vector<uint8_t>* blackout,
             uint32_t viewportWidth,uint32_t viewportHeight,int64_t mapLeft,int64_t mapTop,
             int32_t cameraX,int32_t cameraY,bool bordered) noexcept {
    // TextboxLocal commands are enqueued before W:A dispatches the terrain
    // command that starts wkRGBA's capture. Consume them here, after the
    // native render queue has completed, rather than clearing them when the
    // terrain pass begins.
    const auto area=size_t(viewportWidth)*viewportHeight;
    if(visibility.size()!=area||(blackout&&blackout->size()!=area)){frameLabels.clear();frameHud.clear();framePanels.clear();frameScaled.clear();frameBlackImages.clear();frameStipples.clear();return;}
    const auto carve=[&](int64_t left,int64_t top,uint32_t width,uint32_t height,uint32_t cornerRadius) {
        for(uint32_t row=0;row<height;++row) {
            const int64_t y=top+row;if(y<0||y>=viewportHeight)continue;
            const uint32_t edgeDistance=(std::min)(row,height-1-row);
            const uint32_t inset=edgeDistance<cornerRadius?cornerRadius-edgeDistance:0;
            auto* mask=visibility.data()+size_t(y)*viewportWidth;
            for(uint32_t col=inset;col+inset<width;++col) {
                const int64_t x=left+col;if(x>=0&&x<viewportWidth)mask[x]=0;
            }
        }
    };
    const int64_t displayCenterX=int64_t(viewportWidth/2);
    const int64_t displayCenterY=int64_t(viewportHeight/2);
    // Game::render reserves an animated strip at the top of the physical
    // framebuffer while the in-game chat is open.  The native terrain
    // viewport is shortened by this exact value, but the RGBA compositor uses
    // the physical framebuffer dimensions so camera motion remains smooth.
    // Preserve the reserved strip as native foreground; otherwise the RGBA
    // terrain is painted back through the chat's palette-black background.
    try {
        const auto base=reinterpret_cast<uintptr_t>(hostModule);
        auto* session=*reinterpret_cast<void**>(base+0x3A0884);
        auto* runtime=session?*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(session)+0xA0):nullptr;
        if(runtime&&*reinterpret_cast<int32_t*>(runtime+0x1C)!=0) {
            const int32_t lineHeight=*reinterpret_cast<int32_t*>(runtime+0x2B0);
            const int32_t shownLines=*reinterpret_cast<int32_t*>(runtime+0x2A0);
            const int32_t retainedLines=*reinterpret_cast<int32_t*>(runtime+0x2A4);
            const int32_t animation=*reinterpret_cast<int32_t*>(runtime+0x3FC);
            if(lineHeight>0&&lineHeight<=128&&shownLines>=0&&shownLines<=128&&
               retainedLines>=0&&retainedLines<=shownLines&&animation>=0&&animation<=65536) {
                int64_t retained{};
                if(*reinterpret_cast<uint8_t*>(runtime+0x414)!=0)
                    retained=(int64_t(retainedLines)+1)*lineHeight+6;
                int64_t expanded=(int64_t(shownLines)+1)*lineHeight-retained+6;
                int64_t chatHeight=((expanded*animation)>>16)+retained;
                chatHeight=(chatHeight+1)&~int64_t(1);
                if(chatHeight>0&&chatHeight<=viewportHeight)
                    carve(0,0,viewportWidth,uint32_t(chatHeight),0);
            }
        }
    }catch(...) {}
    if(!bordered) for(const auto& hud:frameHud) {
        const int64_t centerX=int64_t(hud.xFixed>>16)+displayCenterX;
        const int64_t centerY=int64_t(hud.yFixed>>16)+displayCenterY;
        const int64_t left=centerX-int64_t(hud.width/2);
        const int64_t top=centerY-int64_t(hud.height/2);
        if(centerX<int64_t(viewportWidth)*15/100&&hud.width==56&&hud.height==42)
            carve(left,top,hud.width,hud.height,2);
        else if(centerX<int64_t(viewportWidth)*15/100&&hud.height==17&&hud.width>=40&&hud.width<=64)
            carve(left,top,hud.width,hud.height,2);
        else if(centerX>=int64_t(viewportWidth)*35/100&&centerX<=int64_t(viewportWidth)*70/100&&
                hud.height>=12&&hud.height<=32&&hud.width>=20)
            carve(left,top,hud.width,hud.height,2);
        else if(centerX>int64_t(viewportWidth)*70/100&&hud.height==17&&hud.width>=100)
            carve(left,top,hud.width,hud.height,2);
    }
    frameHud.clear();
    if(!bordered) for(const auto& panel:framePanels) {
        const int64_t centerX=int64_t(panel.xFixed>>16)+displayCenterX;
        const int64_t centerY=int64_t(panel.yFixed>>16)+displayCenterY;
        carve(centerX-int64_t(panel.width/2),centerY-int64_t(panel.height/2),
              panel.width,panel.height,1);
    }
    framePanels.clear();
    if(bordered) for(const auto& sprite:frameScaled) {
        for(uint32_t row=0;row<sprite.height;++row) {
            const int64_t destinationY=int64_t(sprite.top)+row;
            if(destinationY<0||destinationY>=viewportHeight)continue;
            const auto* source=sprite.pixels+size_t(sprite.sourceY+int32_t(row))*sprite.stride+sprite.sourceX;
            auto* mask=visibility.data()+size_t(destinationY)*viewportWidth;
            for(uint32_t col=0;col<sprite.width;++col) {
                const int64_t destinationX=int64_t(sprite.left)+col;
                if(destinationX<0||destinationX>=viewportWidth||!mask[destinationX])continue;
                const bool roundedCorner=(row==0||row+1==sprite.height)&&(col==0||col+1==sprite.width);
                if((sprite.blackRectangle&&!roundedCorner)||(sprite.blackSilhouette&&source[col]!=0))
                    mask[destinationX]=blackout?128:0;
            }
        }
    }
    frameScaled.clear();
    frameBlackImages.clear();
    if(!bordered) for(const auto& sprite:frameStipples) {
        for(uint32_t row=0;row<sprite.height;++row) {
            const int64_t destinationY=int64_t(sprite.top)+row;if(destinationY<0||destinationY>=viewportHeight)continue;
            const auto* source=sprite.pixels+size_t(sprite.sourceY+int32_t(row))*sprite.stride+sprite.sourceX;
            auto* mask=visibility.data()+size_t(destinationY)*viewportWidth;
            for(uint32_t col=0;col<sprite.width;++col) {
                const int64_t destinationX=int64_t(sprite.left)+col;
                const bool written=((uint32_t(destinationX)^sprite.parity^uint32_t(destinationY)^sprite.mode)&1u)!=0;
                if(written&&source[col]!=0&&destinationX>=0&&destinationX<viewportWidth)mask[destinationX]=0;
            }
        }
    }
    frameStipples.clear();
    // RenderEscMenuOverlay uses runtime animation fields directly. Recreate
    // the exact panel rectangles here so the RGBA terrain is never drawn
    // below their intentionally transparent black interiors.
    if(!bordered) try {
        const auto base=reinterpret_cast<uintptr_t>(hostModule);
        auto* session=*reinterpret_cast<void**>(base+0x3A0884);
        auto* runtime=session?*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(session)+0xA0):nullptr;
        if(runtime) {
            const int32_t escAnim=*reinterpret_cast<int32_t*>(runtime+0x424);
            const int32_t confirmAnim=*reinterpret_cast<int32_t*>(runtime+0x42C);
            const int32_t menuWidth=*reinterpret_cast<int32_t*>(runtime+0x440);
            const int32_t menuHeight=*reinterpret_cast<int32_t*>(runtime+0x444);
            const int32_t confirmWidth=*reinterpret_cast<int32_t*>(runtime+0x448);
            const int32_t confirmHeight=*reinterpret_cast<int32_t*>(runtime+0x44C);
            if(escAnim&&menuWidth>0&&menuHeight>0&&menuWidth<=int32_t(viewportWidth)*2&&menuHeight<=int32_t(viewportHeight)*2) {
                const int64_t xFixed=(int64_t(menuWidth/2)-int64_t(viewportWidth/2)+8)*65536-
                    int64_t(menuWidth+8)*(65536-int64_t(escAnim));
                const int64_t centerX=(xFixed>>16)+int64_t(viewportWidth/2);
                const int64_t centerY=-64+int64_t(viewportHeight/2);
                carve(centerX-menuWidth/2,centerY-menuHeight/2,uint32_t(menuWidth),uint32_t(menuHeight),1);
                if(confirmAnim&&confirmWidth>0&&confirmHeight>0&&confirmWidth<=int32_t(viewportWidth)*2&&confirmHeight<=int32_t(viewportHeight)*2) {
                    const int64_t yFixed=int64_t(menuHeight-confirmHeight)*65536/2+
                        int64_t(confirmHeight+8)*confirmAnim-int64_t(64)*65536;
                    const int64_t confirmCenterY=(yFixed>>16)+int64_t(viewportHeight/2);
                    carve(centerX-confirmWidth/2,confirmCenterY-confirmHeight/2,
                          uint32_t(confirmWidth),uint32_t(confirmHeight),1);
                }
            }
        }
    }catch(...) {}
    if(!bordered) for(const auto& label:frameLabels) {
        // DisplayGfx::DrawScaledSprite (slot 20) treats x/y as the centre
        // of the source rectangle, after applying the camera offset.
        const int64_t originX=int64_t(label.xFixed>>16)+mapLeft-int64_t(label.width/2);
        const int64_t originY=int64_t(label.yFixed>>16)+mapTop-int64_t(label.height/2);
        for(uint32_t row=0;row<label.height;++row) {
        const int64_t destinationY=originY+row;
        if(destinationY<0||destinationY>=viewportHeight)continue;
        const auto* source=label.pixels+size_t(row)*label.stride;
        auto* mask=visibility.data()+size_t(destinationY)*viewportWidth;
        for(uint32_t col=0;col<label.width;++col) {
            const int64_t destinationX=originX+col;
            // Only the actual textbox silhouette is protected. Exterior zero
            // pixels at rounded corners stay on the RGBA terrain.
            if(destinationX>=0&&destinationX<viewportWidth&&source[col]!=0)
                mask[destinationX]=0;
        }
        }
    }
    frameLabels.clear();
}
}
