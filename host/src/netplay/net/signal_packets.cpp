// Signalling's packets (signal_packets.hpp): encoding, and decoding with every field checked. This is the
// only code that parses what arrives from the signalling server or its peers; fuzz_packets.cpp fuzzes it.
//
// Each packet: "CS", u16 version, u8 kind, then (all little-endian):
//   1 Register     u64 id (not 0)                                                  13 bytes
//   2 Signal       u64 peer (not 0), u16 size (1 to max_signal), size bytes     15 + size bytes
//   3 Unreachable  u64 peer (not 0)                                                13 bytes
//   4 Refused      u8 reason (1 to Refusal::last)                                   6 bytes
//   5 Registered   u64 id (not 0)                                                  13 bytes
//   6 CreateRoom   u64 key (not 0)                                                 13 bytes
//   7 RoomCreated  u64 room (1 to max_room)                                        13 bytes
//   8 FindRoom     u64 room (1 to max_room)                                        13 bytes
//   9 RoomFound    u64 room (1 to max_room), u64 host, u64 key (not 0)             29 bytes
//  10 NoRoom       u64 room (1 to max_room)                                        13 bytes
//  11 Turn         3 times: u8 size (1 to max_text), size characters (! to ~)  8 + sizes bytes
// A packet of any other size, version or kind, or with any field out of range, is refused.

#include "signal_packets.hpp"
#include "join_code.hpp"

#include <algorithm>

namespace conker::netplay::signal {
    namespace {
        enum Kind : uint8_t { register_ = 1, signal, unreachable, refused, registered, create_room, room_created, find_room,
            room_found, no_room, turn };
        constexpr size_t header_size = 5;

        struct Writer {
            uint8_t* p;
            void u8(uint8_t v) { *p++ = v; }
            void u16(uint16_t v) { u8((uint8_t)v); u8((uint8_t)(v >> 8)); }
            void u64(uint64_t v) { for (int i = 0; i < 8; i++) u8((uint8_t)(v >> (8 * i))); }
        };
        struct Reader {
            const uint8_t* p;
            uint8_t u8() { return *p++; }
            uint16_t u16() { const uint16_t v = (uint16_t)(p[0] | (p[1] << 8)); p += 2; return v; }
            uint64_t u64() { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); p += 8; return v; }
        };

        bool valid_id(uint64_t id) { return id != 0; }
        bool valid_room(uint64_t room) { return room != 0 && room <= max_room; }
        bool valid_text(const std::string& text) {
            return !text.empty() && text.size() <= max_text &&
                std::all_of(text.begin(), text.end(), [](char c) { return c > ' ' && c <= '~'; });
        }
    }

    size_t encode(const Packet& packet, std::span<uint8_t, max_packet_size> out) {
        Writer w{ out.data() };
        w.u8('C');
        w.u8('S');
        w.u16(version);
        // A kind of one u64, checked.
        const auto one = [&w](Kind kind, uint64_t value, bool valid) {
            if (!valid) return false;
            w.u8(kind);
            w.u64(value);
            return true;
        };
        bool ok = true;
        if (const Register* p = std::get_if<Register>(&packet)) ok = one(register_, p->id, valid_id(p->id));
        else if (const Unreachable* p = std::get_if<Unreachable>(&packet)) ok = one(unreachable, p->peer, valid_id(p->peer));
        else if (const Registered* p = std::get_if<Registered>(&packet)) ok = one(registered, p->id, valid_id(p->id));
        else if (const CreateRoom* p = std::get_if<CreateRoom>(&packet)) ok = one(create_room, p->key, valid_id(p->key));
        else if (const RoomCreated* p = std::get_if<RoomCreated>(&packet)) ok = one(room_created, p->room, valid_room(p->room));
        else if (const FindRoom* p = std::get_if<FindRoom>(&packet)) ok = one(find_room, p->room, valid_room(p->room));
        else if (const NoRoom* p = std::get_if<NoRoom>(&packet)) ok = one(no_room, p->room, valid_room(p->room));
        else if (const Signal* p = std::get_if<Signal>(&packet)) {
            if (p->peer == 0 || p->data.empty() || p->data.size() > max_signal) return 0;
            w.u8(signal);
            w.u64(p->peer);
            w.u16((uint16_t)p->data.size());
            w.p = std::copy(p->data.begin(), p->data.end(), w.p);
        }
        else if (const RoomFound* p = std::get_if<RoomFound>(&packet)) {
            if (!valid_room(p->room) || p->host == 0 || p->key == 0) return 0;
            w.u8(room_found);
            w.u64(p->room);
            w.u64(p->host);
            w.u64(p->key);
        }
        else if (const Turn* p = std::get_if<Turn>(&packet)) {
            if (!valid_text(p->servers) || !valid_text(p->user) || !valid_text(p->password)) return 0;
            w.u8(turn);
            for (const std::string* text : { &p->servers, &p->user, &p->password }) {
                w.u8((uint8_t)text->size());
                w.p = std::copy(text->begin(), text->end(), w.p);
            }
        }
        else {
            const Refused& r = std::get<Refused>(packet);
            if ((uint8_t)r.reason < 1 || r.reason > Refusal::last) return 0;
            w.u8(refused);
            w.u8((uint8_t)r.reason);
        }
        return ok ? (size_t)(w.p - out.data()) : 0;
    }

    std::optional<Packet> decode(std::span<const uint8_t> data) {
        if (data.size() < header_size || data[0] != 'C' || data[1] != 'S' ||
            (uint16_t)(data[2] | (data[3] << 8)) != version) {
            return std::nullopt;
        }
        Reader r{ data.data() + header_size };
        const size_t size = data.size() - header_size;
        // A kind of one u64: the value, if it's the only field and valid.
        const auto one = [&r, size](bool (*valid)(uint64_t)) -> std::optional<uint64_t> {
            if (size != 8) return std::nullopt;
            const uint64_t value = r.u64();
            if (!valid(value)) return std::nullopt;
            return value;
        };
        std::optional<uint64_t> value;
        switch (data[4]) {
            case register_: if ((value = one(valid_id))) return Register{ *value }; return std::nullopt;
            case unreachable: if ((value = one(valid_id))) return Unreachable{ *value }; return std::nullopt;
            case registered: if ((value = one(valid_id))) return Registered{ *value }; return std::nullopt;
            case create_room: if ((value = one(valid_id))) return CreateRoom{ *value }; return std::nullopt;
            case room_created: if ((value = one(valid_room))) return RoomCreated{ *value }; return std::nullopt;
            case find_room: if ((value = one(valid_room))) return FindRoom{ *value }; return std::nullopt;
            case no_room: if ((value = one(valid_room))) return NoRoom{ *value }; return std::nullopt;
            case signal: {
                if (size < 10) return std::nullopt;
                Signal p;
                p.peer = r.u64();
                const uint16_t length = r.u16();
                if (p.peer == 0 || length == 0 || length > max_signal || size != 10u + length) return std::nullopt;
                p.data.assign(r.p, r.p + length);
                return p;
            }
            case refused: {
                if (size != 1) return std::nullopt;
                const uint8_t reason = r.u8();
                if (reason < 1 || reason > (uint8_t)Refusal::last) return std::nullopt;
                return Refused{ (Refusal)reason };
            }
            case room_found: {
                if (size != 24) return std::nullopt;
                RoomFound p;
                p.room = r.u64();
                p.host = r.u64();
                p.key = r.u64();
                if (!valid_room(p.room) || p.host == 0 || p.key == 0) return std::nullopt;
                return p;
            }
            case turn: {
                Turn p;
                const uint8_t* end = data.data() + data.size();
                for (std::string* text : { &p.servers, &p.user, &p.password }) {
                    if (r.p >= end) return std::nullopt;
                    const uint8_t length = r.u8();
                    if (length > end - r.p) return std::nullopt;
                    text->assign((const char*)r.p, length);
                    r.p += length;
                    if (!valid_text(*text)) return std::nullopt;
                }
                if (r.p != end) return std::nullopt;
                return p;
            }
            default:
                return std::nullopt;
        }
    }

    bool other_version(std::span<const uint8_t> data) {
        return data.size() >= 4 && data[0] == 'C' && data[1] == 'S' && (uint16_t)(data[2] | (data[3] << 8)) != version;
    }
}
