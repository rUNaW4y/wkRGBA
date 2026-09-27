#include "Terrain.h"
#include "Png.h"
#include "Api.h"
#include "D3D9Terrain.h"
#include <windows.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

void require(bool b, const char* message) { if (!b) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "Expected invalid input to be rejected");
}
void terrainTest() {
    wkrgba::Image image{5,1,{{0,0,0,255},{100,101,102,0},{1,2,3,127},{4,5,6,128},{255,254,253,255}}};
    wkrgba::Terrain t(image);
    require(t.mask() == std::vector<uint8_t>({1,0,0,1,1}), "Alpha collision policy");
    require(t.image().pixels[0] == image.pixels[0], "Opaque black must remain solid and black");
    const std::array<uint8_t,1> empty{0}, solid{1};
    t.sync({4,0,1,1},empty,1);
    require(t.image().pixels[4].a == 0 && t.mask()[4] == 0, "Destruction must remove colour and collision");
    const std::array<wkrgba::Pixel,1> girder{{{23,45,67,255}}};
    t.sync({4,0,1,1},solid,1,girder,1);
    require(t.image().pixels[4] == girder[0], "New terrain must not resurrect artwork");
    t.reset();
    require(t.image().pixels[4] == image.pixels[4], "Reset restores original artwork");
    rejects([&] { t.sync({4,0,2,1},solid,1); });
    rejects([&] { t.sync({0,0,5,1},solid,1); });
    rejects([&] { t.sync({UINT32_MAX,0,1,1},solid,1); });
    rejects([&] { wkrgba::Terrain invalid(image,0); });
    rejects([] { wkrgba::checkedArea(0,1); });
    rejects([] { wkrgba::checkedArea(UINT32_MAX,UINT32_MAX); });
    require(wkrgba::checkedArea(16600,8800)==146080000u,"Large RGBA map dimensions must be accepted");
    wkrgba::Terrain padded({2,2,{{1,2,3,255},{4,5,6,255},{7,8,9,255},{10,11,12,255}}});
    std::array<uint8_t,6> mask{0,1,77,1,0,88};
    padded.sync({0,0,2,2},mask,3);
    require(padded.mask() == std::vector<uint8_t>({0,1,1,0}), "Padded stride and row order");
    rejects([&] { padded.sync({0,0,2,2},mask,SIZE_MAX); });
    require(wkrgba::overOpaque({255,0,0,128},{0,0,255,255}) == wkrgba::Pixel{128,0,127,255}, "Straight alpha blend");
}
void pngTest(const std::filesystem::path& dir) {
    wkrgba::Image input{257,3,{}};
    for (unsigned i = 0; i < input.width * input.height; ++i)
        input.pixels.push_back({uint8_t(i),uint8_t(i>>8),uint8_t(i*17),uint8_t(i%256)});
    auto path = dir / L"test-è-RGBA.png";
    wkrgba::writeRgbaPng(path,input);
    auto output = wkrgba::readRgbaPng(path);
    require(output.width == input.width && output.height == input.height && output.pixels == input.pixels,
        "RGBA PNG round trip must retain every channel including invisible RGB");
    std::ifstream encodedFile(path,std::ios::binary);std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(encodedFile)),{});
    const std::vector<uint8_t> privateData{'w','k','P','a','l','e','t','t','e'};
    auto augmented=wkrgba::attachPrivatePayload(encoded,privateData);auto extracted=wkrgba::extractPrivatePayload(augmented);
    require(extracted && *extracted==privateData,"Private PNG payload round trip");
    require(wkrgba::readPngMemory(augmented).pixels==input.pixels,"Private ancillary chunk must not change decoded artwork");
    std::vector<uint8_t> levelA(40,0x11),levelB(40,0x22);
    auto levelPng=wkrgba::attachWaLevelMetadata(encoded,levelA);
    levelPng=wkrgba::attachWaLevelMetadata(levelPng,levelB);
    require(wkrgba::extractWaLevelMetadata(levelPng)==levelB,"Updated waLV metadata must replace stale border/hole state");
    rejects([&]{wkrgba::attachPrivatePayload(augmented,privateData);});
    require(wkRGBALoad(path.c_str(),128) == 1, "DLL load PNG");
    std::array<uint8_t,1> mask{0};
    require(wkRGBASync(0,0,1,1,mask.data(),1,1) == 1, "DLL mask synchronization");
    require(wkRGBASync(0,0,1,1,nullptr,1,1) == 0, "DLL must reject null buffer");
    require(wkRGBADraw(nullptr,0,0,1) == 0, "DLL must reject null D3D device");
    require(wkRGBALoad((dir / "missing.png").c_str(),128) == 0, "DLL must report bad map load");
    require(wkRGBASync(0,0,1,1,mask.data(),1,1) == 0, "Failed map load clears previous map");
    wkRGBAClear();
    auto broken = dir / "broken.png";
    { std::ofstream f(broken,std::ios::binary); f << "not a png"; }
    rejects([&] { wkrgba::readRgbaPng(broken); });
    auto truncated = dir / "truncated.png";
    { std::ifstream f(path,std::ios::binary); std::array<char,33> h{}; f.read(h.data(),h.size());
      std::ofstream o(truncated,std::ios::binary); o.write(h.data(),h.size()); }
    rejects([&] { wkrgba::readRgbaPng(truncated); });
    auto corrupt = dir / "bad-crc.png";
    std::filesystem::copy_file(path,corrupt);
    { std::fstream f(corrupt,std::ios::binary|std::ios::in|std::ios::out);
      f.seekg(29); char byte{}; f.get(byte); byte^=1; f.seekp(29); f.put(byte); }
    rejects([&] { wkrgba::readRgbaPng(corrupt); });
    auto rgb = dir / "rgb-not-rgba.png";
    std::filesystem::copy_file(path,rgb);
    { std::fstream f(rgb,std::ios::binary|std::ios::in|std::ios::out); f.seekp(25); f.put(2); }
    rejects([&] { wkrgba::readRgbaPng(rgb); });
    std::cout << "PNG round trip: 771 RGBA pixels, >112 colours, Unicode path OK\n";
}
void d3dTest() {
    using Microsoft::WRL::ComPtr;
    auto window = CreateWindowExW(0,L"STATIC",L"wkRGBA test",WS_POPUP,0,0,300,16,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    require(window != nullptr,"Hidden D3D9 test window");
    struct WindowScope { HWND w; ~WindowScope(){DestroyWindow(w);} } windowScope{window};
    ComPtr<IDirect3D9> d3d; d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    require(bool(d3d),"D3D9 runtime");
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = window; pp.BackBufferWidth = 300; pp.BackBufferHeight = 16;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    ComPtr<IDirect3DDevice9> device;
    require(SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&device)),"D3D9 HAL device");
    wkrgba::Image image{257,3,{}};
    for (unsigned y=0;y<3;++y) for(unsigned x=0;x<257;++x)
        image.pixels.push_back({uint8_t(x),uint8_t(x>>8),uint8_t(y*60),255});
    image.pixels[2] = {255,0,0,128};
    image.pixels[3] = {255,0,0,0};
    wkrgba::D3D9Terrain renderer;
    renderer.uploadDynamic(device.Get(),image);
    image.pixels[0]={12,34,56,255};
    renderer.uploadDynamic(device.Get(),image);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_CW);
    device->Clear(0,nullptr,D3DCLEAR_TARGET,0xFF0000FF,1,0);
    require(SUCCEEDED(device->BeginScene()),"BeginScene");
    renderer.draw(0,0,1);
    require(SUCCEEDED(device->EndScene()),"EndScene");
    DWORD state{}; device->GetRenderState(D3DRS_CULLMODE,&state);
    require(state==D3DCULL_CW,"D3D render state restored");
    ComPtr<IDirect3DSurface9> target, readback;
    require(SUCCEEDED(device->GetRenderTarget(0,&target)),"Render target");
    require(SUCCEEDED(device->CreateOffscreenPlainSurface(300,16,D3DFMT_X8R8G8B8,D3DPOOL_SYSTEMMEM,&readback,nullptr)),"Readback surface");
    require(SUCCEEDED(device->GetRenderTargetData(target.Get(),readback.Get())),"GPU readback");
    D3DLOCKED_RECT locked{};
    require(SUCCEEDED(readback->LockRect(&locked,nullptr,D3DLOCK_READONLY)),"Readback lock");
    bool correct=true;
    for(unsigned y=0;y<3;++y) for(unsigned x=0;x<257;++x) {
        auto p=wkrgba::overOpaque(image.pixels[y*257+x],{0,0,255,255});
        auto colour=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits)+size_t(y)*locked.Pitch)[x]&0xFFFFFF;
        if(colour!=(uint32_t(p.r)<<16|uint32_t(p.g)<<8|p.b)) correct=false;
    }
    readback->UnlockRect();
    require(correct,"D3D9 output: reused textures, all colours, tile seam and alpha must match CPU reference");
    renderer.clear();
    target.Reset(); readback.Reset();
    require(SUCCEEDED(device->Reset(&pp)),"D3D9 reset after releasing renderer");
    renderer.upload(device.Get(),image);
    require(SUCCEEDED(device->BeginScene()),"BeginScene after reset");
    renderer.draw(1,1,1);
    require(SUCCEEDED(device->EndScene()),"Draw after device reset");
    renderer.clear();
    std::cout << "D3D9 HAL readback: colours, tile boundary, alpha, state restore OK\n";
}
int main() {
    const auto dir=std::filesystem::temp_directory_path() / ("wkRGBA-test-"+std::to_string(GetCurrentProcessId()));
    try {
        std::filesystem::create_directory(dir);
        terrainTest(); pngTest(dir); d3dTest();
        // The directory is process-specific and contains only files made above.
        for(const auto& p : std::filesystem::directory_iterator(dir)) std::filesystem::remove(p.path());
        std::filesystem::remove(dir);
        std::cout << "All wkRGBA tests passed\n"; return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << "\nFixtures retained: " << dir << '\n'; return 1; }
}
