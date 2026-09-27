#pragma once
#include "Png.h"
#include <cstdint>
#include <array>
#include <vector>
#include <wrl/client.h>
#include <d3d9.h>

namespace wkrgba {

// Extends W:A's own indexed-framebuffer palette pass.  The game still draws
// the complete scene in software; the replacement pixel shader substitutes
// the original RGBA colour only when the final pixel is still the map proxy.
// Consequently worms, particles, text and panels remain in front naturally.
class NativeCompositor {
public:
    struct Rect64 { int64_t left{},top{},right{},bottom{}; };
    void clear() noexcept;
    void resetResources() noexcept;
    bool observePixelShader(IDirect3DDevice9* device,const DWORD* function,IDirect3DPixelShader9* shader) noexcept;
    bool available() const noexcept { return nativeShader_ && compositeShader_; }

    void captureBackdrop(IDirect3DDevice9* device,const uint8_t* pixels,uint32_t stride,
                         uint32_t width,uint32_t height);
    void captureTerrain(IDirect3DDevice9* device,const uint8_t* pixels,uint32_t stride,
                        uint32_t width,uint32_t height);
    void buildFinalMask(const uint8_t* pixels,uint32_t stride,uint32_t width,uint32_t height,
                        std::vector<uint8_t>& mask) const;
    void uploadFinalMask(IDirect3DDevice9* device,const std::vector<uint8_t>& mask,
                         uint32_t width,uint32_t height);
    void updateArtwork(IDirect3DDevice9* device,const Image& artwork,
                       const uint8_t* collision,uint32_t collisionStride,bool collisionInverted,
                       const uint8_t* terrain,uint32_t terrainStride,const std::array<Pixel,256>& palette,
                       bool bordered,int64_t mapLeft,int64_t mapTop,uint32_t viewportWidth,uint32_t viewportHeight);

    bool beginNativeDraw(IDirect3DDevice9* device,IDirect3DPixelShader9* selected);
    void endNativeDraw(IDirect3DDevice9* device) noexcept;
    bool isNativeDraw(IDirect3DPixelShader9* selected) const noexcept { return selected==nativeShader_.Get(); }
    IDirect3DPixelShader9* nativeShader() const noexcept { return nativeShader_.Get(); }

private:
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> nativeShader_;
    Microsoft::WRL::ComPtr<IDirect3DPixelShader9> compositeShader_;
    Microsoft::WRL::ComPtr<IDirect3DTexture9> backdrop_;
    Microsoft::WRL::ComPtr<IDirect3DTexture9> terrainFrame_;
    Microsoft::WRL::ComPtr<IDirect3DTexture9> artworkAtlas_;
    Microsoft::WRL::ComPtr<IDirect3DTexture9> artworkStaging_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> artworkAtlasSurface_;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> artworkStagingSurface_;
    IDirect3DDevice9* device_{};
    D3DFORMAT backdropFormat_{D3DFMT_UNKNOWN};
    D3DFORMAT artworkFormat_{D3DFMT_UNKNOWN};
    uint32_t viewportWidth_{},viewportHeight_{},atlasWidth_{},atlasHeight_{},artworkWidth_{},artworkHeight_{};
    int64_t mapLeft_{},mapTop_{};
    Rect64 resident_{};
    bool residentValid_{};
    std::vector<uint8_t> collisionShadow_;
    std::vector<uint8_t> terrainSnapshot_;
    uint32_t shadowStride_{},shadowHeight_{};
    bool bordered_{};
    std::array<Pixel,256> uploadedPalette_{};
    bool uploadedPaletteValid_{};

    void ensureTextures(IDirect3DDevice9* device,uint32_t viewportWidth,uint32_t viewportHeight);
    void uploadWorldRect(const Image& artwork,const uint8_t* collision,uint32_t collisionStride,
                         bool collisionInverted,const uint8_t* terrain,uint32_t terrainStride,
                         const std::array<Pixel,256>& palette,Rect64 rect);
};

}
