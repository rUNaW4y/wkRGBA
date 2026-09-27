#include "NativeCompositor.h"
#include "NativeCompositeShader.h"
#include "NativePaletteShader.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace wkrgba {
namespace {
void check(HRESULT hr) { if(FAILED(hr))throw std::runtime_error("Direct3D native compositor operation failed"); }
uint32_t powerOfTwo(uint32_t value) {
    uint32_t result=1;while(result<value&&result<0x80000000u)result<<=1;
    return result;
}
bool empty(const NativeCompositor::Rect64& r) {return r.right<=r.left||r.bottom<=r.top;}
}

void NativeCompositor::clear() noexcept {
    nativeShader_.Reset();compositeShader_.Reset();backdrop_.Reset();terrainFrame_.Reset();artworkAtlas_.Reset();artworkStaging_.Reset();
    artworkAtlasSurface_.Reset();artworkStagingSurface_.Reset();device_=nullptr;
    backdropFormat_=artworkFormat_=D3DFMT_UNKNOWN;viewportWidth_=viewportHeight_=atlasWidth_=atlasHeight_=artworkWidth_=artworkHeight_=0;
    resident_={};residentValid_=false;bordered_=false;uploadedPaletteValid_=false;collisionShadow_.clear();terrainSnapshot_.clear();shadowStride_=shadowHeight_=0;
}

void NativeCompositor::resetResources() noexcept {
    backdrop_.Reset();terrainFrame_.Reset();artworkAtlas_.Reset();artworkStaging_.Reset();artworkAtlasSurface_.Reset();artworkStagingSurface_.Reset();
    backdropFormat_=artworkFormat_=D3DFMT_UNKNOWN;
    viewportWidth_=viewportHeight_=atlasWidth_=atlasHeight_=artworkWidth_=artworkHeight_=0;resident_={};residentValid_=false;bordered_=false;uploadedPaletteValid_=false;
    collisionShadow_.clear();terrainSnapshot_.clear();shadowStride_=shadowHeight_=0;
}

bool NativeCompositor::observePixelShader(IDirect3DDevice9* device,const DWORD* function,IDirect3DPixelShader9* shader) noexcept {
    if(!device||!function||!shader)return false;
    if(std::memcmp(function,native_palette_shader::bytecode,sizeof(native_palette_shader::bytecode))!=0)return false;
    try {
        if(device_&&device_!=device)clear();device_=device;nativeShader_=shader;
        check(device->CreatePixelShader(reinterpret_cast<const DWORD*>(native_shader::bytecode),&compositeShader_));
        return true;
    } catch(...) {compositeShader_.Reset();nativeShader_.Reset();return false;}
}

void NativeCompositor::ensureTextures(IDirect3DDevice9* device,uint32_t width,uint32_t height) {
    if(!device||!width||!height)throw std::invalid_argument("Invalid native compositor viewport");
    if(device_&&device_!=device)clear();device_=device;
    if(backdrop_&&viewportWidth_==width&&viewportHeight_==height)return;
    backdrop_.Reset();terrainFrame_.Reset();artworkAtlas_.Reset();artworkStaging_.Reset();artworkAtlasSurface_.Reset();artworkStagingSurface_.Reset();
    residentValid_=false;collisionShadow_.clear();
    viewportWidth_=width;viewportHeight_=height;
    // Keep at least a sizeable camera margin resident. At 1920x1080 this is a
    // 4096x2048 cache, so ordinary rope movement performs no texture update at
    // all; only a 512-pixel strip is refreshed when the view crosses a cache
    // boundary.
    atlasWidth_=powerOfTwo(width<=UINT32_MAX-1024?width+1024:width);
    atlasHeight_=powerOfTwo(height<=UINT32_MAX-512?height+512:height);
    D3DCAPS9 caps{};check(device->GetDeviceCaps(&caps));
    if(atlasWidth_>caps.MaxTextureWidth||atlasHeight_>caps.MaxTextureHeight)
        throw std::runtime_error("Viewport exceeds the native RGBA compositor texture limit");
    backdropFormat_=D3DFMT_L8;
    auto hr=device->CreateTexture(width,height,1,D3DUSAGE_DYNAMIC,backdropFormat_,D3DPOOL_DEFAULT,&backdrop_,nullptr);
    if(FAILED(hr)) {
        backdropFormat_=D3DFMT_A8R8G8B8;
        check(device->CreateTexture(width,height,1,D3DUSAGE_DYNAMIC,backdropFormat_,D3DPOOL_DEFAULT,&backdrop_,nullptr));
    }
    check(device->CreateTexture(width,height,1,D3DUSAGE_DYNAMIC,D3DFMT_L8,D3DPOOL_DEFAULT,&terrainFrame_,nullptr));
    artworkFormat_=D3DFMT_A8B8G8R8;
    hr=device->CreateTexture(atlasWidth_,atlasHeight_,1,0,artworkFormat_,D3DPOOL_DEFAULT,&artworkAtlas_,nullptr);
    if(FAILED(hr)) {
        artworkFormat_=D3DFMT_A8R8G8B8;
        check(device->CreateTexture(atlasWidth_,atlasHeight_,1,0,artworkFormat_,D3DPOOL_DEFAULT,&artworkAtlas_,nullptr));
    }
    check(device->CreateTexture(atlasWidth_,atlasHeight_,1,0,artworkFormat_,D3DPOOL_SYSTEMMEM,&artworkStaging_,nullptr));
    check(artworkAtlas_->GetSurfaceLevel(0,&artworkAtlasSurface_));
    check(artworkStaging_->GetSurfaceLevel(0,&artworkStagingSurface_));
}

void NativeCompositor::captureBackdrop(IDirect3DDevice9* device,const uint8_t* pixels,uint32_t stride,
                                       uint32_t width,uint32_t height) {
    if(!pixels||stride<width)throw std::invalid_argument("Invalid indexed backdrop");
    ensureTextures(device,width,height);
    D3DLOCKED_RECT lock{};check(backdrop_->LockRect(0,&lock,nullptr,D3DLOCK_DISCARD));
    for(uint32_t y=0;y<height;++y) {
        const auto* source=pixels+size_t(y)*stride;auto* destination=static_cast<uint8_t*>(lock.pBits)+size_t(y)*lock.Pitch;
        if(backdropFormat_==D3DFMT_L8)std::memcpy(destination,source,width);
        else {auto* words=reinterpret_cast<uint32_t*>(destination);for(uint32_t x=0;x<width;++x)words[x]=0xff000000u|uint32_t(source[x])*0x00010101u;}
    }
    check(backdrop_->UnlockRect(0));
}

void NativeCompositor::captureTerrain(IDirect3DDevice9* device,const uint8_t* pixels,uint32_t stride,
                                      uint32_t width,uint32_t height) {
    if(!pixels||stride<width)throw std::invalid_argument("Invalid indexed terrain frame");
    ensureTextures(device,width,height);
    terrainSnapshot_.resize(size_t(width)*height);
    for(uint32_t y=0;y<height;++y)
        std::memcpy(terrainSnapshot_.data()+size_t(y)*width,pixels+size_t(y)*stride,width);
}

void NativeCompositor::buildFinalMask(const uint8_t* pixels,uint32_t stride,uint32_t width,uint32_t height,
                                      std::vector<uint8_t>& mask) const {
    if(!pixels||stride<width||width!=viewportWidth_||height!=viewportHeight_||
       terrainSnapshot_.size()!=size_t(width)*height)
        throw std::invalid_argument("Invalid final indexed frame");
    mask.resize(size_t(width)*height);
    for(uint32_t y=0;y<height;++y) {
        const auto* current=pixels+size_t(y)*stride;
        const auto* terrain=terrainSnapshot_.data()+size_t(y)*width;
        auto* output=mask.data()+size_t(y)*width;
        for(uint32_t x=0;x<width;++x)output[x]=current[x]==terrain[x]?255:0;
    }
}

void NativeCompositor::uploadFinalMask(IDirect3DDevice9* device,const std::vector<uint8_t>& mask,
                                       uint32_t width,uint32_t height) {
    if(device!=device_||width!=viewportWidth_||height!=viewportHeight_||mask.size()!=size_t(width)*height)
        throw std::invalid_argument("Invalid final ownership mask");
    D3DLOCKED_RECT lock{};check(terrainFrame_->LockRect(0,&lock,nullptr,D3DLOCK_DISCARD));
    for(uint32_t y=0;y<height;++y)
        std::memcpy(static_cast<uint8_t*>(lock.pBits)+size_t(y)*lock.Pitch,mask.data()+size_t(y)*width,width);
    check(terrainFrame_->UnlockRect(0));
}

void NativeCompositor::uploadWorldRect(const Image& artwork,const uint8_t* collision,uint32_t collisionStride,
                                       bool inverted,const uint8_t* terrain,uint32_t terrainStride,
                                       const std::array<Pixel,256>& palette,Rect64 rect) {
    rect.left=(std::max)(int64_t(0),rect.left);rect.top=(std::max)(int64_t(0),rect.top);
    rect.right=(std::min)(int64_t(artwork.width),rect.right);rect.bottom=(std::min)(int64_t(artwork.height),rect.bottom);
    if(empty(rect))return;
    for(int64_t wy=rect.top;wy<rect.bottom;) {
        const auto ay=uint32_t(wy)&(atlasHeight_-1);const auto rows=uint32_t((std::min)(rect.bottom-wy,int64_t(atlasHeight_-ay)));
        for(int64_t wx=rect.left;wx<rect.right;) {
            const auto ax=uint32_t(wx)&(atlasWidth_-1);const auto columns=uint32_t((std::min)(rect.right-wx,int64_t(atlasWidth_-ax)));
            RECT target{LONG(ax),LONG(ay),LONG(ax+columns),LONG(ay+rows)};D3DLOCKED_RECT lock{};
            check(artworkStaging_->LockRect(0,&lock,&target,0));
            for(uint32_t row=0;row<rows;++row) {
                auto* destination=static_cast<uint8_t*>(lock.pBits)+size_t(row)*lock.Pitch;
                const auto sourceY=uint32_t(wy+row);const auto* source=artwork.pixels.data()+size_t(sourceY)*artwork.width+size_t(wx);
                const auto* bits=collision+size_t(sourceY)*collisionStride;
                const auto* terrainRow=terrain+size_t(sourceY)*terrainStride+size_t(wx);
                if(artworkFormat_==D3DFMT_A8B8G8R8) {
                    auto* output=reinterpret_cast<Pixel*>(destination);
                    for(uint32_t col=0;col<columns;++col) {
                        const auto sourceX=uint32_t(wx)+col;auto pixel=source[col];const auto terrainIndex=terrainRow[col];
                        const bool solid=(((bits[sourceX>>3]>>(sourceX&7))&1)!=0)!=inverted;
                        const auto expected=nativeProxyColour(pixel),native=palette[terrainIndex];
                        const bool edge=sourceX<nativeProxyBorder||sourceY<nativeProxyBorder||
                                        sourceX+nativeProxyBorder>=artwork.width||sourceY+nativeProxyBorder>=artwork.height;
                        // W:A compacts and renumbers the terrain palette while
                        // baking editor borders. Interior ownership is already
                        // proven by the dedicated terrain-index texture and the
                        // live collision mask, so a palette comparison there is
                        // both redundant and lossy. At the outer eight pixels it
                        // remains useful: a mismatch identifies W:A's native
                        // yellow/black border and keeps it above the artwork.
                        const bool originalTerrain=native.r==expected.r&&native.g==expected.g&&native.b==expected.b;
                        if(!solid||!originalTerrain||(bordered_&&edge))pixel.a=0;output[col]=pixel;
                    }
                } else {
                    auto* output=reinterpret_cast<uint32_t*>(destination);
                    for(uint32_t col=0;col<columns;++col) {
                        const auto sourceX=uint32_t(wx)+col;auto pixel=source[col];const auto terrainIndex=terrainRow[col];
                        const bool solid=(((bits[sourceX>>3]>>(sourceX&7))&1)!=0)!=inverted;
                        const auto expected=nativeProxyColour(pixel),native=palette[terrainIndex];
                        const bool edge=sourceX<nativeProxyBorder||sourceY<nativeProxyBorder||
                                        sourceX+nativeProxyBorder>=artwork.width||sourceY+nativeProxyBorder>=artwork.height;
                        const bool originalTerrain=native.r==expected.r&&native.g==expected.g&&native.b==expected.b;
                        if(!solid||!originalTerrain||(bordered_&&edge))pixel.a=0;
                        output[col]=uint32_t(pixel.a)<<24|uint32_t(pixel.r)<<16|uint32_t(pixel.g)<<8|pixel.b;
                    }
                }
            }
            check(artworkStaging_->UnlockRect(0));
            POINT destination{LONG(ax),LONG(ay)};
            check(device_->UpdateSurface(artworkStagingSurface_.Get(),&target,artworkAtlasSurface_.Get(),&destination));
            wx+=columns;
        }
        wy+=rows;
    }
}

void NativeCompositor::updateArtwork(IDirect3DDevice9* device,const Image& artwork,const uint8_t* collision,
                                     uint32_t collisionStride,bool inverted,const uint8_t* terrain,uint32_t terrainStride,
                                     const std::array<Pixel,256>& palette,bool bordered,int64_t mapLeft,int64_t mapTop,
                                     uint32_t viewportWidth,uint32_t viewportHeight) {
    artwork.validate();if(!collision||!terrain||terrainStride<artwork.width)throw std::invalid_argument("Missing terrain surfaces");
    ensureTextures(device,viewportWidth,viewportHeight);mapLeft_=mapLeft;mapTop_=mapTop;
    artworkWidth_=artwork.width;artworkHeight_=artwork.height;
    if(uploadedPaletteValid_&&uploadedPalette_!=palette)residentValid_=false;
    uploadedPalette_=palette;uploadedPaletteValid_=true;
    if(bordered_!=bordered){bordered_=bordered;residentValid_=false;}
    Rect64 visible{(std::max)(int64_t(0),-mapLeft),(std::max)(int64_t(0),-mapTop),
                   (std::min)(int64_t(artwork.width),int64_t(viewportWidth)-mapLeft),
                   (std::min)(int64_t(artwork.height),int64_t(viewportHeight)-mapTop)};
    if(empty(visible)){residentValid_=false;return;}
    if(shadowStride_!=collisionStride||shadowHeight_!=artwork.height) {
        collisionShadow_.assign(size_t(collisionStride)*artwork.height,0);
        std::memcpy(collisionShadow_.data(),collision,size_t(collisionStride)*artwork.height);
        shadowStride_=collisionStride;shadowHeight_=artwork.height;residentValid_=false;
    }
    const auto cacheWidth=(std::min)(int64_t(artwork.width),int64_t(atlasWidth_));
    const auto cacheHeight=(std::min)(int64_t(artwork.height),int64_t(atlasHeight_));
    Rect64 desired=resident_;
    if(!residentValid_) {
        desired.left=(std::clamp)(visible.left-(cacheWidth-(visible.right-visible.left))/2,int64_t(0),int64_t(artwork.width)-cacheWidth);
        desired.top=(std::clamp)(visible.top-(cacheHeight-(visible.bottom-visible.top))/2,int64_t(0),int64_t(artwork.height)-cacheHeight);
        desired.right=desired.left+cacheWidth;desired.bottom=desired.top+cacheHeight;
        uploadWorldRect(artwork,collision,collisionStride,inverted,terrain,terrainStride,palette,desired);
    } else if(visible.left<resident_.left||visible.right>resident_.right||visible.top<resident_.top||visible.bottom>resident_.bottom) {
        constexpr int64_t step=512;
        if(visible.left<desired.left)desired.left-=(std::max)(step,desired.left-visible.left);
        if(visible.right>desired.right)desired.left+=(std::max)(step,visible.right-desired.right);
        if(visible.top<desired.top)desired.top-=(std::max)(step,desired.top-visible.top);
        if(visible.bottom>desired.bottom)desired.top+=(std::max)(step,visible.bottom-desired.bottom);
        desired.left=(std::clamp)(desired.left,int64_t(0),int64_t(artwork.width)-cacheWidth);
        desired.top=(std::clamp)(desired.top,int64_t(0),int64_t(artwork.height)-cacheHeight);
        desired.right=desired.left+cacheWidth;desired.bottom=desired.top+cacheHeight;
        if(desired.left>=resident_.right||desired.right<=resident_.left||desired.top>=resident_.bottom||desired.bottom<=resident_.top)
            uploadWorldRect(artwork,collision,collisionStride,inverted,terrain,terrainStride,palette,desired);
        else {
            if(desired.top<resident_.top)uploadWorldRect(artwork,collision,collisionStride,inverted,terrain,terrainStride,palette,{desired.left,desired.top,desired.right,resident_.top});
            if(desired.bottom>resident_.bottom)uploadWorldRect(artwork,collision,collisionStride,inverted,terrain,terrainStride,palette,{desired.left,resident_.bottom,desired.right,desired.bottom});
            const auto overlapTop=(std::max)(desired.top,resident_.top),overlapBottom=(std::min)(desired.bottom,resident_.bottom);
            if(desired.left<resident_.left)uploadWorldRect(artwork,collision,collisionStride,inverted,terrain,terrainStride,palette,{desired.left,overlapTop,resident_.left,overlapBottom});
            if(desired.right>resident_.right)uploadWorldRect(artwork,collision,collisionStride,inverted,terrain,terrainStride,palette,{resident_.right,overlapTop,desired.right,overlapBottom});
        }
    }
    // Collision is only 1 bpp.  Comparing the visible bytes is cheap and lets
    // explosions update a small rectangle while camera motion uploads only the
    // newly exposed strips.
    int64_t dirtyLeft=visible.right,dirtyTop=visible.bottom,dirtyRight=visible.left,dirtyBottom=visible.top;
    const auto byteStart=uint32_t(visible.left)>>3,byteEnd=(uint32_t(visible.right)+7)>>3;
    for(int64_t y=visible.top;y<visible.bottom;++y) {
        const auto* current=collision+size_t(y)*collisionStride;auto* old=collisionShadow_.data()+size_t(y)*collisionStride;
        for(uint32_t b=byteStart;b<byteEnd;++b)if(current[b]!=old[b]) {
            old[b]=current[b];dirtyLeft=(std::min)(dirtyLeft,int64_t(b)*8);dirtyRight=(std::max)(dirtyRight,int64_t(b+1)*8);
            dirtyTop=(std::min)(dirtyTop,y);dirtyBottom=(std::max)(dirtyBottom,y+1);
        }
    }
    if(dirtyRight>dirtyLeft&&dirtyBottom>dirtyTop) {
        // W:A's monochrome crater rim extends beyond the collision bits that
        // actually changed. Refresh a small halo as well, otherwise the lower
        // part of the rim remains cached until a device reset.
        constexpr int64_t craterHalo=6;
        uploadWorldRect(artwork,collision,collisionStride,inverted,terrain,terrainStride,palette,
                        {dirtyLeft-craterHalo,dirtyTop-craterHalo,dirtyRight+craterHalo,dirtyBottom+craterHalo});
    }
    resident_=desired;residentValid_=true;
}

bool NativeCompositor::beginNativeDraw(IDirect3DDevice9* device,IDirect3DPixelShader9* selected) {
    if(!available()||device!=device_||selected!=nativeShader_.Get()||!backdrop_||!artworkAtlas_||!terrainFrame_||!residentValid_)return false;
    check(device->SetPixelShader(compositeShader_.Get()));check(device->SetTexture(2,backdrop_.Get()));check(device->SetTexture(3,artworkAtlas_.Get()));check(device->SetTexture(4,terrainFrame_.Get()));
    const float constants[4]{float(viewportWidth_)/atlasWidth_,float(viewportHeight_)/atlasHeight_,
                             float(-mapLeft_)/atlasWidth_,float(-mapTop_)/atlasHeight_};
    check(device->SetPixelShaderConstantF(1,constants,1));
    const float bounds[4]{float(artworkWidth_)/atlasWidth_,float(artworkHeight_)/atlasHeight_,0,0};
    check(device->SetPixelShaderConstantF(2,bounds,1));
    check(device->SetSamplerState(2,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(device->SetSamplerState(2,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
    check(device->SetSamplerState(2,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(device->SetSamplerState(2,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(device->SetSamplerState(2,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
    check(device->SetSamplerState(3,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(device->SetSamplerState(3,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
    check(device->SetSamplerState(3,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(device->SetSamplerState(3,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP));check(device->SetSamplerState(3,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP));
    check(device->SetSamplerState(4,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(device->SetSamplerState(4,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
    check(device->SetSamplerState(4,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(device->SetSamplerState(4,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(device->SetSamplerState(4,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
    return true;
}

void NativeCompositor::endNativeDraw(IDirect3DDevice9* device) noexcept {
    if(!device)return;device->SetPixelShader(nativeShader_.Get());device->SetTexture(2,nullptr);device->SetTexture(3,nullptr);device->SetTexture(4,nullptr);
}
}
