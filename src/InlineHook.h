#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
namespace wkrgba {
// Only for individually disassembled prologues with no relative
// operands. The exact host hash is validated by the caller before installation.
// Install/remove during single-threaded game initialization only.
struct InlineHook {
    uint8_t* target{};
    uint8_t* trampoline{};
    std::vector<uint8_t> original;
    bool chained{};
    static void jump(uint8_t* at,const void* to) {
        at[0]=0xE9;uint32_t distance=uint32_t(uintptr_t(to))-uint32_t(uintptr_t(at))-5;
        std::memcpy(at+1,&distance,4);
    }
    bool install(void* address,std::vector<uint8_t> expected,void* replacement) {
        auto* p=static_cast<uint8_t*>(address);
        const size_t size=expected.size();if(size<5 || size>32)return false;
        if(target)return false;
        // WormKit modules are loaded into one process and commonly detour the same
        // W:A entry points.  Preserve an earlier detour as our next handler instead
        // of rejecting the hook merely because the original prologue is gone.
        if(std::memcmp(p,expected.data(),size)) {
            void* next{};
            if(p[0]==0xE9) {
                int32_t displacement{};std::memcpy(&displacement,p+1,4);
                next=p+5+displacement;
            } else if(p[0]==0x68 && p[5]==0xC3) {
                uint32_t absolute{};std::memcpy(&absolute,p+1,4);next=reinterpret_cast<void*>(absolute);
            } else if(p[0]==0xB8 && p[5]==0xFF && p[6]==0xE0) {
                uint32_t absolute{};std::memcpy(&absolute,p+1,4);next=reinterpret_cast<void*>(absolute);
            } else if(p[0]==0xFF && p[1]==0x25) {
                uint32_t slot{};std::memcpy(&slot,p+2,4);
                if(slot)next=*reinterpret_cast<void**>(slot);
            }
            if(!next || next==replacement)return false;
            DWORD old{};if(!VirtualProtect(p,5,PAGE_EXECUTE_READWRITE,&old))return false;
            target=p;trampoline=static_cast<uint8_t*>(next);original.assign(p,p+5);chained=true;jump(p,replacement);
            DWORD ignored{};VirtualProtect(p,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),p,5);return true;
        }
        auto* stub=static_cast<uint8_t*>(VirtualAlloc(nullptr,size+5,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        if(!stub)return false;
        std::memcpy(stub,p,size);jump(stub+size,p+size);
        DWORD old{};
        if(!VirtualProtect(stub,size+5,PAGE_EXECUTE_READ,&old)){VirtualFree(stub,0,MEM_RELEASE);return false;}
        FlushInstructionCache(GetCurrentProcess(),stub,size+5);
        if(!VirtualProtect(p,5,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(stub,0,MEM_RELEASE);return false;}
        target=p;trampoline=stub;original=std::move(expected);chained=false;jump(p,replacement);
        DWORD ignored{};VirtualProtect(p,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),p,5);return true;
    }
    void remove() {
        if(!target)return;DWORD old{};
        if(!VirtualProtect(target,5,PAGE_EXECUTE_READWRITE,&old))return;
        std::memcpy(target,original.data(),5);DWORD ignored{};VirtualProtect(target,5,old,&ignored);
        FlushInstructionCache(GetCurrentProcess(),target,5);if(!chained)VirtualFree(trampoline,0,MEM_RELEASE);
        target=nullptr;trampoline=nullptr;chained=false;
    }
};
}
