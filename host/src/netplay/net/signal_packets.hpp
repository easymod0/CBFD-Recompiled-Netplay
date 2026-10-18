// Signalling's packets: what a peer and the signalling server (signalling.hpp) send each other, so that
// GameNetworkingSockets' ICE can find a way between two peers (through NAT, or a TURN relay). The server
// only passes signals on, between the ids its peers registered; it never sees the game's packets, which
// go over the connection ICE makes. This is the only code that reads what arrives from the server or a
// peer of it: every field is checked, and anything else is refused whole.
//
// The layout, little-endian: "CS", u16 version, u8 kind, then the kind's fields (see signal_packets.cpp).
// New kinds (rooms, a lobby) come with a new version.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace conker::netplay::signal {
    // Changes whenever any packet's layout or meaning does: a peer with another one is refused.
    constexpr uint16_t version = 1;
    // The largest signal GameNetworkingSockets sends (4200 bytes of message data at most, plus its
    // own), with room to spare.
    constexpr size_t max_signal = 8192;
    // The largest packet, to size buffers and the library's message limit.
    constexpr size_t max_packet_size = 5 + 10 + max_signal;

    // Peer to server, first and once: the id it's reached by (never 0). The library's identity for this
    // process is made from it (session.hpp: local_id()).
    struct Register {
        uint64_t id = 0;
        bool operator==(const Register&) const = default;
    };
    // Peer to server: to pass on to `peer`; server to peer: from `peer`. `data` is the library's own
    // (1 to max_signal bytes), handed to it as it came.
    struct Signal {
        uint64_t peer = 0;
        std::vector<uint8_t> data;
        bool operator==(const Signal&) const = default;
    };
    // Server to peer: nobody has registered `peer` (gone, or never there).
    struct Unreachable {
        uint64_t peer = 0;
        bool operator==(const Unreachable&) const = default;
    };
    // Server to peer, before closing its connection: why (the id is taken, a broken packet, too many).
    enum class Refusal : uint8_t { version = 1, taken, protocol, busy, last = busy };
    struct Refused {
        Refusal reason = Refusal::protocol;
        bool operator==(const Refused&) const = default;
    };

    using Packet = std::variant<Register, Signal, Unreachable, Refused>;

    // The packet's bytes into `out` (max_packet_size): their size, or 0 for a packet that isn't valid.
    size_t encode(const Packet& packet, std::span<uint8_t, max_packet_size> out);
    // The packet in `data`, or nothing if it isn't exactly one valid packet of this version.
    std::optional<Packet> decode(std::span<const uint8_t> data);
    // Whether `data` starts like a signalling packet of another version (to say so, not "broken").
    bool other_version(std::span<const uint8_t> data);
}
