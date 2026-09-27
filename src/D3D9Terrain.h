#pragma once
#include "Terrain.h"
#include <array>
#include <d3d9.h>
#include <unordered_map>
#include <wrl/client.h>

namespace wkrgba {
// Call from the terrain render pass, inside BeginScene, before worms and HUD.
// Every tile uses a managed A8R8G8B8 texture; classic D3D9 resets preserve it.
class D3D9Terrain {
public:
    void upload(IDirect3DDevice9* device, const Image& image);
    void uploadDynamic(IDirect3DDevice9* device, const Image& image);
    void draw(float left, float top, float scale);
    // Draw a cached full-colour map through a screen-space 8-bit visibility
    // mask. Only newly exposed map tiles are uploaded; camera movement merely
    // changes the vertices, so it stays in step with W:A's native scene.
    void drawMasked(IDirect3DDevice9* device,const Image& image,const std::vector<uint8_t>& mask,
                    const std::vector<uint8_t>* blackout,uint32_t viewportWidth,uint32_t viewportHeight,int64_t left,int64_t top);
    // Reapply the already uploaded black-background UI mask to the final
    // backbuffer immediately before Present.
    void drawFinalBlackout(IDirect3DDevice9* device,const std::vector<uint8_t>& blackout);
    void clear();
private:
    struct Tile {
        uint32_t x, y, width, height, textureWidth, textureHeight;
        std::array<Microsoft::WRL::ComPtr<IDirect3DTexture9>,3> textures;
    };
    Microsoft::WRL::ComPtr<IDirect3DDevice9> device_;
    Microsoft::WRL::ComPtr<IDirect3DStateBlock9> state_;
    std::vector<Tile> tiles_;
    bool dynamic_{};
    uint32_t dynamicFrame_{};
    D3DFORMAT format_{D3DFMT_UNKNOWN};
    struct CachedTile {
        Microsoft::WRL::ComPtr<IDirect3DTexture9> texture;
        uint32_t x{},y{},width{},height{};
        D3DFORMAT format{D3DFMT_UNKNOWN};
        bool binaryAlpha{true};
        uint64_t used{};
    };
    std::unordered_map<uint64_t,CachedTile> cache_;
    int64_t previousVisibleLeft_{},previousVisibleTop_{};
    bool previousVisible_{};
    Microsoft::WRL::ComPtr<IDirect3DTexture9> maskTexture_;
    Microsoft::WRL::ComPtr<IDirect3DTexture9> blackoutTexture_;
    uint32_t maskWidth_{},maskHeight_{};
    D3DFORMAT maskFormat_{D3DFMT_UNKNOWN};
    uint64_t useCounter_{};
    void uploadImpl(IDirect3DDevice9* device,const Image& image,bool dynamic);
};
}
