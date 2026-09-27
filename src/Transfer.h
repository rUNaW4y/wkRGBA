#pragma once
#include "Terrain.h"
#include <array>
#include <memory>
#include <optional>
#include <span>

namespace wkrgba::net {
using Digest = std::array<uint8_t,32>;
using Bytes = std::vector<uint8_t>;
constexpr uint32_t chunkSize = 4096;
constexpr uint32_t maxTransferBytes = 768u * 1024u * 1024u;
constexpr uint32_t protocolVersion = 3;
Digest sha256(std::span<const uint8_t> bytes);
// Identity covers dimensions, exact RGBA pixels and the alpha collision rule.
// All machines must agree on both artwork and initial physics.
struct Manifest {
    uint64_t generation{};
    uint32_t width{}, height{};
    uint8_t threshold{};
    Digest artwork{}, collision{};
    uint32_t payloadSize{};
    Digest payload{};
    bool operator==(const Manifest&) const = default;
};
struct Offer { Manifest manifest; };
struct Request { uint64_t generation{}; uint32_t offset{}; Digest artwork{}; };
struct Chunk { uint64_t generation{}; uint32_t offset{}; Digest artwork{}; Bytes data; };
struct Ready { Manifest manifest; };
Manifest identify(const Image& image, uint8_t threshold, uint64_t generation);
Bytes encode(const Offer&);
Bytes encode(const Request&);
Bytes encode(const Chunk&);
Bytes encode(const Ready&);
// Envelope independent of W:A packet IDs. Native adapter must frame it only
// after capability negotiation; never reinterpret unrelated mod packets.
enum class Type : uint8_t { Offer=1, Request=2, Chunk=3, Ready=4, Probe=5, ProbeAck=6 };
Type packetType(std::span<const uint8_t> packet);
Offer decodeOffer(std::span<const uint8_t>);
Request decodeRequest(std::span<const uint8_t>);
Chunk decodeChunk(std::span<const uint8_t>);
Ready decodeReady(std::span<const uint8_t>);

class Sender {
public:
    Sender(Image image, uint8_t threshold, uint64_t generation);
    Sender(std::shared_ptr<const Image> image, uint8_t threshold, uint64_t generation);
    Sender(std::shared_ptr<const Image> image, std::shared_ptr<const Bytes> encodedPng,
           uint8_t threshold, uint64_t generation);
    Sender(Manifest manifest,std::shared_ptr<const Bytes> encodedPng);
    const Manifest& manifest() const { return manifest_; }
    Chunk serve(const Request&) const;
    bool accepts(const Ready&) const;
private:
    Manifest manifest_;
    std::shared_ptr<const Image> image_;
    std::shared_ptr<const Bytes> encoded_;
};
// One receiver per peer/session, no global cache or peer-shared assembly.
// Host generation must be strictly increasing within one session.
class Receiver {
public:
    enum class State { Idle, Receiving, AwaitingMap, Ready, Failed };
    void offer(const Offer&, uint64_t nowMs);
    std::optional<Request> request(uint64_t nowMs);
    void receive(const Chunk&, uint64_t nowMs);
    void bindNativeMap(uint32_t width, uint32_t height, std::span<const uint8_t> mask);
    void bindNativeProxy(std::span<const uint8_t> encodedPng);
    std::optional<Ready> acknowledgement() const;
    const Image& image() const;
    Image takeImage();
    Bytes takeEncoded();
    State state() const { return state_; }
    void reset();
private:
    void fail(const char* message);
    State state_ = State::Idle;
    Manifest manifest_{};
    uint64_t highestGeneration_{}, deadline_{}, retryAt_{}, hardDeadline_{};
    uint32_t offset_{};
    Image image_{};
    Bytes encoded_{};
    // Allocation is delayed until the first valid requested chunk arrives.
    bool pending_ = false;
};
}
