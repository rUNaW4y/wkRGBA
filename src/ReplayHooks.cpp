#include "ReplayHooks.h"
#include "ReplayData.h"
#include "InlineHook.h"
#include "Loader.h"
#include "Png.h"
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
namespace wkrgba::replay {
namespace {
InlineHook createHook,loadHook;
std::filesystem::path logPath;
std::filesystem::path runtimeDirectory;
std::mutex logMutex;
void log(const char* text) noexcept {try{std::lock_guard lock(logMutex);std::ofstream f(logPath,std::ios::app);f<<"t="<<GetTickCount64()<<" "<<text<<'\n';}catch(...){}}
uint32_t number(std::span<const uint8_t> b,size_t offset){
    if(offset>b.size() || b.size()-offset<4)throw std::invalid_argument("Invalid replay map header");
    uint32_t value{};std::memcpy(&value,b.data()+offset,4);return value;
}
std::filesystem::path filename(const char* text) {
    const auto length=strnlen_s(text,1024);if(!length || length==1024)throw std::invalid_argument("Invalid native replay filename");
    auto size=MultiByteToWideChar(CP_ACP,0,text,int(length),nullptr,0);
    if(!size)throw std::runtime_error("Cannot convert replay path");
    std::wstring wide(size,L'\0');MultiByteToWideChar(CP_ACP,0,text,int(length),wide.data(),size);return wide;
}
struct Handle {HANDLE value{INVALID_HANDLE_VALUE};~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
net::Bytes read(HANDLE h) {
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(h,&size) || size.QuadPart<16 || size.QuadPart>1024ll*1024*1024)throw std::runtime_error("Replay exceeds input limits");
    net::Bytes bytes(size_t(size.QuadPart));DWORD got{};
    if(!ReadFile(h,bytes.data(),DWORD(bytes.size()),&got,nullptr)||got!=bytes.size())throw std::runtime_error("Cannot read complete replay");
    return bytes;
}
enum class Attach { Done, Retry, Skipped };
Attach attach(const std::filesystem::path& path,const loader::ImportedMap& imported) {
    // Exclusive access means the native recorder has closed its event stream.
    // Rewriting an actively recorded replay would corrupt it.
    Handle file{CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    if(file.value==INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();
        if(error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION || error==ERROR_FILE_NOT_FOUND)return Attach::Retry;
        throw std::runtime_error("Cannot open replay for RGBA metadata");
    }
    auto bytes=read(file.value);
    if(extract(bytes))return Attach::Done;
    const auto mapSize=number(bytes,4);
    if(mapSize<4 || mapSize>bytes.size()-8 || number(bytes,8)!=3)return Attach::Skipped;
    if(!imported.nativePng.empty()) {
        const auto nativeBytes=std::span(bytes).subspan(12,mapSize-4);
        if(!pngDimensionsMatch(nativeBytes,imported.width,imported.height)){
            log("Replay native PNG dimensions do not match RGBA map; extension skipped");return Attach::Skipped;}
    } else {
        auto artwork=imported.artwork();auto native=readPngMemory(std::span(bytes).subspan(12,mapSize-4),false);
        if(!matchesNativeProxy(*artwork,native)){log("Replay artwork does not match native map; extension skipped");return Attach::Skipped;}
    }
    auto packed=embed(bytes,imported.manifest,imported.rgbaPng);
    wchar_t backupName[MAX_PATH+1]{};
    if(!GetTempFileNameW(runtimeDirectory.c_str(),L"rep",0,backupName))
        throw std::runtime_error("Cannot allocate replay recovery backup");
    const std::filesystem::path backup=backupName;
    Handle saved{CreateFileW(backup.c_str(),GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    DWORD count{};
    if(saved.value==INVALID_HANDLE_VALUE || !WriteFile(saved.value,bytes.data(),DWORD(bytes.size()),&count,nullptr) || count!=bytes.size())
        throw std::runtime_error("Cannot preserve original replay header");
    LARGE_INTEGER zero{};
    if(!SetFilePointerEx(file.value,zero,nullptr,FILE_BEGIN) || !WriteFile(file.value,packed.data(),DWORD(packed.size()),&count,nullptr) || count!=packed.size() || !SetEndOfFile(file.value)) {
        SetFilePointerEx(file.value,zero,nullptr,FILE_BEGIN);WriteFile(file.value,bytes.data(),DWORD(bytes.size()),&count,nullptr);SetEndOfFile(file.value);
        throw std::runtime_error("Replay metadata write failed; original header backup retained");
    }
    FlushFileBuffers(file.value);CloseHandle(saved.value);saved.value=INVALID_HANDLE_VALUE;DeleteFileW(backup.c_str());
    log("RGBA replay settings extension written");return Attach::Done;
}
struct Deferred {std::filesystem::path path;std::shared_ptr<const loader::ImportedMap> imported;};
DWORD WINAPI deferredAttach(void* value) noexcept {
    std::unique_ptr<Deferred> work(static_cast<Deferred*>(value));
    // A long match may keep the replay open for hours. The DLL is pinned at
    // startup, so this worker cannot outlive its code while W:A is running.
    for(unsigned attempt=0;attempt<12u*60u*60u;++attempt) {
        try {if(attach(work->path,*work->imported)!=Attach::Retry)return 0;}
        catch(const std::exception& error){log(error.what());return 0;}
        Sleep(1000);
    }
    log("Replay RGBA metadata timed out while waiting for the recorder");return 0;
}
void defer(std::filesystem::path path,std::shared_ptr<const loader::ImportedMap> imported) {
    auto work=std::make_unique<Deferred>(Deferred{std::move(path),std::move(imported)});
    Handle thread{CreateThread(nullptr,0,&deferredAttach,work.get(),0,nullptr)};
    if(thread.value==INVALID_HANDLE_VALUE || !thread.value)throw std::runtime_error("Cannot start deferred replay metadata writer");
    work.release();
}
using Create=int (__stdcall*)(int,const char*,__time64_t);
int __stdcall onCreate(int owner,const char* name,__time64_t time) {
    const auto nativeBegin=GetTickCount64();log("CreateReplay native call begin");
    auto result=reinterpret_cast<Create>(createHook.trampoline)(owner,name,time);
    try {auto elapsed=std::to_string(GetTickCount64()-nativeBegin);log(("CreateReplay native call completed in "+elapsed+" ms").c_str());}catch(...){}
    try {
        auto imported=loader::lastImport();if(!imported)return result;
        const auto attachBegin=GetTickCount64();log("Replay RGBA attachment begin");
        auto path=filename(reinterpret_cast<const char*>(uintptr_t(owner)+0xDF60));
        if(attach(path,*imported)==Attach::Retry) {
            defer(std::move(path),std::move(imported));
            log("Replay metadata writer is waiting for the native recorder to close");
        }
        auto elapsed=std::to_string(GetTickCount64()-attachBegin);log(("Replay RGBA attachment returned in "+elapsed+" ms").c_str());
    }catch(const std::exception& e){log(e.what());}
    return result;
}
using Load=int (__stdcall*)(int,int);
int __stdcall onLoad(int owner,int argument) {
    loader::clearImport();
    try {
        auto path=filename(reinterpret_cast<const char*>(uintptr_t(owner)+0xDB60));
        Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot inspect replay RGBA data");
        auto bytes=read(file.value);auto artwork=extract(bytes);
        if(artwork){loader::acceptImage(std::move(artwork->image),path,std::move(artwork->png));log("Replay RGBA artwork restored and verified");}
    }catch(const std::exception& e){log(e.what());}
    return reinterpret_cast<Load>(loadHook.trampoline)(owner,argument);
}
}
bool installHooks(HMODULE host,const std::filesystem::path& dir) noexcept {
    try {
        runtimeDirectory=dir;logPath=dir/L"wkRGBA_replay.log";auto* base=reinterpret_cast<uint8_t*>(host);
        if(!createHook.install(base+0x616B0,{0x55,0x8B,0xEC,0x6A,0xFF},reinterpret_cast<void*>(&onCreate)))return false;
        if(!loadHook.install(base+0x62DF0,{0x55,0x8D,0x6C,0x24,0x90},reinterpret_cast<void*>(&onLoad))){createHook.remove();return false;}
        log("CreateReplay and LoadReplay hooks installed");return true;
    }catch(...){return false;}
}
}
