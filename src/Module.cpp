#define WKRGBA_BUILD
#include "Api.h"
#include "Png.h"
#include "D3D9Terrain.h"
#include "Loader.h"
#include "ExperimentalScene.h"
#include <windows.h>
#include <memory>
#include <mutex>
#include <string>

namespace {
struct State {
    std::mutex mutex;
    std::unique_ptr<wkrgba::Terrain> terrain;
    wkrgba::D3D9Terrain renderer;
    IDirect3DDevice9* uploadedDevice{};
    bool dirty = true;
};
State& state() {
    // Avoid Direct3D Release calls under the Windows loader lock on unload.
    // Explicit Clear performs resource release on the caller's device thread.
    static auto* value = new State;
    return *value;
}
thread_local std::string error;
template<class F> int guarded(F action) {
    try { auto& s=state(); std::lock_guard lock(s.mutex); action(s); error.clear(); return 1; }
    catch (const std::exception& e) { error = e.what(); return 0; }
    catch (...) { error = "Unknown wkRGBA failure"; return 0; }
}
}
const char* __cdecl wkRGBAStatus() {
    if(wkrgba::experimental::hasPresented())return "wkRGBA: RGBA scene presented; multiplayer hooks active";
    return wkrgba::loader::installed() ? "wkRGBA: RGBA loader installed; renderer waiting for a matching map"
        : "wkRGBA: inactive (unsupported host)";
}
const char* __cdecl wkRGBALastError() { return error.c_str(); }
int __cdecl wkRGBALoad(const wchar_t* path, unsigned threshold) {
    return guarded([&](State& s) {
        s.terrain.reset(); s.renderer.clear(); s.uploadedDevice = nullptr; s.dirty = true;
        if (!path || threshold < 1 || threshold > 255) throw std::invalid_argument("Invalid path/alpha threshold");
        // A failed map change must not continue drawing the previous map.
        s.terrain = std::make_unique<wkrgba::Terrain>(wkrgba::readRgbaPng(path), uint8_t(threshold));
    });
}
int __cdecl wkRGBASync(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         const uint8_t* mask, size_t bytes, size_t stride) {
    return guarded([&](State& s) {
        if (!s.terrain || (!mask && bytes)) throw std::invalid_argument("Missing terrain or mask");
        s.terrain->sync({x,y,w,h}, {mask,bytes}, stride);
        s.dirty = true;
    });
}
int __cdecl wkRGBADraw(IDirect3DDevice9* device, float left, float top, float scale) {
    return guarded([&](State& s) {
        if (!s.terrain) throw std::runtime_error("No RGBA map loaded");
        if (s.dirty || s.uploadedDevice != device) {
            s.renderer.upload(device, s.terrain->image()); s.dirty = false; s.uploadedDevice = device;
        }
        s.renderer.draw(left, top, scale);
    });
}
void __cdecl wkRGBAClear() {
    guarded([](State& s) { s.terrain.reset(); s.renderer.clear(); s.uploadedDevice = nullptr; s.dirty = true; });
}
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        auto host=GetModuleHandleW(nullptr);
        wkrgba::loader::install(host);
    }
    return TRUE;
}
