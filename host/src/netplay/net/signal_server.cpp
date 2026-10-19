// netplay_server: the signalling server. Peers connect over GameNetworkingSockets, each registers an id
// (Register, signal_packets.hpp; answered with Registered) and sends the library's ICE signals for another
// id (Signal); the server passes them on, from the sender's id, or says the other isn't there
// (Unreachable). It keeps rooms too: a host creates one for its match (CreateRoom, with the match's key)
// and gets a room code to pass on; a joiner looks it up (FindRoom) and gets the host's id and the key. A
// room lasts as long as its host's connection. A host may list its room in public (Publish), and anyone may
// ask for the listed rooms they could join (ListRooms): those whose game fits theirs (the same hash of
// build, ROM and settings) on a platform that plays with theirs (Linux and Windows together; macOS apart
// until it's tested with them). It never sees the game's packets, nor the TURN relay's
// traffic (coturn, beside it: tools/netplay/server/).
//
//   netplay_server [--port 27100] [--max-per-address 8] [--max-peers 4096] [--connection-burst 20]
//                  [--max-listed-per-address 2]
//                  [--turn <host:port> [--turn-hours 12]]   (with the secret in TURN_SECRET)
//
// With --turn, each peer is offered that TURN relay as it registers (Turn), with credentials of its own
// made from the secret it shares with coturn (turn_credentials.hpp), which expire after --turn-hours.
//
// Limits: a peer must register within 5 s, once, with an id nobody else has (nor had in the last hour, so
// a peer that leaves can't be posed as straight away); at most --max-per-address connections at once from
// one IP address (an IPv6 one's /64), --connection-burst new ones (then one each 2 s), and --max-peers in
// all; at most 20 packets a second from each (bursts of 60); one room each; from each address, at most 10
// lookups of rooms that aren't there (then one each 6 s), so codes can't be guessed; 6 room lists (then one
// each 3 s), each of at most signal::max_listed rooms; --max-listed-per-address public rooms from each
// address at once (more stay private). A room's code isn't
// given out again for an hour after it closes. A peer without a room that sends nothing for 10 minutes is
// closed, and any after 12 hours. Anything broken or past a limit closes the connection, after Refused. It
// logs counts, never addresses, codes or signals.
//
// It runs until SIGINT or SIGTERM (docker stop).

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include "join_code.hpp"
#include "signal_packets.hpp"
#include "turn_credentials.hpp"

using namespace conker::netplay;

namespace {
    constexpr double register_seconds = 5.0;
    constexpr double id_reuse_seconds = 3600.0;
    constexpr double signals_per_second = 20.0;
    constexpr double signal_burst = 60.0;
    constexpr int max_messages_per_loop = 1024;
    constexpr double misses_per_second = 1.0 / 6.0;
    constexpr double miss_burst = 10.0;
    // New connections from one address: each leaves an id (and maybe a room code) not reused for an hour,
    // so they're limited, to bound what that keeps.
    constexpr double connections_per_second = 0.5;
    // A registered peer without a room that sends nothing for this long is closed (it's done: a joiner
    // only signals while connecting), and any connection after max_seconds (longer than any match).
    constexpr double idle_seconds = 600.0;
    constexpr double max_seconds = 12 * 3600.0;
    // Room lists: each looks at every listed room.
    constexpr double lists_per_second = 1.0 / 3.0;
    constexpr double list_burst = 6.0;

    // Whether games on these platforms play together: Linux and Windows have, live; macOS hasn't been
    // tried with them.
    int platform_group(signal::Platform platform) {
        return platform == signal::Platform::macos ? 1 : 0;
    }

    volatile std::sig_atomic_t stopping = 0;
    void on_signal(int) { stopping = 1; }

    double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // What the per-address limits count by: an IPv4 address, or an IPv6 one's /64 (one network: a machine
    // can have any number of addresses in it).
    std::string address_key(const SteamNetworkingIPAddr& address) {
        if (address.IsIPv4()) {
            const uint32_t ip = address.GetIPv4();
            char text[16];
            std::snprintf(text, sizeof(text), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
            return text;
        }
        char text[32];
        const uint8_t* b = address.m_ipv6;
        std::snprintf(text, sizeof(text), "%02x%02x:%02x%02x:%02x%02x:%02x%02x::/64", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        return text;
    }

    // A rate: `tokens` that come back at `per_second`, up to `burst`.
    struct Bucket {
        double tokens = 0;
        double at = 0;
        // Takes one, if there is one.
        bool take(double t, double per_second, double burst) {
            tokens = std::min(burst, tokens + (t - at) * per_second);
            at = t;
            if (tokens < 1) return false;
            tokens -= 1;
            return true;
        }
    };

    struct Peer {
        uint64_t id = 0;           // 0 until it registers
        std::string address;       // its IP, for the per-address limits only
        double connected_at = 0;
        double last_packet = 0;
        Bucket packets;
        uint64_t room = 0;         // the room it hosts, if any
        uint64_t room_key = 0;
        bool listed = false;       // its room is in the public list (Publish), as `listing` says
        signal::Publish listing;
        double listed_at = 0;
        Bucket lists;
    };

    struct Server {
        HSteamListenSocket listen = k_HSteamListenSocket_Invalid;
        HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
        std::unordered_map<HSteamNetConnection, Peer> peers;
        std::unordered_map<uint64_t, HSteamNetConnection> by_id;
        // Ids whose peers left, and rooms closed, within the hour: not given out again yet. Oldest first.
        std::unordered_set<uint64_t> released;
        std::deque<std::pair<double, uint64_t>> released_order;
        std::unordered_map<uint64_t, HSteamNetConnection> rooms;
        std::unordered_set<uint64_t> closed_rooms;
        std::deque<std::pair<double, uint64_t>> closed_order;
        std::map<std::string, int> per_address;
        // Each IP address's lookups of rooms that weren't there, kept after its peers leave (until it's
        // full again), so reconnecting doesn't refill it.
        std::unordered_map<std::string, Bucket> misses;
        std::unordered_map<std::string, Bucket> arrivals;   // new connections, kept the same way
        int max_per_address = 8;
        // Public rooms from one address at once (--max-listed-per-address): more aren't listed (they stay
        // private), so one network can't fill the list with rooms nobody answers in.
        int max_listed_per_address = 2;
        size_t max_peers = 4096;
        std::string turn;           // the TURN relay offered, if any
        std::string turn_secret;
        double turn_hours = 12;
        double connection_burst = 20;   // --connection-burst
        std::random_device random;
        uint64_t signals_passed = 0;
        uint64_t rooms_created = 0;
        uint64_t rooms_found = 0;
        uint64_t rooms_missed = 0;
        uint64_t lists_sent = 0;

        void send(HSteamNetConnection connection, const signal::Packet& packet) {
            uint8_t buffer[signal::max_packet_size];
            const size_t size = signal::encode(packet, buffer);
            if (size != 0) {
                SteamNetworkingSockets()->SendMessageToConnection(connection, buffer, (uint32)size, k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
            }
        }

        void forget(HSteamNetConnection connection) {
            auto it = peers.find(connection);
            if (it == peers.end()) {
                return;
            }
            if (it->second.id != 0) {
                by_id.erase(it->second.id);
                released.insert(it->second.id);
                released_order.push_back({ now(), it->second.id });
            }
            if (it->second.room != 0) {
                rooms.erase(it->second.room);
                closed_rooms.insert(it->second.room);
                closed_order.push_back({ now(), it->second.room });
            }
            if (--per_address[it->second.address] <= 0) {
                per_address.erase(it->second.address);
            }
            peers.erase(it);
        }

        void refuse(HSteamNetConnection connection, signal::Refusal reason) {
            send(connection, signal::Refused{ reason });
            SteamNetworkingSockets()->CloseConnection(connection, k_ESteamNetConnectionEnd_App_Min + (int)reason, "refused", true);
            forget(connection);
        }

        void on_status(SteamNetConnectionStatusChangedCallback_t* info) {
            ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
            switch (info->m_info.m_eState) {
                case k_ESteamNetworkingConnectionState_Connecting: {
                    const std::string address = address_key(info->m_info.m_addrRemote);
                    const auto known = per_address.find(address);
                    const double t = now();
                    if (peers.size() >= max_peers || (known != per_address.end() && known->second >= max_per_address) ||
                        !arrivals.try_emplace(address, Bucket{ connection_burst, t }).first->second.take(t, connections_per_second, connection_burst)) {
                        sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)signal::Refusal::busy, "busy", false);
                        return;
                    }
                    if (sockets->AcceptConnection(info->m_hConn) != k_EResultOK) {
                        sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
                        return;
                    }
                    sockets->SetConnectionPollGroup(info->m_hConn, group);
                    per_address[address]++;
                    Peer peer;
                    peer.address = address;
                    peer.connected_at = peer.last_packet = now();
                    peer.packets = { signal_burst, peer.connected_at };
                    peer.lists = { list_burst, peer.connected_at };
                    peers[info->m_hConn] = peer;
                    break;
                }
                case k_ESteamNetworkingConnectionState_ClosedByPeer:
                case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
                    sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
                    forget(info->m_hConn);
                    break;
                default:
                    break;
            }
        }

        void take(HSteamNetConnection connection, std::span<const uint8_t> data) {
            auto it = peers.find(connection);
            if (it == peers.end()) {
                return;
            }
            Peer& peer = it->second;
            peer.last_packet = now();
            const std::optional<signal::Packet> packet = signal::decode(data);
            if (!packet) {
                refuse(connection, signal::other_version(data) ? signal::Refusal::version : signal::Refusal::protocol);
                return;
            }
            if (const signal::Register* r = std::get_if<signal::Register>(&*packet)) {
                if (peer.id != 0) {
                    refuse(connection, signal::Refusal::protocol);
                }
                else if (by_id.contains(r->id) || released.contains(r->id)) {
                    refuse(connection, signal::Refusal::taken);
                }
                else {
                    peer.id = r->id;
                    by_id[r->id] = connection;
                    // The relay first: once registered, a peer may connect at once.
                    if (!turn.empty()) {
                        const uint64_t expires = (uint64_t)std::time(nullptr) + (uint64_t)(turn_hours * 3600);
                        const TurnCredentials credentials = turn_credentials(turn_secret, r->id, expires);
                        send(connection, signal::Turn{ turn, credentials.user, credentials.password });
                    }
                    send(connection, signal::Registered{ r->id });
                }
                return;
            }
            if (peer.id == 0) {
                refuse(connection, signal::Refusal::protocol);
                return;
            }
            const double t = now();
            if (!peer.packets.take(t, signals_per_second, signal_burst)) {
                refuse(connection, signal::Refusal::busy);
                return;
            }
            if (const signal::Signal* s = std::get_if<signal::Signal>(&*packet)) {
                const auto to = by_id.find(s->peer);
                if (to == by_id.end()) {
                    send(connection, signal::Unreachable{ s->peer });
                    return;
                }
                send(to->second, signal::Signal{ peer.id, s->data });
                signals_passed++;
            }
            else if (const signal::CreateRoom* c = std::get_if<signal::CreateRoom>(&*packet)) {
                if (peer.room != 0) {
                    refuse(connection, signal::Refusal::protocol);
                    return;
                }
                // A random code, not one open now or lately.
                uint64_t room = 0;
                while (room == 0 || rooms.contains(room) || closed_rooms.contains(room)) {
                    room = ((uint64_t)random() << 32 | random()) & max_room;
                }
                peer.room = room;
                peer.room_key = c->key;
                rooms[room] = connection;
                rooms_created++;
                send(connection, signal::RoomCreated{ room });
            }
            else if (const signal::FindRoom* f = std::get_if<signal::FindRoom>(&*packet)) {
                const auto found = rooms.find(f->room);
                const auto host_it = found == rooms.end() ? peers.end() : peers.find(found->second);
                if (host_it != peers.end()) {
                    const Peer& host = host_it->second;
                    rooms_found++;
                    send(connection, signal::RoomFound{ f->room, host.id, host.room_key });
                    return;
                }
                rooms_missed++;
                auto bucket = misses.try_emplace(peer.address, Bucket{ miss_burst, t }).first;
                if (!bucket->second.take(t, misses_per_second, miss_burst)) {
                    refuse(connection, signal::Refusal::busy);
                    return;
                }
                send(connection, signal::NoRoom{ f->room });
            }
            else if (const signal::Publish* p = std::get_if<signal::Publish>(&*packet)) {
                if (peer.room == 0) {
                    refuse(connection, signal::Refusal::protocol);
                    return;
                }
                if (!peer.listed) {
                    const auto from_here = std::count_if(peers.begin(), peers.end(), [&peer](const auto& p) {
                        return p.second.listed && p.second.address == peer.address;
                    });
                    if (from_here >= max_listed_per_address) {
                        return;
                    }
                    peer.listed_at = t;
                }
                peer.listed = true;
                peer.listing = *p;
                peer.listing.room.room = peer.room;
            }
            else if (const signal::ListRooms* l = std::get_if<signal::ListRooms>(&*packet)) {
                if (!peer.lists.take(t, lists_per_second, list_burst)) {
                    refuse(connection, signal::Refusal::busy);
                    return;
                }
                send(connection, listed_for(*l));
                lists_sent++;
            }
            else {
                // A server's packet.
                refuse(connection, signal::Refusal::protocol);
            }
        }

        // The listed rooms for `ask`: of its fit and platform group, open ones first (the fullest, then the
        // oldest: to fill matches), then those being played; at most signal::max_listed. And how many of
        // its game and platform group have another fit.
        signal::Rooms listed_for(const signal::ListRooms& ask) const {
            std::vector<const Peer*> fitting;
            size_t others = 0;
            for (const auto& [room, connection] : rooms) {
                const auto host = peers.find(connection);
                if (host == peers.end() || !host->second.listed ||
                    platform_group(host->second.listing.platform) != platform_group(ask.platform)) {
                    continue;
                }
                if (host->second.listing.fit == ask.fit) {
                    fitting.push_back(&host->second);
                }
                else if (host->second.listing.game == ask.game) {
                    others++;
                }
            }
            const auto open = [](const Peer* p) { return !p->listing.room.playing && p->listing.room.joined < p->listing.room.players; };
            std::sort(fitting.begin(), fitting.end(), [&open](const Peer* a, const Peer* b) {
                if (open(a) != open(b)) return open(a);
                if (a->listing.room.joined != b->listing.room.joined) return a->listing.room.joined > b->listing.room.joined;
                return a->listed_at < b->listed_at;
            });
            signal::Rooms out;
            out.others = (uint16_t)std::min<size_t>(others, UINT16_MAX);
            for (size_t i = 0; i < fitting.size() && i < signal::max_listed; i++) {
                out.rooms.push_back(fitting[i]->listing.room);
            }
            return out;
        }

        // Peers that never registered, idle or too old; ids and rooms released long enough ago to be given
        // out again; addresses whose limits are full again. Once a second.
        void expire() {
            const double t = now();
            std::vector<HSteamNetConnection> late;
            for (const auto& [connection, peer] : peers) {
                if ((peer.id == 0 && t - peer.connected_at > register_seconds) ||
                    (peer.room == 0 && t - peer.last_packet > idle_seconds) || t - peer.connected_at > max_seconds) {
                    late.push_back(connection);
                }
            }
            for (HSteamNetConnection connection : late) {
                refuse(connection, signal::Refusal::protocol);
            }
            for (auto [order, set] : { std::pair{ &released_order, &released }, std::pair{ &closed_order, &closed_rooms } }) {
                while (!order->empty() && t - order->front().first >= id_reuse_seconds) {
                    set->erase(order->front().second);
                    order->pop_front();
                }
            }
            std::erase_if(misses, [t](const auto& entry) { return entry.second.tokens + (t - entry.second.at) * misses_per_second >= miss_burst; });
            std::erase_if(arrivals, [this, t](const auto& entry) {
                return entry.second.tokens + (t - entry.second.at) * connections_per_second >= connection_burst;
            });
        }
    };

    Server* server = nullptr;
    void status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
        if (server != nullptr) server->on_status(info);
    }

    int usage() {
        std::fprintf(stderr, "usage: netplay_server [--port 27100] [--max-per-address 8] [--max-peers 4096] [--connection-burst 20]\n"
                             "                      [--max-listed-per-address 2]\n"
                             "                      [--turn <host:port> [--turn-hours 12]]   (the secret in TURN_SECRET)\n");
        return 2;
    }
}

int main(int argc, char** argv) {
    int port = 27100;
    Server state;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return usage();
        if (arg == "--turn") {
            state.turn = argv[++i];
            continue;
        }
        const int value = std::atoi(argv[++i]);
        if (arg == "--port" && value > 0 && value < 65536) port = value;
        else if (arg == "--turn-hours" && value > 0 && value <= 48) state.turn_hours = value;
        else if (arg == "--connection-burst" && value > 0) state.connection_burst = value;
        else if (arg == "--max-per-address" && value > 0) state.max_per_address = value;
        else if (arg == "--max-listed-per-address" && value > 0) state.max_listed_per_address = value;
        else if (arg == "--max-peers" && value > 0) state.max_peers = (size_t)value;
        else return usage();
    }

    if (!state.turn.empty()) {
        const char* secret = std::getenv("TURN_SECRET");
        state.turn_secret = secret != nullptr ? secret : "";
        if (state.turn_secret.size() < 16) {
            std::fprintf(stderr, "netplay_server: --turn needs the secret it shares with the TURN server in TURN_SECRET (16 characters or more)\n");
            return 2;
        }
        static uint8_t buffer[signal::max_packet_size];
        if (signal::encode(signal::Turn{ state.turn, "u", "p" }, buffer) == 0) {
            std::fprintf(stderr, "netplay_server: --turn takes host:port, separated by commas (no spaces, %zu characters at most)\n", signal::max_text);
            return 2;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    SteamNetworkingErrMsg message;
    if (!GameNetworkingSockets_Init(nullptr, message)) {
        std::fprintf(stderr, "netplay_server: %s\n", message);
        return 1;
    }
    server = &state;

    SteamNetworkingIPAddr address;
    address.Clear();
    address.m_port = (uint16)port;
    SteamNetworkingConfigValue_t options[4];
    options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)status_changed);
    options[1].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, (int32_t)signal::max_packet_size);
    options[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages, 128);
    options[3].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, 16 * (int32_t)signal::max_packet_size);
    state.listen = SteamNetworkingSockets()->CreateListenSocketIP(address, 4, options);
    if (state.listen == k_HSteamListenSocket_Invalid) {
        std::fprintf(stderr, "netplay_server: couldn't listen on UDP port %d\n", port);
        GameNetworkingSockets_Kill();
        return 1;
    }
    state.group = SteamNetworkingSockets()->CreatePollGroup();
    std::printf("netplay_server: listening on UDP port %d (signalling version %u)%s\n", port, (unsigned)signal::version,
        state.turn.empty() ? ", no TURN relay" : ", offering a TURN relay");
    std::fflush(stdout);

    double next_report = now() + 600;
    double next_expiry = now();
    while (!stopping) {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        sockets->RunCallbacks();
        SteamNetworkingMessage_t* messages[64];
        int count;
        for (int taken = 0; taken < max_messages_per_loop && (count = sockets->ReceiveMessagesOnPollGroup(state.group, messages, 64)) > 0; taken += count) {
            for (int i = 0; i < count; i++) {
                state.take(messages[i]->m_conn, { (const uint8_t*)messages[i]->m_pData, (size_t)messages[i]->m_cbSize });
                messages[i]->Release();
            }
        }
        if (now() >= next_expiry) {
            state.expire();
            next_expiry = now() + 1;
        }
        if (now() >= next_report) {
            const size_t listed = (size_t)std::count_if(state.peers.begin(), state.peers.end(), [](const auto& p) { return p.second.listed; });
            std::printf("netplay_server: %zu peers, %zu rooms (%zu listed); %llu signals passed, rooms %llu created, %llu found, %llu missed; %llu lists\n",
                state.peers.size(), state.rooms.size(), listed, (unsigned long long)state.signals_passed, (unsigned long long)state.rooms_created,
                (unsigned long long)state.rooms_found, (unsigned long long)state.rooms_missed, (unsigned long long)state.lists_sent);
            std::fflush(stdout);
            next_report = now() + 600;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::printf("netplay_server: stopping\n");
    SteamNetworkingSockets()->CloseListenSocket(state.listen);
    server = nullptr;
    GameNetworkingSockets_Kill();
    return 0;
}
