#include "D3D9Terrain.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace wkrgba {
namespace {
void check(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("wkRGBA Direct3D 9 operation failed: " + std::to_string(uint32_t(hr)));
}
struct Vertex { float x, y, z, rhw, u, v; };
struct MaskedVertex { float x,y,z,rhw,u0,v0,u1,v1; };
}
void D3D9Terrain::clear() { tiles_.clear();cache_.clear();previousVisible_=false;maskTexture_.Reset();blackoutTexture_.Reset();maskWidth_=maskHeight_=0;maskFormat_=D3DFMT_UNKNOWN;useCounter_=0;state_.Reset();device_.Reset();dynamic_=false;dynamicFrame_=0;format_=D3DFMT_UNKNOWN; }
void D3D9Terrain::upload(IDirect3DDevice9* device,const Image& image){uploadImpl(device,image,false);}
void D3D9Terrain::uploadDynamic(IDirect3DDevice9* device,const Image& image){uploadImpl(device,image,true);}
void D3D9Terrain::uploadImpl(IDirect3DDevice9* device, const Image& image,bool dynamic) {
    if (!device) throw std::invalid_argument("Null D3D9 device");
    image.validate();
    D3DCAPS9 caps{};check(device->GetDeviceCaps(&caps));
    const bool nonPowerOfTwo=!(caps.TextureCaps&D3DPTEXTURECAPS_POW2) ||
        (caps.TextureCaps&D3DPTEXTURECAPS_NONPOW2CONDITIONAL);
    // A dynamic upload is the already-cropped viewport. On the shader renderer
    // it fits in one NPOT texture, avoiding dozens of locks and draw calls per
    // frame. Full-map/static uploads retain bounded 256x256 tiles.
    const bool single=dynamic && nonPowerOfTwo && image.width<=caps.MaxTextureWidth && image.height<=caps.MaxTextureHeight;
    const auto tileWidth=single?image.width:256u,tileHeight=single?image.height:256u;
    const auto columns=(image.width+tileWidth-1)/tileWidth,rows=(image.height+tileHeight-1)/tileHeight;
    bool reusable=device_.Get()==device && dynamic_==dynamic && tiles_.size()==size_t(columns)*rows;
    if(reusable) {
        size_t i=0;
        for(uint32_t y=0;y<image.height && reusable;y+=tileHeight)for(uint32_t x=0;x<image.width;x+=tileWidth,++i)
            reusable=tiles_[i].x==x && tiles_[i].y==y && tiles_[i].width==(std::min)(tileWidth,image.width-x) &&
                     tiles_[i].height==(std::min)(tileHeight,image.height-y) &&
                     tiles_[i].textureWidth==(single?image.width:256u) && tiles_[i].textureHeight==(single?image.height:256u);
    }
    if(!reusable) {
        if(caps.MaxTextureWidth<256 || caps.MaxTextureHeight<256)throw std::runtime_error("D3D9 device cannot support 256x256 terrain tiles");
        std::vector<Tile> next;
        next.reserve(size_t(columns)*rows);
        D3DFORMAT selected=D3DFMT_A8B8G8R8;
        for(uint32_t y=0;y<image.height;y+=tileHeight)for(uint32_t x=0;x<image.width;x+=tileWidth) {
            const auto width=(std::min)(tileWidth,image.width-x),height=(std::min)(tileHeight,image.height-y);
            Tile tile{x,y,width,height,single?width:256u,single?height:256u,{}};
            auto hr=device->CreateTexture(tile.textureWidth,tile.textureHeight,1,dynamic?D3DUSAGE_DYNAMIC:0,selected,
                dynamic?D3DPOOL_DEFAULT:D3DPOOL_MANAGED,&tile.textures[0],nullptr);
            if(FAILED(hr) && next.empty() && selected==D3DFMT_A8B8G8R8) {
                selected=D3DFMT_A8R8G8B8;
                hr=device->CreateTexture(tile.textureWidth,tile.textureHeight,1,dynamic?D3DUSAGE_DYNAMIC:0,selected,
                    dynamic?D3DPOOL_DEFAULT:D3DPOOL_MANAGED,&tile.textures[0],nullptr);
            }
            check(hr);
            if(dynamic)for(size_t buffer=1;buffer<tile.textures.size();++buffer)
                check(device->CreateTexture(tile.textureWidth,tile.textureHeight,1,D3DUSAGE_DYNAMIC,selected,D3DPOOL_DEFAULT,&tile.textures[buffer],nullptr));
            next.push_back(std::move(tile));
        }
        tiles_=std::move(next);device_=device;state_.Reset();dynamic_=dynamic;dynamicFrame_=0;format_=selected;
    }
    if(dynamic)dynamicFrame_=(dynamicFrame_+1)%3;
    size_t tileIndex=0;
    for (uint32_t y = 0; y < image.height; y += tileHeight) {
        for (uint32_t x = 0; x < image.width; x += tileWidth) {
            auto& tile=tiles_[tileIndex++];
            auto& texture=tile.textures[dynamic?dynamicFrame_:0];
            D3DLOCKED_RECT lock{};
            check(texture->LockRect(0, &lock, nullptr, dynamic?D3DLOCK_DISCARD:0));
            for (uint32_t row = 0; row < tile.textureHeight; ++row) {
                auto* dst = static_cast<uint8_t*>(lock.pBits) + size_t(row) * lock.Pitch;
                if(format_==D3DFMT_A8B8G8R8) {
                    if(row<tile.height) {
                        const auto* source=image.pixels.data()+size_t(y+row)*image.width+x;
                        std::memcpy(dst,source,size_t(tile.width)*sizeof(Pixel));
                        if(tile.width<tile.textureWidth)std::memset(dst+size_t(tile.width)*sizeof(Pixel),0,size_t(tile.textureWidth-tile.width)*sizeof(Pixel));
                    } else std::memset(dst,0,size_t(tile.textureWidth)*sizeof(Pixel));
                } else {
                    auto* words=reinterpret_cast<uint32_t*>(dst);
                    for(uint32_t col=0;col<tile.textureWidth;++col) {
                        Pixel p{};if(row<tile.height&&col<tile.width)p=image.pixels[size_t(y+row)*image.width+x+col];
                        words[col]=uint32_t(p.a)<<24|uint32_t(p.r)<<16|uint32_t(p.g)<<8|p.b;
                    }
                }
            }
            check(texture->UnlockRect(0));
        }
    }
}
void D3D9Terrain::draw(float left, float top, float scale) {
    if (!device_) throw std::runtime_error("No terrain uploaded to D3D9");
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(scale) || scale <= 0 || scale > 64 ||
        std::abs(left) > 1e7f || std::abs(top) > 1e7f) throw std::invalid_argument("Invalid terrain projection");
    auto* d = device_.Get();
    if(!state_)check(d->CreateStateBlock(D3DSBT_ALL,&state_));
    check(state_->Capture());
    struct Restore {IDirect3DStateBlock9* state;~Restore(){if(state)state->Apply();}} restore{state_.Get()};
    check(d->SetVertexShader(nullptr));
    check(d->SetPixelShader(nullptr));
    check(d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1));
    check(d->SetStreamSourceFreq(0, 1));
    check(d->SetRenderState(D3DRS_ZENABLE, FALSE));
    check(d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
    check(d->SetRenderState(D3DRS_STENCILENABLE, FALSE));
    check(d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
    check(d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
    check(d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
    check(d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
    check(d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD));
    check(d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE));
    check(d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
    check(d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID));
    check(d->SetRenderState(D3DRS_FOGENABLE, FALSE));
    check(d->SetRenderState(D3DRS_LIGHTING, FALSE));
    check(d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE));
    check(d->SetRenderState(D3DRS_COLORWRITEENABLE, 15));
    check(d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    check(d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    check(d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    check(d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
    check(d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0));
    check(d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
    check(d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
    check(d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
    check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    check(d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    check(d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    check(d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    check(d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE));
    for (const auto& tile : tiles_) {
        const float x = left + tile.x * scale - 0.5f, y = top + tile.y * scale - 0.5f;
        const float right = x + tile.width * scale, bottom = y + tile.height * scale;
        const float u = tile.width/float(tile.textureWidth), v = tile.height/float(tile.textureHeight);
        const Vertex vertices[]{{x,y,0,1,0,0},{right,y,0,1,u,0},{x,bottom,0,1,0,v},{right,bottom,0,1,u,v}};
        check(d->SetTexture(0, tile.textures[dynamic_?dynamicFrame_:0].Get()));
        check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex)));
    }
}

void D3D9Terrain::drawMasked(IDirect3DDevice9* device,const Image& image,const std::vector<uint8_t>& mask,
                             const std::vector<uint8_t>* blackout,uint32_t viewportWidth,uint32_t viewportHeight,int64_t left,int64_t top) {
    if(!device || !viewportWidth || !viewportHeight || mask.size()!=size_t(viewportWidth)*viewportHeight ||
       (blackout&&blackout->size()!=mask.size()))
        throw std::invalid_argument("Invalid masked terrain viewport");
    image.validate();
    if(device_.Get()!=device){clear();device_=device;}
    D3DCAPS9 caps{};check(device->GetDeviceCaps(&caps));
    // Keep uploads small while the camera moves quickly. A 256x256 RGBA page
    // is 256 KiB; larger pages caused visible frame-time spikes on long maps.
    constexpr uint32_t tileSize=256;
    constexpr size_t maxCachedTiles=512;
    if(caps.MaxTextureWidth<tileSize || caps.MaxTextureHeight<tileSize)
        throw std::runtime_error("D3D9 device cannot cache 256x256 terrain tiles");
    if(cache_.empty()&&cache_.bucket_count()<maxCachedTiles)cache_.reserve(maxCachedTiles+8);

    if(!maskTexture_ || maskWidth_!=viewportWidth || maskHeight_!=viewportHeight) {
        maskTexture_.Reset();blackoutTexture_.Reset();maskWidth_=viewportWidth;maskHeight_=viewportHeight;maskFormat_=D3DFMT_A8;
        auto hr=device->CreateTexture(viewportWidth,viewportHeight,1,D3DUSAGE_DYNAMIC,maskFormat_,D3DPOOL_DEFAULT,&maskTexture_,nullptr);
        if(FAILED(hr)) {
            maskFormat_=D3DFMT_A8R8G8B8;
            check(device->CreateTexture(viewportWidth,viewportHeight,1,D3DUSAGE_DYNAMIC,maskFormat_,D3DPOOL_DEFAULT,&maskTexture_,nullptr));
        }
    }
    const auto uploadMask=[&](IDirect3DTexture9* texture,const std::vector<uint8_t>& source) {
        D3DLOCKED_RECT lock{};check(texture->LockRect(0,&lock,nullptr,D3DLOCK_DISCARD));
        for(uint32_t y=0;y<viewportHeight;++y) {
            const auto* src=source.data()+size_t(y)*viewportWidth;auto* dst=static_cast<uint8_t*>(lock.pBits)+size_t(y)*lock.Pitch;
            if(maskFormat_==D3DFMT_A8)std::memcpy(dst,src,viewportWidth);
            else {auto* words=reinterpret_cast<uint32_t*>(dst);for(uint32_t x=0;x<viewportWidth;++x)words[x]=uint32_t(src[x])<<24|0x00ffffffu;}
        }
        check(texture->UnlockRect(0));
    };
    uploadMask(maskTexture_.Get(),mask);
    if(blackout) {
        if(!blackoutTexture_)check(device->CreateTexture(viewportWidth,viewportHeight,1,D3DUSAGE_DYNAMIC,maskFormat_,D3DPOOL_DEFAULT,&blackoutTexture_,nullptr));
        uploadMask(blackoutTexture_.Get(),*blackout);
    }

    const int64_t visibleLeft=(std::max)(int64_t(0),-left),visibleTop=(std::max)(int64_t(0),-top);
    const int64_t visibleRight=(std::min)(int64_t(image.width),int64_t(viewportWidth)-left);
    const int64_t visibleBottom=(std::min)(int64_t(image.height),int64_t(viewportHeight)-top);
    if(visibleRight<=visibleLeft || visibleBottom<=visibleTop)return;
    const uint32_t firstX=uint32_t(visibleLeft)/tileSize*tileSize,firstY=uint32_t(visibleTop)/tileSize*tileSize;
    const uint32_t lastX=(uint32_t(visibleRight-1)/tileSize)*tileSize,lastY=(uint32_t(visibleBottom-1)/tileSize)*tileSize;
    const auto stamp=++useCounter_;
    const auto ensureTile=[&](uint32_t tx,uint32_t ty) {
        const uint64_t key=(uint64_t(ty)<<32)|tx;
        auto found=cache_.find(key);
        if(found==cache_.end()) {
            CachedTile tile{};tile.x=tx;tile.y=ty;tile.width=(std::min)(tileSize,image.width-tx);tile.height=(std::min)(tileSize,image.height-ty);
            // Once the bounded cache is full, recycle an old texture of the
            // same dimensions. This avoids D3D allocation/free stalls while
            // traversing maps that are much larger than the cache.
            if(cache_.size()>=maxCachedTiles) {
                auto victim=cache_.end(),matching=cache_.end();
                for(auto it=cache_.begin();it!=cache_.end();++it) {
                    if(it->second.used==stamp)continue;
                    if(victim==cache_.end()||it->second.used<victim->second.used)victim=it;
                    if(it->second.width==tile.width&&it->second.height==tile.height&&
                       (matching==cache_.end()||it->second.used<matching->second.used))matching=it;
                }
                if(matching!=cache_.end())victim=matching;
                if(victim!=cache_.end()) {
                    if(victim->second.width==tile.width&&victim->second.height==tile.height) {
                        tile.texture=std::move(victim->second.texture);
                        tile.format=victim->second.format;
                    }
                    cache_.erase(victim);
                }
            }
            // W:A's shader renderer can expose an IDirect3DDevice9Ex-backed
            // device. D3DPOOL_MANAGED is invalid there, so persistent cache
            // tiles live in DEFAULT video memory and are recreated on Reset.
            if(!tile.texture) {
                tile.format=D3DFMT_A8B8G8R8;
                auto hr=device->CreateTexture(tile.width,tile.height,1,D3DUSAGE_DYNAMIC,tile.format,D3DPOOL_DEFAULT,&tile.texture,nullptr);
                if(FAILED(hr)) {
                    tile.format=D3DFMT_A8R8G8B8;
                    check(device->CreateTexture(tile.width,tile.height,1,D3DUSAGE_DYNAMIC,tile.format,D3DPOOL_DEFAULT,&tile.texture,nullptr));
                }
            }
            D3DLOCKED_RECT pixels{};check(tile.texture->LockRect(0,&pixels,nullptr,D3DLOCK_DISCARD));
            for(uint32_t row=0;row<tile.height;++row) {
                auto* destination=static_cast<uint8_t*>(pixels.pBits)+size_t(row)*pixels.Pitch;
                const auto* source=image.pixels.data()+size_t(ty+row)*image.width+tx;
                for(uint32_t col=0;col<tile.width&&tile.binaryAlpha;++col)
                    tile.binaryAlpha=source[col].a<128||source[col].a==255;
                if(tile.format==D3DFMT_A8B8G8R8)std::memcpy(destination,source,size_t(tile.width)*sizeof(Pixel));
                else {auto* words=reinterpret_cast<uint32_t*>(destination);for(uint32_t col=0;col<tile.width;++col){const auto p=source[col];words[col]=uint32_t(p.a)<<24|uint32_t(p.r)<<16|uint32_t(p.g)<<8|p.b;}}
            }
            check(tile.texture->UnlockRect(0));
            found=cache_.emplace(key,std::move(tile)).first;
        }
        found->second.used=stamp;
        return found;
    };
    for(uint32_t ty=firstY;ty<=lastY;ty+=tileSize)for(uint32_t tx=firstX;tx<=lastX;tx+=tileSize) {
        ensureTile(tx,ty);
    }
    // Warm one small neighbouring tile per frame. Prioritize the leading edge
    // so every row/column is resident before a moving camera crosses the next
    // 256-pixel boundary; the generic ring remains the idle fallback.
    const uint32_t preFirstX=firstX>=tileSize?firstX-tileSize:0,preFirstY=firstY>=tileSize?firstY-tileSize:0;
    const uint32_t maxTileX=(image.width-1)/tileSize*tileSize,maxTileY=(image.height-1)/tileSize*tileSize;
    const uint32_t preLastX=(std::min)(maxTileX,lastX+tileSize),preLastY=(std::min)(maxTileY,lastY+tileSize);
    bool prefetched=false;
    const auto prefetch=[&](uint32_t tx,uint32_t ty) {
        if(prefetched||tx>maxTileX||ty>maxTileY)return;
        const uint64_t key=(uint64_t(ty)<<32)|tx;
        if(cache_.find(key)==cache_.end()){ensureTile(tx,ty);prefetched=true;}
    };
    if(previousVisible_) {
        const auto dx=visibleLeft-previousVisibleLeft_,dy=visibleTop-previousVisibleTop_;
        if(std::abs(dx)>=std::abs(dy) && dx>0 && lastX<maxTileX)
            for(uint32_t ty=firstY;ty<=lastY&&!prefetched;ty+=tileSize)prefetch(lastX+tileSize,ty);
        else if(std::abs(dx)>=std::abs(dy) && dx<0 && firstX>=tileSize)
            for(uint32_t ty=firstY;ty<=lastY&&!prefetched;ty+=tileSize)prefetch(firstX-tileSize,ty);
        else if(dy>0 && lastY<maxTileY)
            for(uint32_t tx=firstX;tx<=lastX&&!prefetched;tx+=tileSize)prefetch(tx,lastY+tileSize);
        else if(dy<0 && firstY>=tileSize)
            for(uint32_t tx=firstX;tx<=lastX&&!prefetched;tx+=tileSize)prefetch(tx,firstY-tileSize);
    }
    for(uint32_t ty=preFirstY;ty<=preLastY&&!prefetched;ty+=tileSize)for(uint32_t tx=preFirstX;tx<=preLastX;tx+=tileSize) {
        if(tx>=firstX&&tx<=lastX&&ty>=firstY&&ty<=lastY)continue;
        prefetch(tx,ty);if(prefetched)break;
    }
    previousVisibleLeft_=visibleLeft;previousVisibleTop_=visibleTop;previousVisible_=true;
    // A 128 MiB upper bound keeps long rope-race maps from filling video RAM.
    while(cache_.size()>maxCachedTiles) {
        auto victim=cache_.end();
        for(auto it=cache_.begin();it!=cache_.end();++it)if(it->second.used!=stamp && (victim==cache_.end()||it->second.used<victim->second.used))victim=it;
        if(victim==cache_.end())break;cache_.erase(victim);
    }

    auto* d=device;
    if(!state_)check(d->CreateStateBlock(D3DSBT_ALL,&state_));
    check(state_->Capture());
    struct Restore {IDirect3DStateBlock9* state;~Restore(){if(state)state->Apply();}} restore{state_.Get()};
    check(d->SetVertexShader(nullptr));check(d->SetPixelShader(nullptr));
    check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX2));check(d->SetStreamSourceFreq(0,1));
    check(d->SetRenderState(D3DRS_ZENABLE,FALSE));check(d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE));
    check(d->SetRenderState(D3DRS_STENCILENABLE,FALSE));check(d->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE));
    check(d->SetRenderState(D3DRS_ALPHAREF,0));check(d->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER));
    check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE));check(d->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA));
    check(d->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA));check(d->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD));
    check(d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE));check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
    check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));check(d->SetRenderState(D3DRS_FOGENABLE,FALSE));
    check(d->SetRenderState(D3DRS_LIGHTING,FALSE));check(d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE));check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));
    check(d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));check(d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE));
    check(d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));check(d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE));
    check(d->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0));check(d->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE));
    check(d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_SELECTARG1));check(d->SetTextureStageState(1,D3DTSS_COLORARG1,D3DTA_CURRENT));
    check(d->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_MODULATE));check(d->SetTextureStageState(1,D3DTSS_ALPHAARG1,D3DTA_CURRENT));check(d->SetTextureStageState(1,D3DTSS_ALPHAARG2,D3DTA_TEXTURE));
    check(d->SetTextureStageState(1,D3DTSS_TEXCOORDINDEX,1));check(d->SetTextureStageState(1,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE));
    check(d->SetTextureStageState(2,D3DTSS_COLOROP,D3DTOP_DISABLE));check(d->SetTextureStageState(2,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
    for(DWORD stage=0;stage<2;++stage){check(d->SetSamplerState(stage,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(stage,D3DSAMP_MAGFILTER,D3DTEXF_POINT));check(d->SetSamplerState(stage,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(stage,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));check(d->SetSamplerState(stage,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(stage,D3DSAMP_SRGBTEXTURE,FALSE));}
    check(d->SetTexture(1,maskTexture_.Get()));
    bool blending=true;
    for(uint32_t ty=firstY;ty<=lastY;ty+=tileSize)for(uint32_t tx=firstX;tx<=lastX;tx+=tileSize) {
        auto& tile=cache_.at((uint64_t(ty)<<32)|tx);
        const bool needsBlending=!tile.binaryAlpha;
        if(needsBlending!=blending) {
            check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,needsBlending));
            blending=needsBlending;
        }
        const float x=float(left+tx)-.5f,y=float(top+ty)-.5f,right=x+tile.width,bottom=y+tile.height;
        const float mu0=(x+.5f)/viewportWidth,mv0=(y+.5f)/viewportHeight,mu1=(right+.5f)/viewportWidth,mv1=(bottom+.5f)/viewportHeight;
        const MaskedVertex vertices[]{{x,y,0,1,0,0,mu0,mv0},{right,y,0,1,1,0,mu1,mv0},{x,bottom,0,1,0,1,mu0,mv1},{right,bottom,0,1,1,1,mu1,mv1}};
        check(d->SetTexture(0,tile.texture.Get()));check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(MaskedVertex)));
    }
    // Apply panel interiors last. This prevents even a partially covered mask
    // texel from allowing the RGBA terrain to tint an area that W:A expects to
    // remain pure black in INS background mode.
    if(blackout) {
        check(d->SetRenderState(D3DRS_TEXTUREFACTOR,0xff000000u));
        check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));
        check(d->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE));
        check(d->SetRenderState(D3DRS_ALPHAREF,0));
        check(d->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER));
        check(d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));check(d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TFACTOR));
        check(d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));check(d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE));
        check(d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE));check(d->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
        check(d->SetTexture(0,blackoutTexture_.Get()));check(d->SetTexture(1,nullptr));
        const float right=float(viewportWidth)-.5f,bottom=float(viewportHeight)-.5f;
        const MaskedVertex vertices[]{{-.5f,-.5f,0,1,0,0,0,0},{right,-.5f,0,1,1,0,0,0},{-.5f,bottom,0,1,0,1,0,0},{right,bottom,0,1,1,1,0,0}};
        check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(MaskedVertex)));
    }
    d->SetTexture(1,nullptr);
}

void D3D9Terrain::drawFinalBlackout(IDirect3DDevice9* device,const std::vector<uint8_t>& blackout) {
    if(!device||device_.Get()!=device||!blackoutTexture_||!maskWidth_||!maskHeight_)
        throw std::runtime_error("Final UI blackout is unavailable");
    if(blackout.size()!=size_t(maskWidth_)*maskHeight_)throw std::invalid_argument("Invalid final UI blackout mask");
    D3DLOCKED_RECT lock{};check(blackoutTexture_->LockRect(0,&lock,nullptr,D3DLOCK_DISCARD));
    for(uint32_t y=0;y<maskHeight_;++y) {
        const auto* src=blackout.data()+size_t(y)*maskWidth_;auto* dst=static_cast<uint8_t*>(lock.pBits)+size_t(y)*lock.Pitch;
        if(maskFormat_==D3DFMT_A8)std::memcpy(dst,src,maskWidth_);
        else {auto* words=reinterpret_cast<uint32_t*>(dst);for(uint32_t x=0;x<maskWidth_;++x)words[x]=uint32_t(src[x])<<24|0x00ffffffu;}
    }
    check(blackoutTexture_->UnlockRect(0));
    auto* d=device;
    if(!state_)check(d->CreateStateBlock(D3DSBT_ALL,&state_));
    check(state_->Capture());
    struct Restore {IDirect3DStateBlock9* state;~Restore(){if(state)state->Apply();}} restore{state_.Get()};
    check(d->SetVertexShader(nullptr));check(d->SetPixelShader(nullptr));
    check(d->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX2));check(d->SetStreamSourceFreq(0,1));
    check(d->SetRenderState(D3DRS_ZENABLE,FALSE));check(d->SetRenderState(D3DRS_ZWRITEENABLE,FALSE));
    check(d->SetRenderState(D3DRS_STENCILENABLE,FALSE));check(d->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE));
    check(d->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE));check(d->SetRenderState(D3DRS_ALPHAREF,0));
    check(d->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER));check(d->SetRenderState(D3DRS_TEXTUREFACTOR,0xff000000u));
    check(d->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));check(d->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));
    check(d->SetRenderState(D3DRS_FOGENABLE,FALSE));check(d->SetRenderState(D3DRS_LIGHTING,FALSE));
    check(d->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE));check(d->SetRenderState(D3DRS_COLORWRITEENABLE,15));
    check(d->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));check(d->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TFACTOR));
    check(d->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));check(d->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE));
    check(d->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0));check(d->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE));
    check(d->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE));check(d->SetTextureStageState(1,D3DTSS_ALPHAOP,D3DTOP_DISABLE));
    check(d->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT));check(d->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT));
    check(d->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));check(d->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
    check(d->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));check(d->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE));
    check(d->SetTexture(0,blackoutTexture_.Get()));check(d->SetTexture(1,nullptr));
    const float right=float(maskWidth_)-.5f,bottom=float(maskHeight_)-.5f;
    const MaskedVertex vertices[]{{-.5f,-.5f,0,1,0,0,0,0},{right,-.5f,0,1,1,0,0,0},{-.5f,bottom,0,1,0,1,0,0},{right,bottom,0,1,1,1,0,0}};
    check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(MaskedVertex)));
    d->SetTexture(0,nullptr);
}
}
