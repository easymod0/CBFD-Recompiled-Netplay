// Signalling's packets: what a peer and the signalling server (signalling.hpp) send each other, so that
// GameNetworkingSockets' ICE can find a way between two peers (through NAT, or a TURN relay). The server
// only passes signals on, between the ids its peers registered; it never sees the game's packets, which
// go over the connection ICE makes. This is the only code that reads what arrives from the server or a
// peer of it: every field is checked, and anything else is refused whole.
//
// The server also keeps rooms: a host creates one (with its match's key) and is given a room code
// (join_code.hpp) to pass on; a joiner looks the code up and is given the host's id and the key, to connect
// to it through ICE. A room lasts as long as its host stays connected to the server (the whole match, so
// that someone could rejoin it later). And the server may offer each peer its TURN relay, with credentials
// of its own that expire (Turn).
//
// The layout, little-endian: "CS", u16 version, u8 kind, then the kind's fields (see signal_packets.cpp).
// New kinds (a lobby's) come with a new version.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace conker::netplay::signal {
    // Changes whenever any packet's layout or meaning does: a peer with another one is refused.
    constexpr uint16_t version = 2;
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
    // Server to peer: its Register is taken, under `id`; only then can others reach it.
    struct Registered {
        uint64_t id = 0;
        bool operator==(const Registered&) const = default;
    };
    // Server to peer, before closing its connection: why (the id is taken, a broken packet, too many; or
    // too many rooms looked up that weren't there).
    enum class Refusal : uint8_t { version = 1, taken, protocol, busy, last = busy };
    struct Refused {
        Refusal reason = Refusal::protocol;
        bool operator==(const Refused&) const = default;
    };

    // Peer to server, once registered: makes a room for this peer's match, with its key (not 0), until it
    // leaves. One each.
    struct CreateRoom {
        uint64_t key = 0;
        bool operator==(const CreateRoom&) const = default;
    };
    // Server to peer: its room, 1 to max_room (join_code.hpp: the room code).
    struct RoomCreated {
        uint64_t room = 0;
        bool operator==(const RoomCreated&) const = default;
    };
    // Peer to server, once registered: who hosts `room` (1 to max_room)?
    struct FindRoom {
        uint64_t room = 0;
        bool operator==(const FindRoom&) const = default;
    };
    // Server to peer: `room`'s host's id, and the key its match was created with (neither 0).
    struct RoomFound {
        uint64_t room = 0;
        uint64_t host = 0;
        uint64_t key = 0;
        bool operator==(const RoomFound&) const = default;
    };
    // Server to peer: there's no `room` (gone, or never there).
    struct NoRoom {
        uint64_t room = 0;
        bool operator==(const NoRoom&) const = default;
    };
    // Server to peer, before Registered, if it has a TURN relay: where (as Ice::turn: "host:port" separated by
    // commas), and the user and password this peer may use it with, for a while. Each is 1 to max_text
    // printable characters (no spaces), as the library takes them.
    constexpr size_t max_text = 128;
    struct Turn {
        std::string servers;
        std::string user;
        std::string password;
        bool operator==(const Turn&) const = default;
    };

    using Packet = std::variant<Register, Signal, Unreachable, Refused, Registered, CreateRoom, RoomCreated, FindRoom,
        RoomFound, NoRoom, Turn>;

    // The packet's bytes into `out` (max_packet_size): their size, or 0 for a packet that isn't valid.
    size_t encode(const Packet& packet, std::span<uint8_t, max_packet_size> out);
    // The packet in `data`, or nothing if it isn't exactly one valid packet of this version.
    std::optional<Packet> decode(std::span<const uint8_t> data);
    // Whether `data` starts like a signalling packet of another version (to say so, not "broken").
    bool other_version(std::span<const uint8_t> data);
}
