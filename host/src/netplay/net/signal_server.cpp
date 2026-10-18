// netplay_server: the signalling server. Peers connect over GameNetworkingSockets, each registers an id
// (Register, signal_packets.hpp) and sends the library's ICE signals for another id (Signal); the server
// passes them on, from the sender's id, or says the other isn't there (Unreachable). It never sees the
// game's packets, nor the TURN relay's traffic (coturn, beside it: tools/netplay/server/).
//
//   netplay_server [--port 27100] [--max-per-address 8] [--max-peers 4096]
//
// Limits: a peer must register within 5 s, once, with an id nobody else has (nor had in the last hour, so
// a peer that leaves can't be posed as straight away); at most --max-per-address connections from one IP
// address and --max-peers in all; at most 20 signals a second from each (bursts of 60). Anything broken
// or past a limit closes the connection, after Refused. It logs counts, never addresses or signals.
//
// It runs until SIGINT or SIGTERM (docker stop).

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include "signal_packets.hpp"

using namespace conker::netplay;

namespace {
    constexpr double register_seconds = 5.0;
    constexpr double id_reuse_seconds = 3600.0;
    constexpr double signals_per_second = 20.0;
    constexpr double signal_burst = 60.0;
    constexpr int max_messages_per_loop = 1024;

    volatile std::sig_atomic_t stopping = 0;
    void on_signal(int) { stopping = 1; }

    double now() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    struct Peer {
        uint64_t id = 0;           // 0 until it registers
        std::string address;       // its IP, for the per-address limit only
        double connected_at = 0;
        double tokens = signal_burst;
        double tokens_at = 0;
    };

    struct Server {
        HSteamListenSocket listen = k_HSteamListenSocket_Invalid;
        HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
        std::unordered_map<HSteamNetConnection, Peer> peers;
        std::unordered_map<uint64_t, HSteamNetConnection> by_id;
        std::unordered_map<uint64_t, double> released;    // id -> when its peer left
        std::map<std::string, int> per_address;
        int max_per_address = 8;
        size_t max_peers = 4096;
        uint64_t signals_passed = 0;

        void send(HSteamNetConnection connection, const signal::Packet& packet) {
            uint8_t buffer[signal::max_packet_size];
            const size_t size = signal::encode(packet, buffer);
            if (size != 0) {
                SteamNetworkingSockets()->SendMessageToConnection(connection, buffer, (uint32)size, k_nSteamNetworkingSend_Reliable, nullptr);
            }
        }

        void forget(HSteamNetConnection connection) {
            auto it = peers.find(connection);
            if (it == peers.end()) {
                return;
            }
            if (it->second.id != 0) {
                by_id.erase(it->second.id);
                released[it->second.id] = now();
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
                    char ip[SteamNetworkingIPAddr::k_cchMaxString];
                    info->m_info.m_addrRemote.ToString(ip, sizeof(ip), false);
                    const std::string address = ip;
                    if (peers.size() >= max_peers || per_address[address] >= max_per_address) {
                        if (per_address[address] == 0) per_address.erase(address);
                        sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)signal::Refusal::busy, "busy", false);
                        return;
                    }
                    if (sockets->AcceptConnection(info->m_hConn) != k_EResultOK) {
                        if (per_address[address] == 0) per_address.erase(address);
                        sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
                        return;
                    }
                    sockets->SetConnectionPollGroup(info->m_hConn, group);
                    per_address[address]++;
                    Peer peer;
                    peer.address = address;
                    peer.connected_at = peer.tokens_at = now();
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
            const std::optional<signal::Packet> packet = signal::decode(data);
            if (!packet) {
                refuse(connection, signal::other_version(data) ? signal::Refusal::version : signal::Refusal::protocol);
                return;
            }
            if (const signal::Register* r = std::get_if<signal::Register>(&*packet)) {
                const auto reused = released.find(r->id);
                if (peer.id != 0) {
                    refuse(connection, signal::Refusal::protocol);
                }
                else if (by_id.contains(r->id) || (reused != released.end() && now() - reused->second < id_reuse_seconds)) {
                    refuse(connection, signal::Refusal::taken);
                }
                else {
                    peer.id = r->id;
                    by_id[r->id] = connection;
                }
                return;
            }
            const signal::Signal* s = std::get_if<signal::Signal>(&*packet);
            if (s == nullptr || peer.id == 0) {
                refuse(connection, signal::Refusal::protocol);
                return;
            }
            const double t = now();
            peer.tokens = std::min(signal_burst, peer.tokens + (t - peer.tokens_at) * signals_per_second);
            peer.tokens_at = t;
            if (peer.tokens < 1) {
                refuse(connection, signal::Refusal::busy);
                return;
            }
            peer.tokens -= 1;
            const auto to = by_id.find(s->peer);
            if (to == by_id.end()) {
                send(connection, signal::Unreachable{ s->peer });
                return;
            }
            send(to->second, signal::Signal{ peer.id, s->data });
            signals_passed++;
        }

        // Peers that never registered, and ids released long enough ago to be forgotten.
        void expire() {
            const double t = now();
            std::vector<HSteamNetConnection> late;
            for (const auto& [connection, peer] : peers) {
                if (peer.id == 0 && t - peer.connected_at > register_seconds) late.push_back(connection);
            }
            for (HSteamNetConnection connection : late) {
                refuse(connection, signal::Refusal::protocol);
            }
            std::erase_if(released, [t](const auto& entry) { return t - entry.second >= id_reuse_seconds; });
        }
    };

    Server* server = nullptr;
    void status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
        if (server != nullptr) server->on_status(info);
    }

    int usage() {
        std::fprintf(stderr, "usage: netplay_server [--port 27100] [--max-per-address 8] [--max-peers 4096]\n");
        return 2;
    }
}

int main(int argc, char** argv) {
    int port = 27100;
    Server state;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) return usage();
        const int value = std::atoi(argv[++i]);
        if (arg == "--port" && value > 0 && value < 65536) port = value;
        else if (arg == "--max-per-address" && value > 0) state.max_per_address = value;
        else if (arg == "--max-peers" && value > 0) state.max_peers = (size_t)value;
        else return usage();
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
    std::printf("netplay_server: listening on UDP port %d (signalling version %u)\n", port, (unsigned)signal::version);
    std::fflush(stdout);

    double next_report = now() + 600;
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
        state.expire();
        if (now() >= next_report) {
            std::printf("netplay_server: %zu peers, %llu signals passed\n", state.peers.size(), (unsigned long long)state.signals_passed);
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
