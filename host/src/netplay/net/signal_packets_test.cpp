// Tests of signalling's packets (signal_packets.cpp): each kind round-trips, its size and layout are as
// documented, and every out-of-range field, size, version and kind is refused. Exits 1 on a failure.

#include <cstdio>
#include <vector>

#include "signal_packets.hpp"
#include "join_code.hpp"

using namespace conker::netplay::signal;
using conker::netplay::max_room;

namespace {
    int failures = 0;

    void check(bool ok, const char* what, int line) {
        if (!ok) {
            std::printf("signal_packets_test.cpp:%d: FAILED: %s\n", line, what);
            failures++;
        }
    }
#define CHECK(x) check((x), #x, __LINE__)

    std::vector<uint8_t> bytes_of(const Packet& packet) {
        uint8_t buffer[max_packet_size];
        const size_t size = encode(packet, buffer);
        return std::vector<uint8_t>(buffer, buffer + size);
    }

    bool decodes(const std::vector<uint8_t>& data) {
        return decode(data).has_value();
    }

    bool round_trips(const Packet& packet, size_t size) {
        const std::vector<uint8_t> data = bytes_of(packet);
        const std::optional<Packet> back = decode(data);
        return data.size() == size && back && *back == packet;
    }
}

int main() {
    // Each kind, at its documented size.
    CHECK(round_trips(Register{ 0x0123456789ABCDEFull }, 13));
    CHECK(round_trips(Signal{ 7, { 1, 2, 3 } }, 18));
    CHECK(round_trips(Signal{ 7, std::vector<uint8_t>(max_signal, 0xAB) }, 15 + max_signal));
    CHECK(round_trips(Unreachable{ 9 }, 13));
    CHECK(round_trips(Registered{ 9 }, 13));
    CHECK(round_trips(Refused{ Refusal::taken }, 6));
    CHECK(round_trips(Refused{ Refusal::last }, 6));
    CHECK(round_trips(CreateRoom{ 0xFEDCBA9876543210ull }, 13));
    CHECK(round_trips(RoomCreated{ max_room }, 13));
    CHECK(round_trips(FindRoom{ 1 }, 13));
    CHECK(round_trips(RoomFound{ 5, 6, 7 }, 29));
    CHECK(round_trips(NoRoom{ 5 }, 13));
    CHECK(round_trips(Turn{ "203.0.113.7:3478", "1700000000:0123456789abcdef", "pass/word+=" }, 8 + 16 + 27 + 11));
    CHECK(round_trips(Turn{ std::string(max_text, 'a'), "u", std::string(max_text, '~') }, 8 + 2 * max_text + 1));

    // The layout: "CS", the version, the kind, little-endian fields.
    const std::vector<uint8_t> reg = bytes_of(Register{ 0x0102030405060708ull });
    CHECK(reg == (std::vector<uint8_t>{ 'C', 'S', (uint8_t)version, (uint8_t)(version >> 8), 1, 8, 7, 6, 5, 4, 3, 2, 1 }));
    const std::vector<uint8_t> sig = bytes_of(Signal{ 1, { 0xEE } });
    CHECK(sig.size() == 16 && sig[4] == 2 && sig[13] == 1 && sig[14] == 0 && sig[15] == 0xEE);

    // Not encoded: id 0, an empty or oversized signal, an unknown reason.
    CHECK(bytes_of(Register{ 0 }).empty());
    CHECK(bytes_of(Signal{ 0, { 1 } }).empty());
    CHECK(bytes_of(Signal{ 1, {} }).empty());
    CHECK(bytes_of(Signal{ 1, std::vector<uint8_t>(max_signal + 1) }).empty());
    CHECK(bytes_of(Unreachable{ 0 }).empty());
    CHECK(bytes_of(Registered{ 0 }).empty());
    CHECK(bytes_of(Refused{ (Refusal)0 }).empty());
    CHECK(bytes_of(Refused{ (Refusal)((int)Refusal::last + 1) }).empty());
    CHECK(bytes_of(CreateRoom{ 0 }).empty());
    CHECK(bytes_of(RoomCreated{ 0 }).empty());
    CHECK(bytes_of(RoomCreated{ max_room + 1 }).empty());
    CHECK(bytes_of(FindRoom{ max_room + 1 }).empty());
    CHECK(bytes_of(NoRoom{ 0 }).empty());
    CHECK(bytes_of(RoomFound{ 1, 0, 1 }).empty());
    CHECK(bytes_of(RoomFound{ 1, 1, 0 }).empty());
    CHECK(bytes_of(RoomFound{ 0, 1, 1 }).empty());
    CHECK(bytes_of(Turn{ "", "u", "p" }).empty());
    CHECK(bytes_of(Turn{ "s", "u", std::string(max_text + 1, 'p') }).empty());
    CHECK(bytes_of(Turn{ "s", "a user", "p" }).empty());
    CHECK(bytes_of(Turn{ "s", "u", "p\x7F" }).empty());

    // Refused: another version, magic or kind; a size other than the kind's.
    std::vector<uint8_t> bad = reg;
    bad[2]++;
    CHECK(!decodes(bad) && other_version(bad));
    CHECK(!other_version(reg));
    bad = reg; bad[0] = 'X'; CHECK(!decodes(bad));
    bad = reg; bad[4] = 0; CHECK(!decodes(bad));
    bad = reg; bad[4] = 12; CHECK(!decodes(bad));
    bad = reg; bad.push_back(0); CHECK(!decodes(bad));
    bad = reg; bad.pop_back(); CHECK(!decodes(bad));
    CHECK(!decodes({}));
    CHECK(!decodes({ 'C', 'S', (uint8_t)version, (uint8_t)(version >> 8) }));
    // Id 0.
    bad = reg; for (int i = 5; i < 13; i++) bad[i] = 0; CHECK(!decodes(bad));
    bad = bytes_of(Unreachable{ 1 }); bad[5] = 0; CHECK(!decodes(bad));
    bad = bytes_of(Registered{ 1 }); bad[5] = 0; CHECK(!decodes(bad));
    // A signal whose size isn't what follows, is 0, or past max_signal.
    bad = sig; bad.push_back(0); CHECK(!decodes(bad));
    bad = sig; bad[13] = 2; CHECK(!decodes(bad));
    bad = sig; bad[13] = 0; bad.pop_back(); CHECK(!decodes(bad));
    bad = bytes_of(Signal{ 1, std::vector<uint8_t>(max_signal, 1) });
    bad[13] = (uint8_t)(max_signal + 1); bad[14] = (uint8_t)((max_signal + 1) >> 8); bad.push_back(1); CHECK(!decodes(bad));
    bad = sig; bad[5] = 0; CHECK(!decodes(bad));
    // A reason out of range.
    bad = bytes_of(Refused{ Refusal::busy }); bad[5] = 0; CHECK(!decodes(bad));
    bad[5] = (uint8_t)Refusal::last + 1; CHECK(!decodes(bad));

    // Rooms out of range.
    bad = bytes_of(FindRoom{ 1 }); bad[10] = 1; CHECK(!decodes(bad));   // past max_room
    bad = bytes_of(RoomCreated{ 1 }); bad[5] = 0; CHECK(!decodes(bad));
    const std::vector<uint8_t> found = bytes_of(RoomFound{ 1, 2, 3 });
    bad = found; bad[13] = 0; CHECK(!decodes(bad));                      // host 0
    bad = found; bad[21] = 0; CHECK(!decodes(bad));                      // key 0
    bad = found; bad.pop_back(); CHECK(!decodes(bad));
    bad = bytes_of(CreateRoom{ 1 }); bad[5] = 0; CHECK(!decodes(bad));
    // Turn: a size past what follows, 0 or past max_text, a character out of range, anything after.
    const std::vector<uint8_t> turn = bytes_of(Turn{ "s:1", "u", "p" });
    CHECK(turn.size() == 13 && turn[4] == 11 && turn[5] == 3 && turn[9] == 1 && turn[11] == 1);
    bad = turn; bad.push_back('x'); CHECK(!decodes(bad));
    bad = turn; bad.pop_back(); CHECK(!decodes(bad));
    bad = turn; bad[11] = 2; CHECK(!decodes(bad));
    bad = turn; bad[9] = 0; CHECK(!decodes(bad));
    bad = turn; bad[6] = ' '; CHECK(!decodes(bad));
    bad = turn; bad[6] = 0x80; CHECK(!decodes(bad));
    bad = turn; bad.resize(6); CHECK(!decodes(bad));
    bad = { 'C', 'S', (uint8_t)version, (uint8_t)(version >> 8), 11 }; CHECK(!decodes(bad));
    bad = bytes_of(Turn{ std::string(max_text, 'a'), "u", "p" }); bad[5] = (uint8_t)(max_text + 1); bad.insert(bad.begin() + 6, 'a');
    CHECK(!decodes(bad));

    if (failures == 0) {
        std::printf("signal_packets_test: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
