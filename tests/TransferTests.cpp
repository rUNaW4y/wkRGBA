#include "Transfer.h"
#include <iostream>
#include <stdexcept>
#include <string>
using namespace wkrgba;
using namespace wkrgba::net;
void check(bool p,const char* why){if(!p)throw std::runtime_error(why);}
template<class F>void rejects(F f){bool ok=false;try{f();}catch(const std::exception&){ok=true;}check(ok,"Expected rejection");}
Image fixture(){Image im{257,19,{}};for(unsigned i=0;i<257*19;++i)im.pixels.push_back({uint8_t(i),uint8_t(i>>8),uint8_t(i*13),uint8_t(i)});return im;}
void deliver(Sender& sender,Receiver& receiver,uint64_t& now){
    while(receiver.state()==Receiver::State::Receiving){
        auto request=receiver.request(now++);check(bool(request),"Next request");
        auto wireRequest=encode(*request);
        auto reply=sender.serve(decodeRequest(wireRequest));
        auto wireReply=encode(reply);
        receiver.receive(decodeChunk(wireReply),now++);
    }
}
int main(){try{
    const Bytes abc{'a','b','c'};
    const Digest known{0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                       0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    check(sha256(abc)==known,"SHA256 standard vector");
    auto original=fixture();Sender host(original,128,1);Receiver alice,bob;uint64_t now=1;
    const auto offer=encode(Offer{host.manifest()});
    alice.offer(decodeOffer(offer),now);bob.offer(decodeOffer(offer),now);
    deliver(host,alice,now);deliver(host,bob,now);
    check(!alice.acknowledgement(),"No readiness until native map is checked");
    rejects([&]{alice.image();});
    Terrain terrain(original);
    alice.bindNativeMap(original.width,original.height,terrain.mask());
    bob.bindNativeMap(original.width,original.height,terrain.mask());
    check(alice.image().pixels==original.pixels && bob.image().pixels==original.pixels,"All clients receive exact RGBA");
    check(host.accepts(decodeReady(encode(*alice.acknowledgement()))),"Host validates complete manifest in ACK");
    auto oldAck=*alice.acknowledgement();
    Sender newHost(original,128,2);
    check(!newHost.accepts(oldAck),"Previous map acknowledgement must not unlock new map");
    alice.offer({newHost.manifest()},now++);
    check(!alice.acknowledgement(),"Map change clears readiness");
    alice.offer({host.manifest()},now++);
    auto request=alice.request(now++);check(request->generation==2,"Old offer cannot roll back session");
    auto stale=host.serve({1,0,host.manifest().artwork});
    alice.receive(stale,now++);check(!alice.request(now++),"Old chunk does not disturb current request");
    now+=1000;
    request=alice.request(now++);check(bool(request)&&request->offset==0,"Retry after dropped packet");
    auto first=newHost.serve(*request);alice.receive(first,now++);
    alice.receive(first,now++); // duplicate after receipt must be harmless
    deliver(newHost,alice,now);
    auto wrong=terrain.mask();wrong[0]=!wrong[0];
    rejects([&]{alice.bindNativeMap(original.width,original.height,wrong);});
    check(alice.state()==Receiver::State::Failed,"Wrong native map disables artwork");
    alice.reset();alice.offer({host.manifest()},now);
    rejects([&]{alice.request(now+120001);});
    check(alice.state()==Receiver::State::Failed,"Timeout disables incomplete map");
    alice.reset();alice.offer({host.manifest()},now++);
    request=alice.request(now++);auto damaged=host.serve(*request);damaged.data[0]^=1;
    rejects([&]{alice.receive(damaged,now++);deliver(host,alice,now);});
    check(alice.state()==Receiver::State::Failed,"Corrupt artwork rejected by SHA256");
    alice.reset();alice.offer({host.manifest()},now++);request=alice.request(now++);
    auto badOffset=host.serve(*request);badOffset.offset=chunkSize;
    rejects([&]{alice.receive(badOffset,now++);});
    auto changed=host.manifest();changed.threshold=129;
    bob.offer({host.manifest()},now++);rejects([&]{bob.offer({changed},now++);});
    rejects([&]{host.serve({2,0,host.manifest().artwork});});
    rejects([&]{host.serve({1,1,host.manifest().artwork});});
    for(size_t i=0;i<offer.size();++i)rejects([&]{decodeOffer(std::span(offer).first(i));});
    auto bad=offer;bad[4]=255;rejects([&]{decodeOffer(bad);});
    bad=offer;bad.push_back(0);rejects([&]{decodeOffer(bad);});
    bad=offer;bad[6]=1;rejects([&]{decodeOffer(bad);});
    auto manifest=host.manifest();manifest.width=UINT32_MAX;rejects([&]{encode(Offer{manifest});});
    check(identify(original,127,1).artwork!=host.manifest().artwork,"Alpha rule is part of map identity");
    std::cout<<"Transfer: two peers, exact RGBA, native collision binding, ACK identity, map changes, stale packets, retry, timeout, corruption and malformed input passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
