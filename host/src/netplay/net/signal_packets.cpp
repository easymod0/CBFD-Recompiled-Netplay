// Signalling's packets (signal_packets.hpp): encoding, and decoding with every field checked. This is the
// only code that parses what arrives from the signalling server or its peers; fuzz_packets.cpp fuzzes it.
//
// Each packet: "CS", u16 version, u8 kind, then (all little-endian):
//   1 Register     u64 id (not 0)                                                  13 bytes
//   2 Signal       u64 peer (not 0), u16 size (1 to max_signal), size bytes     15 + size bytes
//   3 Unreachable  u64 peer (not 0)                                                13 bytes
//   4 Refused      u8 reason (1 to Refusal::last)                                   6 bytes
// A packet of any other size, version or kind, or with any field out of range, is refused.

#include "signal_packets.hpp"

#include <algorithm>

namespace conker::netplay::signal {
    namespace {
        enum Kind : uint8_t { register_ = 1, signal, unreachable, refused };
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
    }

    size_t encode(const Packet& packet, std::span<uint8_t, max_packet_size> out) {
        Writer w{ out.data() };
        w.u8('C');
        w.u8('S');
        w.u16(version);
        if (const Register* p = std::get_if<Register>(&packet)) {
            if (p->id == 0) return 0;
            w.u8(register_);
            w.u64(p->id);
        }
        else if (const Signal* p = std::get_if<Signal>(&packet)) {
            if (p->peer == 0 || p->data.empty() || p->data.size() > max_signal) return 0;
            w.u8(signal);
            w.u64(p->peer);
            w.u16((uint16_t)p->data.size());
            w.p = std::copy(p->data.begin(), p->data.end(), w.p);
        }
        else if (const Unreachable* p = std::get_if<Unreachable>(&packet)) {
            if (p->peer == 0) return 0;
            w.u8(unreachable);
            w.u64(p->peer);
        }
        else {
            const Refused& r = std::get<Refused>(packet);
            if ((uint8_t)r.reason < 1 || r.reason > Refusal::last) return 0;
            w.u8(refused);
            w.u8((uint8_t)r.reason);
        }
        return (size_t)(w.p - out.data());
    }

    std::optional<Packet> decode(std::span<const uint8_t> data) {
        if (data.size() < header_size || data[0] != 'C' || data[1] != 'S' ||
            (uint16_t)(data[2] | (data[3] << 8)) != version) {
            return std::nullopt;
        }
        Reader r{ data.data() + header_size };
        switch (data[4]) {
            case register_: {
                if (data.size() != header_size + 8) return std::nullopt;
                Register p{ r.u64() };
                if (p.id == 0) return std::nullopt;
                return p;
            }
            case signal: {
                if (data.size() < header_size + 10) return std::nullopt;
                Signal p;
                p.peer = r.u64();
                const uint16_t size = r.u16();
                if (p.peer == 0 || size == 0 || size > max_signal || data.size() != header_size + 10 + size) return std::nullopt;
                p.data.assign(r.p, r.p + size);
                return p;
            }
            case unreachable: {
                if (data.size() != header_size + 8) return std::nullopt;
                Unreachable p{ r.u64() };
                if (p.peer == 0) return std::nullopt;
                return p;
            }
            case refused: {
                if (data.size() != header_size + 1) return std::nullopt;
                const uint8_t reason = r.u8();
                if (reason < 1 || reason > (uint8_t)Refusal::last) return std::nullopt;
                return Refused{ (Refusal)reason };
            }
            default:
                return std::nullopt;
        }
    }

    bool other_version(std::span<const uint8_t> data) {
        return data.size() >= 4 && data[0] == 'C' && data[1] == 'S' && (uint16_t)(data[2] | (data[3] << 8)) != version;
    }
}
