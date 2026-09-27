#include "Transfer.h"
#include "Png.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace wkrgba::net {
namespace {
void append(Bytes& out, uint64_t n, unsigned count) {
    for(unsigned i=0;i<count;++i) out.push_back(uint8_t(n>>(i*8)));
}
struct Reader {
    std::span<const uint8_t> bytes;
    uint64_t number(unsigned count) {
        if(bytes.size()<count) throw std::invalid_argument("Truncated wkRGBA packet");
        uint64_t result=0;
        for(unsigned i=0;i<count;++i) result |= uint64_t(bytes[i])<<(i*8);
        bytes=bytes.subspan(count); return result;
    }
    Digest digest() {
        if(bytes.size()<32) throw std::invalid_argument("Truncated wkRGBA digest");
        Digest result{}; std::copy_n(bytes.begin(),32,result.begin()); bytes=bytes.subspan(32); return result;
    }
    void end() { if(!bytes.empty()) throw std::invalid_argument("Unexpected wkRGBA packet suffix"); }
};
Bytes header(Type type) { return {'W','K','R','G',uint8_t(protocolVersion),uint8_t(type),0,0}; }
Reader reader(std::span<const uint8_t> bytes, Type expected) {
    if(packetType(bytes)!=expected) throw std::invalid_argument("Unexpected wkRGBA packet type");
    return {bytes.subspan(8)};
}
void putDigest(Bytes& out, const Digest& digest) { out.insert(out.end(),digest.begin(),digest.end()); }
class Sha256 {
public:
    Sha256() {
        if(BCryptOpenAlgorithmProvider(&algorithm_,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)
            throw std::runtime_error("Cannot open SHA-256 provider");
        ULONG got{};
        if(BCryptGetProperty(algorithm_,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize_),sizeof(objectSize_),&got,0)<0)fail();
        object_.resize(objectSize_);
        if(BCryptCreateHash(algorithm_,&hash_,object_.data(),ULONG(object_.size()),nullptr,0,0)<0)fail();
    }
    ~Sha256(){if(hash_)BCryptDestroyHash(hash_);if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);}
    void add(std::span<const uint8_t> bytes) {
        while(!bytes.empty()) {
            const auto count=ULONG((std::min)(bytes.size(),size_t(ULONG_MAX)));
            if(BCryptHashData(hash_,const_cast<PUCHAR>(bytes.data()),count,0)<0)throw std::runtime_error("SHA-256 update failed");
            bytes=bytes.subspan(count);
        }
    }
    Digest finish(){Digest result{};if(BCryptFinishHash(hash_,result.data(),ULONG(result.size()),0)<0)throw std::runtime_error("SHA-256 failed");return result;}
private:
    void fail(){if(algorithm_)BCryptCloseAlgorithmProvider(algorithm_,0);algorithm_=nullptr;throw std::runtime_error("SHA-256 initialization failed");}
    BCRYPT_ALG_HANDLE algorithm_{};BCRYPT_HASH_HANDLE hash_{};ULONG objectSize_{};Bytes object_;
};
void valid(const Manifest& m) {
    if(!m.generation || !m.threshold || checkedArea(m.width,m.height)*4>maxTransferBytes)
        throw std::invalid_argument("Invalid wkRGBA manifest");
}
void putManifest(Bytes& out, const Manifest& m) {
    valid(m);if(m.payloadSize<33 || m.payloadSize>128u*1024u*1024u)throw std::invalid_argument("Invalid PNG payload size");
    append(out,m.generation,8); append(out,m.width,4); append(out,m.height,4);
    append(out,m.threshold,1); putDigest(out,m.artwork); putDigest(out,m.collision);
    append(out,m.payloadSize,4);putDigest(out,m.payload);
}
Manifest getManifest(Reader& r) {
    Manifest m{}; m.generation=r.number(8); m.width=uint32_t(r.number(4)); m.height=uint32_t(r.number(4));
    m.threshold=uint8_t(r.number(1)); m.artwork=r.digest(); m.collision=r.digest();
    m.payloadSize=uint32_t(r.number(4));m.payload=r.digest();valid(m);
    if(m.payloadSize<33 || m.payloadSize>128u*1024u*1024u)throw std::invalid_argument("Invalid PNG payload size");return m;
}
Digest collisionDigest(uint32_t w,uint32_t h,std::span<const uint8_t> mask) {
    if(mask.size()!=checkedArea(w,h)) throw std::invalid_argument("Collision mask dimensions do not match");
    Bytes header;append(header,w,4);append(header,h,4);Sha256 hash;hash.add(header);
    std::array<uint8_t,65536> normalized{};
    for(size_t offset=0;offset<mask.size();offset+=normalized.size()) {
        const auto count=(std::min)(normalized.size(),mask.size()-offset);
        for(size_t i=0;i<count;++i)normalized[i]=mask[offset+i]?1:0;
        hash.add(std::span(normalized).first(count));
    }
    return hash.finish();
}
uint64_t after(uint64_t now,uint64_t delay) {
    if(now>UINT64_MAX-delay) throw std::invalid_argument("Transfer clock overflow");
    return now+delay;
}
}
Digest sha256(std::span<const uint8_t> bytes) {
    Sha256 hash;hash.add(bytes);return hash.finish();
}
Manifest identify(const Image& image,uint8_t threshold,uint64_t generation) {
    image.validate();
    Manifest m{generation,image.width,image.height,threshold,{},{}}; valid(m);
    Bytes canonical;append(canonical,image.width,4);append(canonical,image.height,4);append(canonical,threshold,1);
    Sha256 artwork;artwork.add(canonical);
    const auto* pixels=reinterpret_cast<const uint8_t*>(image.pixels.data());
    artwork.add({pixels,image.pixels.size()*4});m.artwork=artwork.finish();
    Sha256 collision;canonical.clear();append(canonical,image.width,4);append(canonical,image.height,4);collision.add(canonical);
    std::array<uint8_t,65536> mask{};
    for(size_t offset=0;offset<image.pixels.size();offset+=mask.size()) {
        const auto count=(std::min)(mask.size(),image.pixels.size()-offset);
        for(size_t i=0;i<count;++i)mask[i]=image.pixels[offset+i].a>=threshold;
        collision.add(std::span(mask).first(count));
    }
    m.collision=collision.finish();
    return m;
}
Type packetType(std::span<const uint8_t> p) {
    if(p.size()<8 || p.size()>chunkSize+52 || !std::equal(p.begin(),p.begin()+4,"WKRG") ||
       p[4]!=protocolVersion || p[6] || p[7] || p[5]<1 || p[5]>6)
        throw std::invalid_argument("Invalid/unsupported wkRGBA envelope");
    return Type(p[5]);
}
Bytes encode(const Offer& v) { auto p=header(Type::Offer); putManifest(p,v.manifest); return p; }
Bytes encode(const Ready& v) { auto p=header(Type::Ready); putManifest(p,v.manifest); return p; }
Bytes encode(const Request& v) {
    auto p=header(Type::Request); append(p,v.generation,8); append(p,v.offset,4); putDigest(p,v.artwork); return p;
}
Bytes encode(const Chunk& v) {
    if(v.data.empty() || v.data.size()>chunkSize) throw std::invalid_argument("Invalid transfer chunk size");
    auto p=header(Type::Chunk); append(p,v.generation,8); append(p,v.offset,4); putDigest(p,v.artwork);
    p.insert(p.end(),v.data.begin(),v.data.end()); return p;
}
Offer decodeOffer(std::span<const uint8_t> p) { auto r=reader(p,Type::Offer); Offer v{getManifest(r)}; r.end(); return v; }
Ready decodeReady(std::span<const uint8_t> p) { auto r=reader(p,Type::Ready); Ready v{getManifest(r)}; r.end(); return v; }
Request decodeRequest(std::span<const uint8_t> p) {
    auto r=reader(p,Type::Request); Request v{}; v.generation=r.number(8); v.offset=uint32_t(r.number(4)); v.artwork=r.digest(); r.end(); return v;
}
Chunk decodeChunk(std::span<const uint8_t> p) {
    auto r=reader(p,Type::Chunk); Chunk v{}; v.generation=r.number(8); v.offset=uint32_t(r.number(4)); v.artwork=r.digest();
    if(r.bytes.empty() || r.bytes.size()>chunkSize) throw std::invalid_argument("Invalid transfer chunk size");
    v.data.assign(r.bytes.begin(),r.bytes.end()); return v;
}
Sender::Sender(Image image,uint8_t threshold,uint64_t generation)
    : Sender(std::make_shared<const Image>(std::move(image)),threshold,generation) {}
Sender::Sender(std::shared_ptr<const Image> image,uint8_t threshold,uint64_t generation)
    : Sender(image,std::make_shared<const Bytes>(image?writeRgbaPngMemory(*image):throw std::invalid_argument("Null RGBA image")),threshold,generation) {}
Sender::Sender(std::shared_ptr<const Image> image,std::shared_ptr<const Bytes> encodedPng,uint8_t threshold,uint64_t generation)
    : manifest_(image?identify(*image,threshold,generation):throw std::invalid_argument("Null RGBA image")),
      image_(std::move(image)),encoded_(std::move(encodedPng)) {
    if(!encoded_ || encoded_->size()<33 || encoded_->size()>128u*1024u*1024u)throw std::invalid_argument("Invalid encoded PNG");
    manifest_.payloadSize=uint32_t(encoded_->size());manifest_.payload=sha256(*encoded_);
}
Sender::Sender(Manifest manifest,std::shared_ptr<const Bytes> encodedPng)
    : manifest_(std::move(manifest)),encoded_(std::move(encodedPng)) {
    if(!encoded_ || encoded_->size()!=manifest_.payloadSize || sha256(*encoded_)!=manifest_.payload)
        throw std::invalid_argument("Encoded PNG does not match manifest");
    (void)encode(Offer{manifest_});
}
Chunk Sender::serve(const Request& request) const {
    const auto size=encoded_->size();
    if(request.generation!=manifest_.generation || request.artwork!=manifest_.artwork ||
       request.offset>=size || request.offset%chunkSize) throw std::invalid_argument("Stale/invalid RGBA request");
    const auto* begin=encoded_->data()+request.offset;
    return {manifest_.generation,request.offset,manifest_.artwork,Bytes(begin,begin+std::min(size-request.offset,size_t(chunkSize)))};
}
bool Sender::accepts(const Ready& ready) const { return ready.manifest==manifest_; }
void Receiver::fail(const char* message) {
    state_=State::Failed; image_={};encoded_.clear(); pending_=false; throw std::runtime_error(message);
}
void Receiver::reset() { *this=Receiver{}; }
void Receiver::offer(const Offer& offer,uint64_t now) {
    valid(offer.manifest);
    if(offer.manifest.generation<highestGeneration_) return;
    if(offer.manifest.generation==highestGeneration_) {
        if(offer.manifest!=manifest_) fail("Host reused map generation with different data");
        return;
    }
    const auto deadline=after(now,30000);
    const auto hardDeadline=after(now,30*60*1000);
    manifest_=offer.manifest; highestGeneration_=manifest_.generation;
    image_={};encoded_.clear(); offset_=0; pending_=false; deadline_=deadline; hardDeadline_=hardDeadline; retryAt_=now; state_=State::Receiving;
}
std::optional<Request> Receiver::request(uint64_t now) {
    if(state_!=State::Receiving) return {};
    if(now>=deadline_ || now>=hardDeadline_) fail("RGBA transfer timed out");
    if(pending_ && now<retryAt_) return {};
    pending_=true; retryAt_=after(now,1000);
    return Request{manifest_.generation,offset_,manifest_.artwork};
}
void Receiver::receive(const Chunk& chunk,uint64_t now) {
    // Delayed packets from an older map never mutate a new transfer.
    if(chunk.generation!=manifest_.generation || chunk.artwork!=manifest_.artwork) return;
    if(state_!=State::Receiving) return;
    if(now>=deadline_ || now>=hardDeadline_) fail("RGBA transfer timed out");
    if(chunk.offset<offset_) return; // harmless retransmission on reliable stream
    const uint32_t total=manifest_.payloadSize;
    if(!pending_ || chunk.offset!=offset_ || chunk.data.size()!=std::min(chunkSize,total-offset_))
        fail("Unexpected RGBA transfer chunk");
    if(encoded_.empty())encoded_.resize(total);
    std::memcpy(encoded_.data()+offset_,chunk.data.data(),chunk.data.size());
    offset_+=uint32_t(chunk.data.size()); pending_=false; retryAt_=now;
    deadline_=std::min(after(now,30000),hardDeadline_);
    if(offset_==total) {
        if(sha256(encoded_)!=manifest_.payload)fail("PNG payload integrity mismatch");
        image_=readPngMemory(encoded_);
        auto decoded=identify(image_,manifest_.threshold,manifest_.generation);
        if(decoded.width!=manifest_.width || decoded.height!=manifest_.height || decoded.threshold!=manifest_.threshold ||
           decoded.artwork!=manifest_.artwork || decoded.collision!=manifest_.collision)fail("RGBA image integrity mismatch");
        state_=State::AwaitingMap;
    }
}
void Receiver::bindNativeMap(uint32_t width,uint32_t height,std::span<const uint8_t> mask) {
    if(state_!=State::AwaitingMap && state_!=State::Ready) throw std::logic_error("RGBA transfer is not complete");
    if(width!=manifest_.width || height!=manifest_.height || collisionDigest(width,height,mask)!=manifest_.collision)
        fail("RGBA artwork does not match the native map collision mask");
    state_=State::Ready;
}
void Receiver::bindNativeProxy(std::span<const uint8_t> encodedPng) {
    if(state_!=State::AwaitingMap && state_!=State::Ready)throw std::logic_error("RGBA transfer is not complete");
    if(!matchesNativeProxyPng(image_,encodedPng))fail("RGBA artwork does not match the native PNG proxy");
    state_=State::Ready;
}
std::optional<Ready> Receiver::acknowledgement() const {
    if(state_==State::Ready) return Ready{manifest_}; return {};
}
const Image& Receiver::image() const {
    if(state_!=State::Ready) throw std::logic_error("RGBA image is not verified against the native map");
    return image_;
}
Image Receiver::takeImage() {
    if(state_!=State::Ready)throw std::logic_error("RGBA image is not verified against the native map");
    return std::move(image_);
}
Bytes Receiver::takeEncoded(){
    if(state_!=State::Ready)throw std::logic_error("RGBA image is not verified against the native map");
    return std::move(encoded_);
}
}
