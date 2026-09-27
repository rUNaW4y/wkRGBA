#include "NetworkHooks.h"
#include "InlineHook.h"
#include "Loader.h"
#include "Png.h"
#include "Transfer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace wkrgba::network {
namespace {
constexpr uint16_t packetId = 0x4B57; // bytes "WK" on the wire
constexpr uint16_t smallMapPacket = 0x21;
constexpr uint16_t colourMapPacket = 0x2B;
constexpr size_t maxNativeMapBytes = 128u * 1024u * 1024u;
constexpr size_t maxLandBytes = 64u * 1024u * 1024u;

InlineHook hostLobbyHook,hostEndHook,clientLobbyHook,clientEndHook,internalSendHook,sendGameStartHook,sendColourHook,constructHostHook,constructClientHook;
uint8_t* constructHostCallSite{};
std::array<uint8_t,5> constructHostCallOriginalBytes{};
std::filesystem::path logPath;
std::mutex stateMutex;
std::mutex logMutex;
std::mutex augmentedMutex;
std::mutex canonicalMutex;
net::Receiver receiver;
std::vector<uint8_t> nativeMap;
size_t nativeReceived{};
std::atomic<uint64_t> generation{1};
thread_local std::shared_ptr<net::Sender> approvedSender;
std::array<bool,6> probed{},supported{},reported{};
std::array<uint64_t,6> probedAt{};
std::array<UINT_PTR,6> probeTimers{};
std::array<bool,6> hostConnected{};
std::unordered_set<std::string> knownModulePlayers;
uintptr_t hostSlotAddress{},clientSlotAddress{};
uint32_t lobbyHostScreen{},lobbyClientScreen{};
uint64_t clientPresenceSentAt{};
bool clientConnected{},clientPresenceConfirmed{};
UINT_PTR sessionTimer{};
constexpr size_t encodedOfferSize=125;
constexpr std::string_view presenceText="[wkRGBA] RGBA map module installed";
struct CanonicalActivation {
    std::shared_ptr<const std::vector<uint8_t>> canonical;
    std::shared_ptr<const Image> artwork;
    std::vector<uint8_t> rgbaPng,nativePng,land;
    bool transformed{},bordered{};
};
struct AugmentedCache {
    uint32_t host{};const uint8_t* native{};uint32_t nativeSize{};
    net::Digest nativeDigest{};
    std::shared_ptr<const loader::ImportedMap> imported;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::shared_ptr<const CanonicalActivation> activation;
} augmentedCache;
struct LegacyCache {
    uint32_t host{};const uint8_t* native{};uint32_t nativeSize{};
    net::Digest nativeDigest{};
    std::shared_ptr<const loader::ImportedMap> imported;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    bool restoresTop{};
} legacyCache;
struct TransferSnapshot {
    uint32_t host{};
    const uint8_t* original{};
    uint32_t originalSize{};
    std::shared_ptr<const std::vector<uint8_t>> replacement;
    std::shared_ptr<const CanonicalActivation> activation;
    bool active{};
};
std::array<TransferSnapshot,6> transferSnapshots{};
struct HostCanonicalPin {
    uint32_t host{};
    uint8_t* original{};
    uint32_t originalSize{};
    std::shared_ptr<const std::vector<uint8_t>> replacement;
} hostCanonicalPin;

void log(const std::string& text) noexcept {
    try {
        std::lock_guard lock(logMutex);
        std::ofstream file(logPath, std::ios::app);
        file << "t=" << GetTickCount64() << " " << text << '\n';
    } catch (...) {}
}

bool readable(const void* address, size_t bytes) noexcept {
    auto cursor = uintptr_t(address);
    if (!cursor || bytes > UINTPTR_MAX - cursor) return false;
    const auto end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) || info.State != MEM_COMMIT)
            return false;
        const auto protection = info.Protect & 0xff;
        if (info.Protect & PAGE_GUARD || protection == PAGE_NOACCESS) return false;
        const auto regionEnd = uintptr_t(info.BaseAddress) + info.RegionSize;
        if (regionEnd <= cursor) return false;
        cursor = (regionEnd < end) ? regionEnd : end;
    }
    return true;
}

uint16_t word(const uint8_t* data) noexcept {
    uint16_t value{};
    std::memcpy(&value, data, sizeof(value));
    return value;
}

uint32_t dword(const uint8_t* data) noexcept {
    uint32_t value{};
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::vector<uint8_t> frame(net::Bytes payload) {
    std::vector<uint8_t> packet;
    packet.reserve(payload.size() + 2);
    packet.push_back(static_cast<uint8_t>(packetId & 0xffu));
    packet.push_back(static_cast<uint8_t>((packetId >> 8) & 0xffu));
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

using InternalSend = int (__fastcall*)(uint32_t, uint32_t, uint8_t*, size_t);

void sendEnvelope(uint32_t connection, net::Bytes payload) {
    auto packet = frame(std::move(payload));
    reinterpret_cast<InternalSend>(internalSendHook.trampoline)(connection, 0, packet.data(), packet.size());
}

net::Bytes control(net::Type type){return {'W','K','R','G',uint8_t(net::protocolVersion),uint8_t(type),0,0};}

int sendPacketToSlot(int slot,const uint8_t* data,uint32_t packetSize) {
    if(slot<0 || slot>=int(probed.size()) || !hostSlotAddress)return 0;
    const auto target=uint32_t(uintptr_t(GetModuleHandleW(nullptr))+0x18F9F0);const auto hostValue=uint32_t(hostSlotAddress);int result{};
    __asm {
        push esi
        push hostValue
        mov esi,data
        mov ecx,slot
        mov edx,packetSize
        call target
        pop esi
        mov result,eax
    }
    return result;
}

int sendToSlot(int slot,net::Bytes payload) {
    auto packet=frame(std::move(payload));
    return sendPacketToSlot(slot,packet.data(),uint32_t(packet.size()));
}

int sendLobbyTextToSlot(int slot,std::string_view text) {
    std::vector<uint8_t> packet{0,0};
    constexpr std::string_view prefix="SYS::ALL:";
    packet.insert(packet.end(),prefix.begin(),prefix.end());
    packet.insert(packet.end(),text.begin(),text.end());
    packet.push_back(0);
    return sendPacketToSlot(slot,packet.data(),uint32_t(packet.size()));
}

bool sendRawToHost(std::vector<uint8_t> packet) {
    if(!clientSlotAddress || !readable(reinterpret_cast<void*>(clientSlotAddress),sizeof(void*)))return false;
    auto* object=reinterpret_cast<int*>(clientSlotAddress);
    auto** vtable=*reinterpret_cast<void***>(object);if(!vtable || !readable(vtable+13,sizeof(void*)))return false;
    using Send=int(__thiscall*)(int*,uint8_t*,size_t);
    reinterpret_cast<Send>(vtable[13])(object,packet.data(),packet.size());return true;
}
void sendToHost(net::Bytes payload) {sendRawToHost(frame(std::move(payload)));}

std::string nickname(int slot) {
    if(!hostSlotAddress || slot<0 || slot>=int(probed.size()))return "#"+std::to_string(slot);
    const auto address=hostSlotAddress+0x1A4+uintptr_t(slot)*0x19118+0xD0;
    if(!readable(reinterpret_cast<void*>(address),80))return "#"+std::to_string(slot);
    const auto* text=reinterpret_cast<const char*>(address);const auto length=strnlen_s(text,80);
    return length&&length<80?std::string(text,length):"#"+std::to_string(slot);
}

void lobbyMessage(const std::string& text,bool client=false) noexcept {
    try {
        const auto screen=client?lobbyClientScreen:lobbyHostScreen;
        if(!screen || !readable(reinterpret_cast<void*>(screen+0x10318),4))return;
        using Display=int(__stdcall*)(int,char*);auto display=reinterpret_cast<Display>(uintptr_t(GetModuleHandleW(nullptr))+0x93CB0);
        auto line=std::string("SYS::ALL:")+text;display(int(screen+0x10318),line.data());
    }catch(...){}
}

void reportSupported(int slot) {
    std::vector<std::string> names;
    {std::lock_guard lock(stateMutex);for(int i=0;i<int(supported.size());++i)if(supported[i])names.push_back(nickname(i));}
    std::string joined;for(size_t i=0;i<names.size();++i){if(i)joined+=", ";joined+=names[i];}
    lobbyMessage("You are using wkRGBA. The following fekers have the module installed: "+joined+".");
    sendLobbyTextToSlot(slot,"The host is using wkRGBA module.");
}

void cancelProbeTimerLocked(int slot) noexcept {
    if(slot<0||slot>=int(probeTimers.size())||!probeTimers[slot])return;
    KillTimer(nullptr,probeTimers[slot]);probeTimers[slot]=0;
}

void resetHostSlotLocked(int slot) noexcept {
    if(slot<0||slot>=int(supported.size()))return;
    cancelProbeTimerLocked(slot);
    probed[slot]=supported[slot]=reported[slot]=false;
    probedAt[slot]=0;
    transferSnapshots[slot]={};
}

bool connectionState(uintptr_t object) noexcept {
    const auto address=object+0xB8;
    return object&&readable(reinterpret_cast<const void*>(address),sizeof(uint32_t))&&
           *reinterpret_cast<const uint32_t*>(address)==3;
}

void resetNative();

void observeHostSlot(int slot) noexcept {
    if(slot<0||slot>=int(hostConnected.size())||!hostSlotAddress)return;
    const auto peer=hostSlotAddress+0x1A4+uintptr_t(slot)*0x19118;
    if(!connectionState(peer))return;
    std::lock_guard lock(stateMutex);
    if(hostConnected[slot])return;
    resetHostSlotLocked(slot);
    hostConnected[slot]=true;
    const auto name=nickname(slot);
    if(knownModulePlayers.contains(name)) {
        probed[slot]=supported[slot]=reported[slot]=true;
        log("Restored wkRGBA capability for reconnected player "+name);
    }
    log("New host peer generation detected in slot "+std::to_string(slot));
}

void observeClientConnection() noexcept {
    if(!connectionState(clientSlotAddress))return;
    std::lock_guard lock(stateMutex);
    if(clientConnected)return;
    clientConnected=true;
    clientPresenceSentAt=0;
    clientPresenceConfirmed=false;
    log("New client connection generation detected");
}

void CALLBACK onSessionTimer(HWND,UINT,UINT_PTR,DWORD) {
    try {
        std::array<bool,6> connected{};
        for(int slot=0;slot<int(connected.size());++slot) {
            const auto peer=hostSlotAddress+0x1A4+uintptr_t(slot)*0x19118;
            connected[slot]=connectionState(peer);
        }
        const bool outbound=connectionState(clientSlotAddress);
        std::lock_guard lock(stateMutex);
        for(int slot=0;slot<int(connected.size());++slot) {
            if(connected[slot]==hostConnected[slot])continue;
            resetHostSlotLocked(slot);
            hostConnected[slot]=connected[slot];
            log(std::string(connected[slot]?"Host peer connected in slot ":"Host peer disconnected from slot ")+std::to_string(slot));
        }
        if(outbound!=clientConnected) {
            clientConnected=outbound;
            clientPresenceSentAt=0;
            clientPresenceConfirmed=false;
            if(!outbound)resetNative();
            log(outbound?"Client connection generation started":"Client connection generation ended");
        }
    }catch(...){}
}

void ensureSessionTimer() noexcept {
    std::lock_guard lock(stateMutex);
    if(!sessionTimer)sessionTimer=SetTimer(nullptr,0,500,onSessionTimer);
}

void CALLBACK onProbeTimer(HWND,UINT,UINT_PTR timer,DWORD);

void ensureProbe(int slot) {
    if(slot<0||slot>=int(probed.size()))return;
    std::lock_guard lock(stateMutex);
    if(probed[slot])return;
    probed[slot]=true;probedAt[slot]=GetTickCount64();
    probeTimers[slot]=SetTimer(nullptr,0,8000,onProbeTimer);
}

void reportUnsupported(int slot,bool force=false) {
    bool announce=false;
    {std::lock_guard lock(stateMutex);
        if(slot>=0&&slot<int(reported.size())&&probed[slot]&&!supported[slot]&&!reported[slot]&&
           (force||GetTickCount64()-probedAt[slot]>=8000)) {
            reported[slot]=true;cancelProbeTimerLocked(slot);announce=true;
        }
    }
    if(!announce)return;
    sendLobbyTextToSlot(slot,"The host is using wkRGBA. You do not have this module installed, feker! The selected map will suck!");
    std::vector<std::string> names;
    {
        std::lock_guard lock(stateMutex);
        for(int i=0;i<int(reported.size());++i)if(reported[i]&&!supported[i])names.push_back(nickname(i));
    }
    std::string joined;
    for(size_t i=0;i<names.size();++i){if(i)joined+=", ";joined+=names[i];}
    lobbyMessage("You are using wkRGBA. The following fekers do not have the module installed: "+joined+".");
    log("Missing-module lobby warning sent to slot "+std::to_string(slot));
}

void CALLBACK onProbeTimer(HWND,UINT,UINT_PTR timer,DWORD) {
    int slot=-1;
    {
        std::lock_guard lock(stateMutex);
        for(int i=0;i<int(probeTimers.size());++i)if(probeTimers[i]==timer){probeTimers[i]=0;slot=i;break;}
    }
    if(slot>=0)reportUnsupported(slot);
}

void sendArtwork(uint32_t connection, const net::Sender& sender) {
    const auto begin=GetTickCount64();
    sendEnvelope(connection, net::encode(net::Offer{sender.manifest()}));
    const auto total = sender.manifest().payloadSize;
    for (uint32_t offset = 0; offset < total; offset += net::chunkSize) {
        net::Request request{sender.manifest().generation, offset, sender.manifest().artwork};
        sendEnvelope(connection, net::encode(sender.serve(request)));
    }
    log("RGBA artwork queued before native map packet in "+std::to_string(GetTickCount64()-begin)+" ms");
}

std::shared_ptr<net::Sender> senderForNative(std::span<const uint8_t> serialized) {
    if (serialized.size() < 12 || serialized.size() > maxNativeMapBytes || dword(serialized.data()) != 3) return {};
    auto imported = loader::networkImport();
    if (!imported) return {};
    const auto encodedNative=serialized.subspan(4);
    if(!imported->nativePng.empty()) {
        if(!pngDimensionsMatch(encodedNative,imported->width,imported->height))return {};
    } else {
        auto artwork=imported->artwork();auto native=readPngMemory(encodedNative,false);
        if(!matchesNativeProxy(*artwork,native))return {};
    }
    auto id = generation.fetch_add(1);
    if (!id) id = generation.fetch_add(1);
    std::shared_ptr<const net::Bytes> png(imported,&imported->rgbaPng);
    auto manifest=imported->manifest;manifest.generation=id;
    return std::make_shared<net::Sender>(std::move(manifest),std::move(png));
}

std::shared_ptr<net::Sender> senderFromHost(uint32_t hostThis) {
    if (!hostThis || !readable(reinterpret_cast<const void*>(hostThis + 0x3A514), 8)) return {};
    const auto bytes = *reinterpret_cast<const uint32_t*>(hostThis + 0x3A514);
    const auto data = *reinterpret_cast<const uint8_t* const*>(hostThis + 0x3A518);
    if (bytes < 12 || bytes > maxNativeMapBytes || !readable(data, bytes)) return {};
    return senderForNative({data, bytes});
}

std::vector<uint8_t> encodeNative(const Image& image,std::span<const uint8_t> metadata) {
    wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
    if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"wrg",0,tempFile))
        throw std::runtime_error("Cannot create canonical native PNG");
    struct Cleanup { wchar_t* path; ~Cleanup(){DeleteFileW(path);} } cleanup{tempFile};
    writeNativePng(tempFile,image,128,metadata);
    std::ifstream input(tempFile,std::ios::binary|std::ios::ate);const auto length=input.tellg();
    if(!input||length<33||length>std::streamoff(maxNativeMapBytes))
        throw std::runtime_error("Invalid canonical native PNG size");
    std::vector<uint8_t> png(static_cast<size_t>(length));input.seekg(0);
    if(!input.read(reinterpret_cast<char*>(png.data()),length))
        throw std::runtime_error("Cannot read canonical native PNG");
    return png;
}

struct CanonicalParts {
    std::shared_ptr<const Image> artwork;
    std::vector<uint8_t> rgba,native;
    bool transformed{},bordered{};
};
CanonicalParts canonicalParts(const loader::ImportedMap& imported,const loader::ImportedMap& pristine,
                              std::span<const uint8_t> serializedNative) {
    CanonicalParts result{imported.artwork(),imported.rgbaPng,
                          std::vector<uint8_t>(serializedNative.begin(),serializedNative.end()),false,false};
    bool bordered=imported.bordered;
    auto metadata=extractWaLevelMetadata(serializedNative);
    if(metadata&&metadata->size()>=20) {
        const auto* state=metadata->data()+16;
        const uint32_t noBorders=uint32_t(state[0])|uint32_t(state[1])<<8|
                                 uint32_t(state[2])<<16|uint32_t(state[3])<<24;
        bordered=noBorders==0;
    }
    result.bordered=bordered;
    // Maps with natural air (Rope Race included) and bordered maps already
    // have valid W:A geometry. They must pass through without decoding or
    // duplicating the potentially enormous RGBA bitmap.
    const auto rows=nativeProxyTopClearance(*pristine.artwork());
    if(bordered) {
        // A border replaces the artificial teleporting clearance. Build the
        // canonical map at the exact source height, from the pristine artwork,
        // and retain W:A's current border flags in waLV. This also removes the
        // editor-only 128-row strip instead of transmitting a cropped map.
        auto canonical=*pristine.artwork();
        result.native=pristine.nativePng;
        if(metadata)result.native=attachWaLevelMetadata(result.native,*metadata);
        result.artwork=std::make_shared<Image>(std::move(canonical));
        return result;
    }
    if(rows!=nativeProxyFullMapTopClearance) {
        return result;
    }

    auto padded=std::make_shared<Image>(addTransparentTop(*pristine.artwork(),rows));
    result.native=resizeIndexedPngTop(pristine.nativePng,padded->height,0);
    if(metadata)result.native=attachWaLevelMetadata(result.native,*metadata);
    result.rgba=writeRgbaPngMemory(*padded);
    if(auto level=extractWaLevelMetadata(imported.rgbaPng))result.rgba=attachWaLevelMetadata(result.rgba,*level);
    result.artwork=std::move(padded);result.transformed=true;
    return result;
}

std::vector<uint8_t> editorLandSnapshot(uint32_t width,uint32_t height) {
    try {
        const auto path=logPath.parent_path().parent_path()/L"DATA"/L"land.dat";
        std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return {};
        const auto length=input.tellg();if(length<32||length>std::streamoff(maxLandBytes))return {};
        std::vector<uint8_t> land(static_cast<size_t>(length));input.seekg(0);
        if(!input.read(reinterpret_cast<char*>(land.data()),length)||dword(land.data())!=0x1a444e4c||
           dword(land.data()+8)!=width||dword(land.data()+12)!=height)return {};
        return land;
    }catch(...){return {};}
}

std::shared_ptr<const std::vector<uint8_t>> augmentedMap(uint32_t hostThis) {
    if(!hostThis || !readable(reinterpret_cast<const void*>(hostThis+0x3A514),8))return {};
    const auto bytes=*reinterpret_cast<const uint32_t*>(hostThis+0x3A514);
    const auto data=*reinterpret_cast<const uint8_t* const*>(hostThis+0x3A518);
    if(bytes<12 || bytes>maxNativeMapBytes || !readable(data,bytes) || dword(data)!=3)return {};
    auto imported=loader::networkImport();if(!imported)return {};
    const auto native=std::span(data+4,bytes-4);const auto digest=net::sha256(native);
    std::lock_guard cacheLock(augmentedMutex);
    if(augmentedCache.host==hostThis && augmentedCache.native==data && augmentedCache.nativeSize==bytes &&
       augmentedCache.nativeDigest==digest && augmentedCache.imported==imported && augmentedCache.bytes)return augmentedCache.bytes;
    if(!pngDimensionsMatch(native,imported->width,imported->height))return {};
    auto pristine=loader::networkPristineImport();if(!pristine)return {};
    auto parts=canonicalParts(*imported,*pristine,native);
    auto land=editorLandSnapshot(pristine->width,pristine->height);
    if(!land.empty()) {
        land=loader::canonicalizeNetworkLand(std::move(land),*pristine->artwork(),parts.artwork->height,parts.bordered);
        auto canonical=loader::applyNetworkLandCollision(land,*parts.artwork);
        auto metadata=extractWaLevelMetadata(parts.native);
        // LAND is the sole authority for generated holes. Clearing the waLV
        // request prevents a second, peer-local placement pass.
        if(metadata&&metadata->size()>=32)
            for(unsigned i=0;i<4;++i)(*metadata)[28+i]=0;
        parts.native=encodeNative(canonical,metadata?std::span<const uint8_t>(*metadata):std::span<const uint8_t>{});
        if(metadata)parts.native=attachWaLevelMetadata(parts.native,*metadata);
        parts.rgba=writeRgbaPngMemory(canonical);
        if(metadata)parts.rgba=attachWaLevelMetadata(parts.rgba,*metadata);
        parts.artwork=std::make_shared<Image>(std::move(canonical));parts.transformed=true;
        log("Canonical artwork and collision rebuilt from host LAND (holes="+
            std::to_string(dword(land.data()+24))+", locations="+
            std::to_string(dword(land.data()+28))+")");
    }
    auto id=generation.fetch_add(1);if(!id)id=generation.fetch_add(1);
    auto manifest=parts.transformed?net::identify(*parts.artwork,128,id):imported->manifest;
    manifest.generation=id;manifest.payloadSize=uint32_t(parts.rgba.size());manifest.payload=net::sha256(parts.rgba);
    loader::setNetworkCanonicalLand(land);
    auto offer=net::encode(net::Offer{manifest});net::Bytes payload;
    payload.reserve(offer.size()+parts.rgba.size()+(land.empty()?0:40+land.size()));
    payload.insert(payload.end(),offer.begin(),offer.end());payload.insert(payload.end(),parts.rgba.begin(),parts.rgba.end());
    if(!land.empty()) {
        payload.insert(payload.end(),{'W','K','L','D'});
        const auto size=uint32_t(land.size());for(unsigned i=0;i<4;++i)payload.push_back(uint8_t(size>>(i*8)));
        const auto digestLand=net::sha256(land);payload.insert(payload.end(),digestLand.begin(),digestLand.end());
        payload.insert(payload.end(),land.begin(),land.end());
        log("Canonical payload includes exact host land.dat ("+std::to_string(land.size())+" bytes, holes="+
            std::to_string(dword(land.data()+24))+", locations="+std::to_string(dword(land.data()+28))+")");
    }
    auto canonical=std::make_shared<std::vector<uint8_t>>(4);std::memcpy(canonical->data(),data,4);
    canonical->insert(canonical->end(),parts.native.begin(),parts.native.end());
    auto png=attachPrivatePayload(parts.native,payload);auto result=std::make_shared<std::vector<uint8_t>>(4);std::memcpy(result->data(),data,4);
    result->insert(result->end(),png.begin(),png.end());
    auto activation=std::make_shared<CanonicalActivation>();
    activation->canonical=std::move(canonical);activation->artwork=std::move(parts.artwork);
    activation->rgbaPng=std::move(parts.rgba);activation->nativePng=std::move(parts.native);
    activation->land=std::move(land);activation->transformed=parts.transformed;activation->bordered=parts.bordered;
    augmentedCache={hostThis,data,bytes,digest,std::move(imported),result,std::move(activation)};
    log("Canonical RGBA payload prepared once for native map transfer ("+std::to_string(result->size())+" bytes)");
    return result;
}

std::shared_ptr<const std::vector<uint8_t>> legacyMap(uint32_t hostThis) {
    if(!hostThis || !readable(reinterpret_cast<const void*>(hostThis+0x3A514),8))return {};
    const auto bytes=*reinterpret_cast<const uint32_t*>(hostThis+0x3A514);
    const auto data=*reinterpret_cast<const uint8_t* const*>(hostThis+0x3A518);
    if(bytes<12 || bytes>maxNativeMapBytes || !readable(data,bytes) || dword(data)!=3)return {};
    auto imported=loader::networkImport();if(!imported||imported->nativePng.empty())return {};
    const auto serialized=std::span(data+4,bytes-4);const auto digest=net::sha256(serialized);
    std::lock_guard cacheLock(augmentedMutex);
    if(legacyCache.host==hostThis && legacyCache.native==data && legacyCache.nativeSize==bytes &&
       legacyCache.nativeDigest==digest && legacyCache.imported==imported && legacyCache.bytes)return legacyCache.bytes;
    // Begin with the exact editor map already accepted by W:A. A completely
    // solid replacement is not a valid legacy playable map, even though it is
    // a syntactically valid PNG.
    std::vector<uint8_t> png(data+4,data+bytes);
    bool restoresTop=false;
        const auto artwork=imported->artwork();const auto rows=(std::min)(nativeProxyTopClearance(*artwork),nativeProxyEditorTopClearance);
    if(rows&&pngDimensionsMatch(imported->nativePng,imported->width,imported->height)) {
        // Preserve holes made by W:A below the temporary placement strip, but
        // reconstruct the original solid rows above it.  Encoding the result
        // once and sending those exact bytes to the legacy client gives both
        // peers the offline geometry without adding height or offsets.
        auto native=readPngMemory(imported->nativePng,false);Image collision=*artwork;
        bool generatedAir=false;
        for(uint32_t y=rows;y<native.height&&!generatedAir;++y)for(uint32_t x=0;x<native.width;++x) {
            const auto p=native.pixels[size_t(y)*native.width+x];
            if(p.r==0&&p.g==0&&p.b==0){generatedAir=true;break;}
        }
        if(!generatedAir) {
            auto result=std::make_shared<std::vector<uint8_t>>(4);std::memcpy(result->data(),data,4);
            result->insert(result->end(),png.begin(),png.end());
            legacyCache={hostThis,data,bytes,digest,std::move(imported),result,false};
            log("112-colour compatibility map retained W:A's editor placement geometry");
            return result;
        }
        for(uint32_t y=rows;y<collision.height;++y)for(uint32_t x=0;x<collision.width;++x) {
            const auto p=native.pixels[size_t(y)*native.width+x];
            if(p.r==0&&p.g==0&&p.b==0)collision.pixels[size_t(y)*collision.width+x].a=0;
        }
        wchar_t tempDir[MAX_PATH+1]{},tempFile[MAX_PATH+1]{};
        if(!GetTempPathW(MAX_PATH,tempDir)||!GetTempFileNameW(tempDir,L"wmp",0,tempFile))return {};
        const auto metadata=extractWaLevelMetadata(png);
        writeNativePng(tempFile,collision,128,metadata?std::span<const uint8_t>(*metadata):std::span<const uint8_t>{});
        std::ifstream input(tempFile,std::ios::binary|std::ios::ate);const auto length=input.tellg();
        if(!input||length<33||length>std::streamoff(maxNativeMapBytes)){DeleteFileW(tempFile);return {};}
        png.resize(size_t(length));input.seekg(0);
        if(!input.read(reinterpret_cast<char*>(png.data()),length)) {input.close();DeleteFileW(tempFile);return {};}
        input.close();DeleteFileW(tempFile);
        restoresTop=true;
    }
    auto result=std::make_shared<std::vector<uint8_t>>(4);std::memcpy(result->data(),data,4);
    result->insert(result->end(),png.begin(),png.end());
    legacyCache={hostThis,data,bytes,digest,std::move(imported),result,restoresTop};
    log(restoresTop
        ?"112-colour compatibility map prepared at native dimensions with source top rows restored"
        :"112-colour compatibility map retained at W:A native dimensions");
    return result;
}

bool legacyMapRestoresTop(const std::shared_ptr<const std::vector<uint8_t>>& map) {
    std::lock_guard lock(augmentedMutex);
    return map&&legacyCache.bytes==map&&legacyCache.restoresTop;
}

bool hasLegacyPeer() {
    std::lock_guard lock(stateMutex);
    for(size_t slot=0;slot<hostConnected.size();++slot)
        if(hostConnected[slot]&&!supported[slot])return true;
    return false;
}

std::shared_ptr<const std::vector<uint8_t>> exactCanonicalMap(uint32_t hostThis,bool& activated) {
    activated=false;
    try {
        std::shared_ptr<const CanonicalActivation> activation;
        {
            std::lock_guard lock(stateMutex);
            for(size_t slot=0;slot<transferSnapshots.size();++slot) {
                const auto& snapshot=transferSnapshots[slot];
                if(hostConnected[slot]&&supported[slot]&&snapshot.active&&snapshot.host==hostThis&&snapshot.activation) {
                    activation=snapshot.activation;break;
                }
            }
        }
        if(activation)log("Host game start selected the canonical snapshot already transferred to the peer");
        else {
            auto result=augmentedMap(hostThis);if(!result)return {};
            std::lock_guard lock(augmentedMutex);
            if(augmentedCache.bytes!=result)return {};
            activation=augmentedCache.activation;
        }
        if(!activation||!activation->canonical||!activation->artwork)return {};
        // Natural-air and bordered maps stay entirely under W:A's native
        // terrain generation. Only the fully opaque borderless case needs a
        // larger canonical map at game start.
        if(!activation->transformed&&!activation->bordered)return {};
        loader::setNetworkCanonicalLand(activation->land);
        activated=loader::setNetworkCanonicalGameplayExact(*activation->artwork,
                                                             activation->rgbaPng,
                                                             activation->nativePng);
        if(activated)log("Host activated the exact canonical map already serialized to wkRGBA peers");
        return activated?activation->canonical:std::shared_ptr<const std::vector<uint8_t>>{};
    } catch(const std::exception& error) {
        log(std::string("Canonical game-start map skipped safely: ")+error.what());
        return {};
    } catch(...) {
        log("Canonical game-start map skipped safely after an unknown failure");
        return {};
    }
}

void resetNative() {
    nativeMap.clear();
    nativeReceived = 0;
}

bool customPacket(const uint8_t* packet, size_t size) {
    return packet && size >= 10 && word(packet) == packetId;
}

void receiveCustom(const uint8_t* packet, size_t size) noexcept {
    try {
        const std::span payload(packet + 2, size - 2);
        std::lock_guard lock(stateMutex);
        switch (net::packetType(payload)) {
        case net::Type::Probe:
            sendToHost(control(net::Type::ProbeAck));
            log("Capability acknowledgement sent to host");
            break;
        case net::Type::Offer:
            receiver.offer(net::decodeOffer(payload), GetTickCount64());
            resetNative();
            loader::clearImport();
            break;
        case net::Type::Chunk: {
            if (receiver.state() != net::Receiver::State::Receiving) break;
            (void)receiver.request(GetTickCount64());
            receiver.receive(net::decodeChunk(payload), GetTickCount64());
            break;
        }
        default:
            break;
        }
    } catch (const std::exception& error) {
        log(std::string("Rejected RGBA network packet: ") + error.what());
    }
}

bool captureNative(const uint8_t* packet, size_t size,bool& started) noexcept {
    try {
        started=false;
        if (!packet || size < 2) return false;
        const auto type = word(packet);
        std::lock_guard lock(stateMutex);
        if (type == smallMapPacket) {
            if (size < 20 || dword(packet + 8) != 3 || size - 8 > maxNativeMapBytes) return false;
            nativeMap.assign(packet + 8, packet + size);
            nativeReceived = nativeMap.size();
            started=true;
            return true;
        }
        if (type != colourMapPacket || size < 16) return false;
        const auto total = dword(packet + 4);
        const auto offset = dword(packet + 8);
        const auto count = dword(packet + 12);
        if (total < 12 || total > maxNativeMapBytes || count > size - 16 || offset > total || count > total - offset)
            return false;
        if (offset == 0) {
            nativeMap.assign(total, 0);
            nativeReceived = 0;
            started=true;
        }
        if (nativeMap.size() != total) return false;
        if (offset < nativeReceived) return false;
        if (offset != nativeReceived) {
            resetNative();
            return false;
        }
        std::memcpy(nativeMap.data() + offset, packet + 16, count);
        nativeReceived += count;
        return nativeReceived == nativeMap.size();
    } catch (...) {
        return false;
    }
}

void publishNative() noexcept {
    try {
        std::lock_guard lock(stateMutex);
        if (nativeMap.size() < 12 || nativeReceived != nativeMap.size() || dword(nativeMap.data()) != 3)
            return;
        const auto encodedNative=std::span(nativeMap).subspan(4);auto payload=extractPrivatePayload(encodedNative);
        if(!payload){
            // A native-only map replaces the previous lobby map just as
            // decisively as an RGBA offer. Keeping the old artwork attached
            // made a reconnecting client render the preceding Rope Race map.
            loader::clearImport();resetNative();
            log("Native map without RGBA payload cleared the previous artwork association");
            return;
        }
        if(payload->size()<=encodedOfferSize)throw std::runtime_error("Truncated embedded RGBA payload");
        auto manifest=net::decodeOffer(std::span(*payload).first(encodedOfferSize)).manifest;
        const auto rgbaEnd=encodedOfferSize+size_t(manifest.payloadSize);
        if(rgbaEnd>payload->size())throw std::runtime_error("Truncated embedded RGBA artwork");
        std::vector<uint8_t> rgbaPng(payload->begin()+encodedOfferSize,payload->begin()+rgbaEnd);
        if(manifest.payloadSize!=rgbaPng.size() || manifest.payload!=net::sha256(rgbaPng))throw std::runtime_error("Embedded RGBA payload integrity mismatch");
        std::vector<uint8_t> synchronizedLand;
        if(rgbaEnd<payload->size()) {
            if(payload->size()-rgbaEnd<40||!std::equal(payload->begin()+rgbaEnd,payload->begin()+rgbaEnd+4,"WKLD"))
                throw std::runtime_error("Invalid synchronized LAND trailer");
            const auto landSize=dword(payload->data()+rgbaEnd+4);
            if(!landSize||landSize>maxLandBytes||payload->size()-rgbaEnd-40!=landSize)
                throw std::runtime_error("Invalid synchronized LAND size");
            synchronizedLand.assign(payload->begin()+rgbaEnd+40,payload->end());
            net::Digest expectedLand{};std::copy_n(payload->begin()+rgbaEnd+8,expectedLand.size(),expectedLand.begin());
            if(net::sha256(synchronizedLand)!=expectedLand)throw std::runtime_error("Synchronized LAND integrity mismatch");
        }
        auto image=readPngMemory(rgbaPng);auto identified=net::identify(image,manifest.threshold,manifest.generation);
        if(identified.width!=manifest.width || identified.height!=manifest.height || identified.artwork!=manifest.artwork || identified.collision!=manifest.collision)
            throw std::runtime_error("Embedded RGBA artwork identity mismatch");
        if(!matchesNativeProxyPng(image,encodedNative))throw std::runtime_error("Embedded RGBA does not match native proxy");
        std::vector<uint8_t> nativePng(nativeMap.begin()+4,nativeMap.end());
        loader::acceptImage(std::move(image),L"wkRGBA_network.png",std::move(rgbaPng),std::move(nativePng),true);
        loader::setNetworkCanonicalLand(std::move(synchronizedLand));
        resetNative();
        log("RGBA network artwork verified against native map and published");
    } catch (const std::exception& error) {
        log(std::string("Native map verification failed: ") + error.what());
        resetNative();
    }
}

void announceClientPresence(uint32_t screen) noexcept {
    if(!screen)return;const auto now=GetTickCount64();
    // Lobby screens can be reconstructed while the network connection stays
    // alive. The host resets its per-lobby capability table in that case, so
    // a one-shot acknowledgement is insufficient. Repeat the hidden marker
    // at a low rate for as long as lobby packets are being processed.
    {std::lock_guard lock(stateMutex);if(clientPresenceSentAt&&now-clientPresenceSentAt<2000)return;}
    try {
        std::vector<uint8_t> packet{0,0};constexpr std::string_view prefix="SYS::ALL:";
        packet.insert(packet.end(),prefix.begin(),prefix.end());packet.insert(packet.end(),presenceText.begin(),presenceText.end());packet.push_back(0);
        if(sendRawToHost(std::move(packet))){std::lock_guard lock(stateMutex);clientPresenceSentAt=now;log("Hidden chat capability announcement sent to host");}
    }catch(...){log("Safe chat capability announcement failed");}
}

void detectHostConfirmation(const uint8_t* packet,size_t size) noexcept {
    constexpr std::string_view confirmation="The host is using wkRGBA";
    if(!packet||size<confirmation.size())return;
    const auto* begin=reinterpret_cast<const char*>(packet);const auto* end=begin+size;
    if(std::search(begin,end,confirmation.begin(),confirmation.end())==end)return;
    std::lock_guard lock(stateMutex);
    if(!clientPresenceConfirmed)log("Host capability response confirmed for current client connection");
    clientPresenceConfirmed=true;
}

int __fastcall onClientLobby(uint32_t self, uint32_t, uint8_t* packet, size_t size) {
    ensureSessionTimer();observeClientConnection();
    if(lobbyClientScreen!=self){std::lock_guard lock(stateMutex);clientPresenceSentAt=0;clientPresenceConfirmed=false;loader::setNetworkGameplayReady(false);}
    lobbyClientScreen=self;
    if(packet&&size>=2&&word(packet)==0x1C)loader::setNetworkGameplayReady(true);
    detectHostConfirmation(packet,size);
    if (customPacket(packet, size)) { receiveCustom(packet, size); return 0; }
    bool started=false;const bool complete = captureNative(packet, size,started);
    if(started) {
        loader::clearImport();
        log("New native map generation invalidated the previous RGBA artwork before transfer");
    }
    using Handler = int (__fastcall*)(uint32_t, uint32_t, uint8_t*, size_t);
    const auto result = reinterpret_cast<Handler>(clientLobbyHook.trampoline)(self, 0, packet, size);
    announceClientPresence(self);
    if (complete) publishNative();
    return result;
}

int __fastcall onClientEnd(uint32_t self, uint32_t, uint8_t* packet, size_t size) {
    ensureSessionTimer();observeClientConnection();
    lobbyClientScreen=self;
    if(packet&&size>=2&&word(packet)==0x1C)loader::setNetworkGameplayReady(true);
    detectHostConfirmation(packet,size);
    if (customPacket(packet, size)) { receiveCustom(packet, size); return 0; }
    bool started=false;const bool complete = captureNative(packet, size,started);
    if(started)loader::clearImport();
    using Handler = int (__fastcall*)(uint32_t, uint32_t, uint8_t*, size_t);
    const auto result = reinterpret_cast<Handler>(clientEndHook.trampoline)(self, 0, packet, size);
    if (complete) publishNative();
    return result;
}

bool receiveHostControl(int slot,const uint8_t* packet,size_t size) noexcept {
    if(!customPacket(packet,size))return false;
    try {
        const auto type=net::packetType(std::span(packet+2,size-2));
        if(type==net::Type::ProbeAck && slot>=0 && slot<int(supported.size())) {
            bool announce=false;{std::lock_guard lock(stateMutex);announce=!supported[slot];supported[slot]=true;reported[slot]=true;
                knownModulePlayers.insert(nickname(slot));cancelProbeTimerLocked(slot);}
            if(announce)reportSupported(slot);
            log("Capability acknowledgement received from slot "+std::to_string(slot));
        }
    }catch(const std::exception& error){log(std::string("Rejected host control packet: ")+error.what());}
    return true;
}

bool detectPresenceChat(int slot,const uint8_t* packet,size_t size) noexcept {
    if(!packet||slot<0||slot>=int(supported.size())||size<presenceText.size())return false;
    const auto* begin=reinterpret_cast<const char*>(packet);const auto* end=begin+size;
    if(std::search(begin,end,presenceText.begin(),presenceText.end())==end)return false;
    bool announce=false;
    {std::lock_guard lock(stateMutex);announce=!supported[slot];supported[slot]=true;reported[slot]=true;
        knownModulePlayers.insert(nickname(slot));cancelProbeTimerLocked(slot);}
    if(announce)reportSupported(slot);
    if(announce)log("Safe chat capability announcement received from slot "+std::to_string(slot));
    return true;
}

int __fastcall onHostLobby(uint32_t self,uint32_t,int slot,uint8_t* packet,size_t size) {
    ensureSessionTimer();observeHostSlot(slot);
    if(receiveHostControl(slot,packet,size))return 0;
    if(detectPresenceChat(slot,packet,size))return 0;
    using Handler=int(__fastcall*)(uint32_t,uint32_t,int,uint8_t*,size_t);
    const auto result=reinterpret_cast<Handler>(hostLobbyHook.trampoline)(self,0,slot,packet,size);ensureProbe(slot);reportUnsupported(slot);return result;
}

void __fastcall onHostEnd(uint32_t self,uint32_t,int slot,uint8_t* packet,size_t size) {
    ensureSessionTimer();observeHostSlot(slot);
    if(receiveHostControl(slot,packet,size))return;
    using Handler=void(__fastcall*)(uint32_t,uint32_t,int,uint8_t*,size_t);
    reinterpret_cast<Handler>(hostEndHook.trampoline)(self,0,slot,packet,size);ensureProbe(slot);reportUnsupported(slot);
}

int __fastcall onInternalSend(uint32_t connection, uint32_t, uint8_t* packet, size_t size) {
    if(packet&&size>=2&&word(packet)==0x1C){loader::setNetworkGameplayReady(true);log("Network game-start packet opened land.dat gameplay transform gate");}
    return reinterpret_cast<InternalSend>(internalSendHook.trampoline)(connection, 0, packet, size);
}

using SendGameStart=DWORD (__stdcall*)(uint32_t);
void restoreCanonicalPin() noexcept {
    try {
        std::lock_guard lock(canonicalMutex);
        if(!hostCanonicalPin.host)return;
        const auto host=hostCanonicalPin.host;
        if(readable(reinterpret_cast<const void*>(host+0x3A514),8)&&
           *reinterpret_cast<uint8_t**>(host+0x3A518)==hostCanonicalPin.replacement->data()) {
            *reinterpret_cast<uint32_t*>(host+0x3A514)=hostCanonicalPin.originalSize;
            *reinterpret_cast<uint8_t**>(host+0x3A518)=hostCanonicalPin.original;
            log("Host canonical wire-map pin released after terrain generation");
        }
        hostCanonicalPin={};
    }catch(...){hostCanonicalPin={};}
}
DWORD __stdcall onSendGameStart(uint32_t screen) {
    // This entry point runs before W:A builds current.thm/land.dat and before
    // packet 0x1C is emitted. Opening the gate here makes borderless placement
    // deterministic instead of depending on packet-send timing.
    loader::setNetworkGameplayReady(true);
    restoreCanonicalPin();
    if(hasLegacyPeer()) {
        // A peer without wkRGBA can only simulate the native map which W:A
        // already owns.  Do not substitute a padded wire image and do not
        // repair land.dat locally: both operations previously made the host's
        // collision grid differ from the legacy client's grid.
        auto canonical=legacyMap(screen);
        const bool restoresTop=legacyMapRestoresTop(canonical);
        loader::setLegacyCompatibilityGameplay(true,restoresTop);
        loader::setNetworkCanonicalGameplay(false);
        if(canonical&&readable(reinterpret_cast<const void*>(screen+0x3A514),8)) {
            std::lock_guard lock(canonicalMutex);
            hostCanonicalPin={screen,*reinterpret_cast<uint8_t**>(screen+0x3A518),
                              *reinterpret_cast<uint32_t*>(screen+0x3A514),canonical};
            *reinterpret_cast<uint32_t*>(screen+0x3A514)=uint32_t(canonical->size());
            *reinterpret_cast<uint8_t**>(screen+0x3A518)=const_cast<uint8_t*>(canonical->data());
        }
        log(canonical
            ?std::string("Mixed lobby game start pinned to the same source-sized native simulation map sent to legacy peers")+
                (restoresTop?" with synchronized top restoration":" without local LAND rewriting")
            :"Mixed lobby repair unavailable; retained W:A's unmodified native simulation on both peers");
        return reinterpret_cast<SendGameStart>(sendGameStartHook.trampoline)(screen);
    }
    loader::setLegacyCompatibilityGameplay(false,false);
    bool activated=false;auto canonical=exactCanonicalMap(screen,activated);
    if(activated&&readable(reinterpret_cast<const void*>(screen+0x3A514),8)) {
        std::lock_guard lock(canonicalMutex);
        hostCanonicalPin={screen,*reinterpret_cast<uint8_t**>(screen+0x3A518),
                          *reinterpret_cast<uint32_t*>(screen+0x3A514),canonical};
        *reinterpret_cast<uint32_t*>(screen+0x3A514)=uint32_t(hostCanonicalPin.replacement->size());
        *reinterpret_cast<uint8_t**>(screen+0x3A518)=const_cast<uint8_t*>(hostCanonicalPin.replacement->data());
        log("Host game start pinned to canonical 112-colour wire map");
    } else log("Host game-start entry opened land.dat gameplay transform gate");
    return reinterpret_cast<SendGameStart>(sendGameStartHook.trampoline)(screen);
}

using SendColour = void (__stdcall*)(uint32_t, int);
int32_t colourTransferOffset(int slot) noexcept {
    if(slot<0||slot>=int(transferSnapshots.size())||!hostSlotAddress)return -1;
    const auto address=hostSlotAddress+0x288+uintptr_t(slot)*0x19118;
    if(!readable(reinterpret_cast<const void*>(address),sizeof(int32_t)))return -1;
    return *reinterpret_cast<const int32_t*>(address);
}
void __stdcall onSendColour(uint32_t hostThis, int slot) {
    std::shared_ptr<const std::vector<uint8_t>> augmented;uint32_t oldSize{};uint8_t* oldData{};
    try {
        // This is the actual host lobby screen object (it owns the serialized
        // map at +0x3A514).  Keep it as a reliable fallback when another
        // WormKit module has already detoured the screen constructor.
        lobbyHostScreen=hostThis;ensureSessionTimer();observeHostSlot(slot);
        ensureProbe(slot);
        const auto offset=colourTransferOffset(slot);
        if(offset==0) {
            bool rgbaPeer=false;{std::lock_guard lock(stateMutex);rgbaPeer=slot>=0&&slot<int(supported.size())&&supported[slot];}
            // A legacy peer must receive W:A's original serialized map.  The
            // former padded replacement could not be reproduced by the
            // host's already-decoded editor state and caused desync.
            auto replacement=rgbaPeer?augmentedMap(hostThis):legacyMap(hostThis);
            std::shared_ptr<const CanonicalActivation> activation;
            if(rgbaPeer&&replacement) {
                std::lock_guard lock(augmentedMutex);
                if(augmentedCache.bytes==replacement)activation=augmentedCache.activation;
            }
            const auto size=*reinterpret_cast<uint32_t*>(hostThis+0x3A514);
            const auto data=*reinterpret_cast<uint8_t**>(hostThis+0x3A518);
            if(slot>=0&&slot<int(transferSnapshots.size())) {
                std::lock_guard lock(stateMutex);
                transferSnapshots[slot]={hostThis,data,size,std::move(replacement),std::move(activation),true};
                augmented=transferSnapshots[slot].replacement;
            }
            if(!rgbaPeer)reportUnsupported(slot);
            log(std::string("Colour-map transfer snapshot frozen for slot ")+std::to_string(slot)+
                (rgbaPeer?" (RGBA)":" (112-colour compatibility)"));
        } else if(offset>0 && slot>=0&&slot<int(transferSnapshots.size())) {
            std::lock_guard lock(stateMutex);const auto& snapshot=transferSnapshots[slot];
            if(snapshot.active&&snapshot.host==hostThis)augmented=snapshot.replacement;
        }
        if(augmented && !augmented->empty()) {
            oldSize=*reinterpret_cast<uint32_t*>(hostThis+0x3A514);oldData=*reinterpret_cast<uint8_t**>(hostThis+0x3A518);
            *reinterpret_cast<uint32_t*>(hostThis+0x3A514)=uint32_t(augmented->size());*reinterpret_cast<uint8_t**>(hostThis+0x3A518)=const_cast<uint8_t*>(augmented->data());
        }
    }catch(const std::exception& error){log(std::string("RGBA map embedding skipped: ")+error.what());}
    reinterpret_cast<SendColour>(sendColourHook.trampoline)(hostThis,slot);
    if(oldData){*reinterpret_cast<uint32_t*>(hostThis+0x3A514)=oldSize;*reinterpret_cast<uint8_t**>(hostThis+0x3A518)=oldData;}
    // Keep the completed snapshot until a new map transfer or disconnect.
    // Game start must activate these exact bytes rather than a later editor
    // rebuild with a different LAND location table.
}

using ConstructHost=int(__stdcall*)(int,int);
ConstructHost constructHostCallOriginal{};
int __stdcall onConstructHost(int screen,int argument){
    restoreCanonicalPin();
    auto original=constructHostHook.trampoline?reinterpret_cast<ConstructHost>(constructHostHook.trampoline):constructHostCallOriginal;
    auto result=original(screen,argument);lobbyHostScreen=uint32_t(screen);
    ensureSessionTimer();
    {
        std::lock_guard lock(stateMutex);
        for(int slot=0;slot<int(supported.size());++slot) {
            const auto peer=hostSlotAddress+0x1A4+uintptr_t(slot)*0x19118;
            const bool connected=connectionState(peer);
            // Returning from a match rebuilds the lobby screen without
            // disconnecting peers. Preserve their negotiated capability;
            // reset only slots whose underlying connection really ended.
            if(!connected)resetHostSlotLocked(slot);
            hostConnected[slot]=connected;
        }
    }
    loader::setNetworkCanonicalGameplay(false);loader::setNetworkGameplayReady(false);
    loader::setLegacyCompatibilityGameplay(false,false);
    return result;
}

bool installConstructHostCallFallback(uint8_t* base) noexcept {
    auto* call=base+0xE91A0;if(call[0]!=0xE8)return false;
    int32_t displacement{};std::memcpy(&displacement,call+1,4);
    constructHostCallOriginal=reinterpret_cast<ConstructHost>(call+5+displacement);
    std::copy_n(call,5,constructHostCallOriginalBytes.begin());
    DWORD old{};if(!VirtualProtect(call,5,PAGE_EXECUTE_READWRITE,&old))return false;
    call[0]=0xE8;const auto replacement=int32_t(uintptr_t(&onConstructHost)-uintptr_t(call)-5);
    std::memcpy(call+1,&replacement,4);DWORD ignored{};VirtualProtect(call,5,old,&ignored);
    FlushInstructionCache(GetCurrentProcess(),call,5);constructHostCallSite=call;return true;
}

int __stdcall onConstructClient(int screen,int argument){
    auto result=reinterpret_cast<ConstructHost>(constructClientHook.trampoline)(screen,argument);
    lobbyClientScreen=uint32_t(screen);ensureSessionTimer();
    {std::lock_guard lock(stateMutex);clientPresenceSentAt=0;clientPresenceConfirmed=false;clientConnected=false;}
    loader::setNetworkCanonicalGameplay(false);loader::setNetworkGameplayReady(false);
    loader::setLegacyCompatibilityGameplay(false,false);return result;
}

void rollback() noexcept {
    restoreCanonicalPin();
    {std::lock_guard lock(stateMutex);for(int i=0;i<int(probeTimers.size());++i)cancelProbeTimerLocked(i);transferSnapshots.fill({});
     if(sessionTimer){KillTimer(nullptr,sessionTimer);sessionTimer=0;}}
    constructClientHook.remove();constructHostHook.remove();hostEndHook.remove();hostLobbyHook.remove();
    if(constructHostCallSite){DWORD old{};if(VirtualProtect(constructHostCallSite,5,PAGE_EXECUTE_READWRITE,&old)){
        std::copy(constructHostCallOriginalBytes.begin(),constructHostCallOriginalBytes.end(),constructHostCallSite);
        DWORD ignored{};VirtualProtect(constructHostCallSite,5,old,&ignored);FlushInstructionCache(GetCurrentProcess(),constructHostCallSite,5);}
        constructHostCallSite=nullptr;constructHostCallOriginal=nullptr;}
    sendColourHook.remove();
    sendGameStartHook.remove();
    internalSendHook.remove();
    clientEndHook.remove();
    clientLobbyHook.remove();
}
}

void canonicalTerrainConsumed() noexcept { restoreCanonicalPin(); }

bool installHooks(HMODULE host, const std::filesystem::path& directory) noexcept {
    try {
        logPath = directory / L"wkRGBA_network.log";
        auto* base = reinterpret_cast<uint8_t*>(host);
        hostSlotAddress=*reinterpret_cast<uint32_t*>(base+0xAC0D0+0xC6);
        clientSlotAddress=*reinterpret_cast<uint32_t*>(base+0xBD400+0x103);
        struct Result {const char* name;InlineHook* hook;bool installed;};
        Result results[]{
            {"host lobby",&hostLobbyHook,hostLobbyHook.install(base+0xB6290,{0x83,0xEC,0x10,0x8B,0x44,0x24,0x1C},reinterpret_cast<void*>(&onHostLobby))},
            {"host endscreen",&hostEndHook,hostEndHook.install(base+0xAC0D0,{0x6A,0xFF,0x68,0xF0,0x79,0x61,0x00},reinterpret_cast<void*>(&onHostEnd))},
            {"host lobby screen",&constructHostHook,constructHostHook.install(base+0xB0160,{0x6A,0xFF,0x68,0x93,0x7C,0x61,0x00},reinterpret_cast<void*>(&onConstructHost))},
            {"client lobby screen",&constructClientHook,constructClientHook.install(base+0xBDBE0,{0x6A,0xFF,0x68,0x65,0x77,0x61,0x00},reinterpret_cast<void*>(&onConstructClient))},
            {"client lobby",&clientLobbyHook,clientLobbyHook.install(base+0xC0790,{0x55,0x8B,0xEC,0x83,0xE4,0xF8},reinterpret_cast<void*>(&onClientLobby))},
            {"client endscreen",&clientEndHook,clientEndHook.install(base+0xBD400,{0x55,0x8B,0xEC,0x83,0xE4,0xF8},reinterpret_cast<void*>(&onClientEnd))},
            {"colour map sender",&sendColourHook,sendColourHook.install(base+0xABCC0,{0x55,0x8B,0x6C,0x24,0x0C},reinterpret_cast<void*>(&onSendColour))},
            {"internal packet sender",&internalSendHook,internalSendHook.install(base+0x18FCA0,{0x53,0x8B,0x5C,0x24,0x0C},reinterpret_cast<void*>(&onInternalSend))},
            {"host game start",&sendGameStartHook,sendGameStartHook.install(base+0xBA370,{0x55,0x8B,0xEC,0x83,0xE4,0xF8},reinterpret_cast<void*>(&onSendGameStart))}
        };
        size_t count{};for(const auto& result:results) {
            if(result.installed){++count;log(std::string(result.name)+" hook installed"+(result.hook->chained?" (chained)":""));}
            else log(std::string(result.name)+" hook unavailable");
        }
        if(!constructHostHook.target && installConstructHostCallFallback(base)){++count;log("host lobby screen call-site fallback installed");}
        const bool useful=clientLobbyHook.target || clientEndHook.target || sendColourHook.target;
        log("RGBA network hook installation completed: "+std::to_string(count)+"/9 entry points active");
        return useful;
    } catch (...) {
        rollback();
        return false;
    }
}
}
