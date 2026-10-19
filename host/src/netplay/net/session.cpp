// A netplay session over GameNetworkingSockets: see session.hpp.

#include "session.hpp"
#include "join_code.hpp"
#include "signalling.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <sys/types.h>
#include <sys/random.h>   // getentropy (Linux, macOS)
#endif

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#include <steam/steamnetworkingsockets.h>

namespace conker::netplay {
    namespace {
        // A joiner that hasn't finished the handshake in this long is dropped (an honest Hello takes a
        // round trip).
        constexpr double handshake_seconds = 3.0;
        // Connections the host takes at once, joined or not: more are turned away unread.
        constexpr size_t max_connections = 8;
        // What the library buffers from one peer, unread: a message bigger than the largest packet closes
        // the connection before it's buffered, and past these limits more are dropped (and resent, as
        // everything is sent reliably). An honest peer is never more than a few dozen messages ahead.
        constexpr int recv_buffer_messages = 512;
        constexpr int recv_buffer_bytes = recv_buffer_messages * (int)max_packet_size;
        // The most messages one poll() takes: a flood can't hold the game's thread there.
        constexpr int max_messages_per_poll = 256;

        // The options for every connection, as the host's listen socket or a joiner's connection, and how an
        // ICE one finds its way (the strings stay `ice`'s, which must outlive the call they're given to).
        struct ConnectionOptions {
            SteamNetworkingConfigValue_t values[10];
            int count = 4;
            ConnectionOptions(void (*status_changed)(SteamNetConnectionStatusChangedCallback_t*), const Ice* ice = nullptr) {
                values[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)status_changed);
                values[1].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, (int32_t)max_packet_size);
                values[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, recv_buffer_bytes);
                values[3].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages, recv_buffer_messages);
                if (ice != nullptr) {
                    values[count++].SetInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, ice->relay_only
                        ? k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay : k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All);
                    values[count++].SetString(k_ESteamNetworkingConfig_P2P_STUN_ServerList, ice->relay_only ? "" : ice->stun.c_str());
                    values[count++].SetString(k_ESteamNetworkingConfig_P2P_TURN_ServerList, ice->turn.c_str());
                    values[count++].SetString(k_ESteamNetworkingConfig_P2P_TURN_UserList, ice->turn_user.c_str());
                    values[count++].SetString(k_ESteamNetworkingConfig_P2P_TURN_PassList, ice->turn_password.c_str());
                }
            }
        };

        // A match's key: 64 bits from the system's random source (never 0, which is "no key").
        bool random_key(uint64_t& key) {
            do {
#if defined(_WIN32)
                if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, (PUCHAR)&key, sizeof(key), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
                    return false;
                }
#else
                if (getentropy(&key, sizeof(key)) != 0) {
                    return false;
                }
#endif
            } while (key == 0);
            return true;
        }

        // Whether two keys are the same, taking the same time whatever they are (no early exit for a
        // stranger to time).
        bool same_key(uint64_t a, uint64_t b) {
            volatile uint64_t difference = 0;
            for (int i = 0; i < 64; i += 8) {
                difference = difference | (((a >> i) ^ (b >> i)) & 0xFF);
            }
            return difference == 0;
        }

        Session* current = nullptr;
        uint64_t process_id = 0;

        // The library's identity for a process's id, and back (0: not one of ours).
        SteamNetworkingIdentity identity_of(uint64_t id) {
            char text[32];
            std::snprintf(text, sizeof(text), "conker-%016llx", (unsigned long long)id);
            SteamNetworkingIdentity identity;
            identity.SetGenericString(text);
            return identity;
        }
        uint64_t id_of(const SteamNetworkingIdentity& identity) {
            const char* text = identity.GetGenericString();
            unsigned long long id = 0;
            char end = 0;
            if (text == nullptr || std::strlen(text) != 23 || std::sscanf(text, "conker-%16llx%c", &id, &end) != 1) {
                return 0;
            }
            return id;
        }

        // A connection's signals, to its peer's id through the signaller's outbox (which it keeps, as the
        // library may send one after the session has gone).
        class ConnectionSignals : public ISteamNetworkingConnectionSignaling {
        public:
            ConnectionSignals(std::shared_ptr<SignalOutbox> outbox, uint64_t peer) : outbox(std::move(outbox)), peer(peer) {}
            bool SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* data, int size) override {
                outbox->push(peer, { (const uint8_t*)data, (size_t)size });
                return true;
            }
            void Release() override { delete this; }
        private:
            std::shared_ptr<SignalOutbox> outbox;
            uint64_t peer;
        };

        double now() {
            using namespace std::chrono;
            return duration<double>(steady_clock::now().time_since_epoch()).count();
        }

        void debug_output(ESteamNetworkingSocketsDebugOutputType, const char* message) {
            std::fprintf(stderr, "[netplay] %s\n", message);
        }

        // The reason a connection was closed for: ours if the peer gave one, otherwise the library's.
        Reason reason_of(const SteamNetConnectionInfo_t& info) {
            const int code = info.m_eEndReason - k_ESteamNetConnectionEnd_App_Min;
            if (code >= 1 && code <= last_reason) {
                return (Reason)code;
            }
            return info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally ? Reason::timeout : Reason::quit;
        }
    }

    // Signals that arrive for this process: someone connecting to the host (each gets its own
    // ConnectionSignals), or a refusal to send back. A joiner takes no new connections.
    struct Session::IceContext : ISteamNetworkingSignalingRecvContext {
        std::shared_ptr<SignalOutbox> outbox;
        bool accepting = false;
        ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection, const SteamNetworkingIdentity& peer, int) override {
            const uint64_t id = id_of(peer);
            return accepting && id != 0 ? new ConnectionSignals(outbox, id) : nullptr;
        }
        void SendRejectionSignal(const SteamNetworkingIdentity& peer, const void* data, int size) override {
            if (const uint64_t id = id_of(peer); id != 0) {
                outbox->push(id, { (const uint8_t*)data, (size_t)size });
            }
        }
    };

    void status_changed_callback(void* info) {
        if (current != nullptr) {
            current->on_status_changed(info);
        }
    }

    namespace {
        void status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
            status_changed_callback(info);
        }
    }

    std::string join_code_for(const std::string& address, uint64_t key) {
        SteamNetworkingIPAddr parsed;
        if (!parsed.ParseString(address.c_str())) {
            return {};
        }
        Endpoint endpoint;
        endpoint.port = parsed.m_port;
        endpoint.key = key;
        if (parsed.IsIPv4()) {
            const uint32_t ip = parsed.GetIPv4();
            endpoint.ip = { (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
        }
        else {
            endpoint.ipv6 = true;
            std::copy(std::begin(parsed.m_ipv6), std::end(parsed.m_ipv6), endpoint.ip.begin());
        }
        return encode_join_code(endpoint);
    }

    bool startup(std::string& error) {
        if (!random_key(process_id)) {
            error = "couldn't make this process's id (no random source)";
            return false;
        }
        const SteamNetworkingIdentity identity = identity_of(process_id);
        SteamNetworkingErrMsg message;
        if (!GameNetworkingSockets_Init(&identity, message)) {
            error = message;
            return false;
        }
        SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, debug_output);
        return true;
    }

    uint64_t local_id() {
        return process_id;
    }

    bool new_id(std::string& error) {
        uint64_t id = 0;
        if (!random_key(id)) {
            error = "couldn't make a new id (no random source)";
            return false;
        }
        process_id = id;
        const SteamNetworkingIdentity identity = identity_of(process_id);
        SteamNetworkingSockets()->ResetIdentity(&identity);
        return true;
    }

    void shutdown() {
        GameNetworkingSockets_Kill();
    }

    void set_fake_network(const FakeNetwork& fake) {
        ISteamNetworkingUtils* utils = SteamNetworkingUtils();
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketLag_Send, fake.lag_ms);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Avg, fake.jitter_ms);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Max, 4 * fake.jitter_ms);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketJitter_Send_Pct, fake.jitter_ms > 0 ? 100.0f : 0.0f);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Send, fake.outage ? 100.0f : fake.loss);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketLoss_Recv, fake.outage ? 100.0f : 0.0f);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketReorder_Send, fake.reorder);
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketReorder_Time, 20);
        utils->SetGlobalConfigValueFloat(k_ESteamNetworkingConfig_FakePacketDup_Send, fake.dup);
        utils->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_FakePacketDup_TimeMax, 20);
    }

    Session::Session() {
        current = this;
    }

    Session::~Session() {
        if (!peers.empty()) {
            leave(Reason::quit);
        }
        if (listen_socket != k_HSteamListenSocket_Invalid) {
            SteamNetworkingSockets()->CloseListenSocket(listen_socket);
        }
        if (poll_group != k_HSteamNetPollGroup_Invalid) {
            SteamNetworkingSockets()->DestroyPollGroup(poll_group);
        }
        current = nullptr;
    }

    bool Session::begin_hosting(int players, const Hello& me, std::string& error, int local) {
        if (players < 2 || players > max_players) {
            error = "a match is 2 to 4 players";
            return false;
        }
        if (local < 1 || local >= players) {
            error = "a match needs a player on another machine";
            return false;
        }
        hello = me;
        if (!random_key(hello.key)) {
            error = "couldn't make the match's key (no random source)";
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        is_host = true;
        own_ports = (uint8_t)((1u << local) - 1);
        match_players = players;
        return true;
    }

    bool Session::host(uint16_t port, int players, const Hello& me, std::string& error, int local) {
        if (!begin_hosting(players, me, error, local)) {
            return false;
        }
        SteamNetworkingIPAddr address;
        address.Clear();
        address.m_port = port;
        ConnectionOptions options(status_changed);
        listen_socket = SteamNetworkingSockets()->CreateListenSocketIP(address, options.count, options.values);
        if (listen_socket == k_HSteamListenSocket_Invalid) {
            error = "couldn't listen on UDP port " + std::to_string(port);
            return false;
        }
        return true;
    }

    bool Session::host(Signaller& through, const Ice& ice, int players, const Hello& me, std::string& error, int local) {
        if (!begin_hosting(players, me, error, local)) {
            return false;
        }
        ConnectionOptions options(status_changed, &ice);
        listen_socket = SteamNetworkingSockets()->CreateListenSocketP2P(0, options.count, options.values);
        if (listen_socket == k_HSteamListenSocket_Invalid) {
            error = "couldn't listen for peer-to-peer connections";
            return false;
        }
        signaller = &through;
        ice_context = std::make_unique<IceContext>();
        ice_context->outbox = through.outbox();
        ice_context->accepting = true;
        return true;
    }

    bool Session::join(const std::string& code, const Hello& me, std::string& error) {
        const std::optional<Endpoint> endpoint = decode_join_code(code);
        if (!endpoint) {
            error = "\"" + code + "\" isn't a join code (or has a typo)";
            return false;
        }
        SteamNetworkingIPAddr host_address;
        if (endpoint->ipv6) {
            host_address.SetIPv6(endpoint->ip.data(), endpoint->port);
        }
        else {
            const auto& ip = endpoint->ip;
            host_address.SetIPv4((uint32_t)ip[0] << 24 | ip[1] << 16 | ip[2] << 8 | ip[3], endpoint->port);
        }
        ConnectionOptions options(status_changed);
        const HSteamNetConnection connection = SteamNetworkingSockets()->ConnectByIPAddress(host_address, options.count, options.values);
        if (connection == k_HSteamNetConnection_Invalid) {
            error = "couldn't connect to the host";
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        SteamNetworkingSockets()->SetConnectionPollGroup(connection, poll_group);
        is_host = false;
        hello = me;
        hello.key = endpoint->key;
        peers.push_back({ connection, 0, 0, now() });  // the host, from port 0
        return true;
    }

    bool Session::join(Signaller& through, const Ice& ice, uint64_t host_id, uint64_t key, const Hello& me, std::string& error) {
        if (host_id == 0 || host_id == process_id) {
            error = "that isn't another host's id";
            return false;
        }
        signaller = &through;
        ice_context = std::make_unique<IceContext>();
        ice_context->outbox = through.outbox();
        const SteamNetworkingIdentity host_identity = identity_of(host_id);
        ConnectionOptions options(status_changed, &ice);
        const HSteamNetConnection connection = SteamNetworkingSockets()->ConnectP2PCustomSignaling(
            new ConnectionSignals(through.outbox(), host_id), &host_identity, 0, options.count, options.values);
        if (connection == k_HSteamNetConnection_Invalid) {
            error = "couldn't connect to the host";
            return false;
        }
        poll_group = SteamNetworkingSockets()->CreatePollGroup();
        SteamNetworkingSockets()->SetConnectionPollGroup(connection, poll_group);
        is_host = false;
        ice_host = host_id;
        hello = me;
        hello.key = key;
        peers.push_back({ connection, 0, 0, now() });  // the host, from port 0
        return true;
    }

    void Session::take_signals(std::vector<Event>& events) {
        if (signaller == nullptr) {
            return;
        }
        signaller->poll();
        for (const signal::Signal& s : signaller->received()) {
            SteamNetworkingSockets()->ReceivedP2PCustomSignal(s.data.data(), (int)s.data.size(), ice_context.get());
        }
        // A joiner still finding its way to the host: ended if the host isn't on the server, or the server
        // can't be reached. (Once connected, the signaller doesn't matter to it.)
        const std::vector<uint64_t> gone = signaller->unreachable();
        if (!is_host && !peers.empty() && own_ports == 0) {
            std::string why;
            if (std::find(gone.begin(), gone.end(), ice_host) != gone.end()) {
                why = "the host isn't on the signalling server";
            }
            else if (signaller->state() == Signaller::State::failed) {
                why = "signalling: " + signaller->error();
            }
            if (!why.empty()) {
                SteamNetworkingSockets()->CloseConnection(peers[0].connection, 0, nullptr, false);
                peers.clear();
                Event event{ Event::Type::ended };
                event.reason = Reason::timeout;
                event.detail = why;
                event.by_signalling = true;
                events.push_back(std::move(event));
            }
        }
    }

    bool Session::start(bool own_view, int delay, int rollback) {
        int taken = std::popcount(own_ports);
        for (const Peer& peer : peers) {
            taken += std::popcount(peer.ports);
        }
        if (!is_host || !entry_open || taken != match_players || delay < 0 || delay > max_delay ||
            rollback < 0 || rollback > max_rollback) {
            return false;
        }
        entry_open = false;
        next_inputs.fill(0);
        const Start start{ (uint8_t)match_players, (uint8_t)(own_view ? 1 : 0), (uint8_t)delay, pairs(), (uint8_t)rollback };
        for (const Peer& peer : peers) {
            if (peer.port >= 0) {
                send_to(peer.connection, start);
            }
        }
        return true;
    }

    uint8_t Session::pairs() const {
        uint8_t out = std::popcount(own_ports) == 2 ? own_ports : 0;
        for (const Peer& peer : peers) {
            if (std::popcount(peer.ports) == 2) {
                out |= peer.ports;
            }
        }
        return out;
    }

    std::vector<int> Session::round_trips_ms() const {
        std::vector<int> out;
        for (const Peer& peer : peers) {
            SteamNetConnectionRealTimeStatus_t status;
            if (peer.port >= 0 &&
                SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.connection, &status, 0, nullptr) == k_EResultOK &&
                status.m_nPing >= 0) {
                out.push_back(status.m_nPing);
            }
        }
        return out;
    }

    std::array<int, max_players> Session::round_trips_by_port() const {
        std::array<int, max_players> out;
        out.fill(-1);
        for (const Peer& peer : peers) {
            SteamNetConnectionRealTimeStatus_t status;
            if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.connection, &status, 0, nullptr) != k_EResultOK ||
                status.m_nPing < 0) {
                continue;
            }
            const uint8_t ports = is_host ? peer.ports : 0x01;
            for (int port = 0; port < max_players; port++) {
                if ((ports & (1u << port)) != 0) out[port] = status.m_nPing;
            }
        }
        return out;
    }

    std::string Session::route(int port) const {
        for (const Peer& peer : peers) {
            if (is_host ? (peer.ports & (1u << port)) == 0 : port != 0) {
                continue;
            }
            SteamNetConnectionInfo_t info;
            if (!SteamNetworkingSockets()->GetConnectionInfo(peer.connection, &info)) {
                return "?";
            }
            // Through ICE, not the address (the library gives the peer's own on a direct route: not for logs).
            if (signaller != nullptr || info.m_addrRemote.IsIPv6AllZeros()) {
                return info.m_szConnectionDescription;
            }
            char address[SteamNetworkingIPAddr::k_cchMaxString];
            info.m_addrRemote.ToString(address, sizeof(address), true);
            return std::string(address) + " (" + info.m_szConnectionDescription + ")";
        }
        return "?";
    }

    int Session::joiners() const {
        return (int)std::count_if(peers.begin(), peers.end(), [](const Peer& p) { return p.port >= 0; });
    }

    int Session::arriving() const {
        return (int)std::count_if(peers.begin(), peers.end(), [](const Peer& p) { return p.port < 0; });
    }

    void Session::set_room_settings(bool own_view, int delay, int rollback) {
        settings.own_view = own_view ? 1 : 0;
        settings.delay = delay < 0 ? auto_delay : (uint8_t)(std::min)(delay, max_delay);
        settings.rollback = (uint8_t)std::clamp(rollback, 0, max_rollback);
    }

    void Session::set_ready(bool ready) {
        if (is_host) {
            own_ready = ready;
        }
        else if (own_ports != 0 && !started && !peers.empty()) {
            send_to(peers[0].connection, Ready{ (uint8_t)(ready ? 1 : 0) });
        }
    }

    bool Session::kick(int port) {
        if (!is_host || port < 0 || port >= max_players) {
            return false;
        }
        for (Peer& peer : peers) {
            if (peer.port >= 0 && (peer.ports & (1u << port)) != 0) {
                drop(peer, Reason::kicked, &pending_events);
                return true;
            }
        }
        return false;
    }

    Roster Session::roster() const {
        if (!is_host) {
            return host_roster;
        }
        Roster r = settings;
        r.players = (uint8_t)match_players;
        const auto seat = [&r](uint8_t ports, const Seat& s) {
            for (int port = 0; port < max_players; port++) {
                if ((ports & (1u << port)) != 0) r.seats[port] = s;
            }
        };
        seat(own_ports, { hello.player, hello.platform, (uint8_t)(own_ready ? 1 : 0), 0 });
        for (const Peer& peer : peers) {
            if (peer.port < 0) continue;
            SteamNetConnectionRealTimeStatus_t status;
            uint16_t trip = 0xFFFF;
            if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.connection, &status, 0, nullptr) == k_EResultOK && status.m_nPing >= 0) {
                trip = (uint16_t)(std::min)(status.m_nPing, 0xFFFE);
            }
            seat(peer.ports, { peer.player, peer.platform, (uint8_t)(peer.ready ? 1 : 0), trip });
        }
        return r;
    }

    bool Session::everyone_ready() const {
        if (!is_host || !own_ready) {
            return false;
        }
        int taken = std::popcount(own_ports);
        for (const Peer& peer : peers) {
            if (peer.port < 0) continue;
            if (!peer.ready) return false;
            taken += std::popcount(peer.ports);
        }
        return taken == match_players;
    }

    std::string Session::join_code(const std::string& address) const {
        return is_host ? join_code_for(address, hello.key) : std::string();
    }

    Session::Peer* Session::find(uint32_t connection) {
        auto it = std::find_if(peers.begin(), peers.end(), [connection](const Peer& p) { return p.connection == connection; });
        return it == peers.end() ? nullptr : &*it;
    }

    void Session::on_status_changed(const void* data) {
        const auto* info = (const SteamNetConnectionStatusChangedCallback_t*)data;
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        switch (info->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connecting:
                // Someone connecting to the host: taken in, to say Hello (or be refused) within the time.
                if (is_host && info->m_info.m_hListenSocket == listen_socket) {
                    // Once the match has started nobody more can join: turned away at once, not held until
                    // their Hello.
                    if (!entry_open) {
                        sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)Reason::started, "started", false);
                        return;
                    }
                    if (peers.size() >= max_connections || sockets->AcceptConnection(info->m_hConn) != k_EResultOK) {
                        sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)Reason::full, "full", false);
                        return;
                    }
                    sockets->SetConnectionPollGroup(info->m_hConn, poll_group);
                    peers.push_back({ info->m_hConn, -1, 0, now() });
                }
                break;
            case k_ESteamNetworkingConnectionState_Connected:
                if (!is_host && find(info->m_hConn) != nullptr) {
                    send_to(info->m_hConn, hello);
                }
                // The handshake time runs from here: through ICE, finding the way can take longer than
                // the time itself (the library's own connect timeout covers a way never found).
                else if (Peer* peer = find(info->m_hConn); is_host && peer != nullptr) {
                    peer->connected_at = now();
                    peer->up = true;
                }
                break;
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                // What the peer sent before closing is read first, in order: closing the connection would
                // throw it away, and a peer's last words matter (after a desync, its Hash for that read, then
                // its Bye). A Bye among them ends the peer itself.
                drain(info->m_hConn, pending_events);
                Peer* peer = find(info->m_hConn);
                // With why, should the other end still hear it (a connection that timed out here only).
                sockets->CloseConnection(info->m_hConn, k_ESteamNetConnectionEnd_App_Min + (int)reason_of(info->m_info), nullptr, false);
                if (peer == nullptr) {
                    break;
                }
                Event event{ is_host ? Event::Type::left : Event::Type::ended };
                event.port = peer->port;
                event.ports = peer->ports;
                event.reason = reason_of(info->m_info);
                // The peer's own words, for the log: printable characters only.
                for (const char* c = info->m_info.m_szEndDebug; *c != '\0'; c++) {
                    event.detail += (*c >= 0x20 && *c < 0x7F) ? *c : '?';
                }
                if (!is_host || peer->port >= 0) {
                    pending_events.push_back(std::move(event));
                }
                peers.erase(peers.begin() + (peer - peers.data()));
                break;
            }
            default:
                break;
        }
    }

    std::vector<Event> Session::poll() {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        std::vector<Event> signal_events;
        take_signals(signal_events);
        sockets->RunCallbacks();
        std::vector<Event> events = std::move(pending_events);
        pending_events.clear();
        events.insert(events.end(), signal_events.begin(), signal_events.end());

        if (poll_group != k_HSteamNetPollGroup_Invalid) {
            SteamNetworkingMessage_t* messages[32];
            int count;
            int taken = 0;
            while (taken < max_messages_per_poll && (count = sockets->ReceiveMessagesOnPollGroup(poll_group, messages, 32)) > 0) {
                taken += count;
                for (int i = 0; i < count; i++) {
                    SteamNetworkingMessage_t* message = messages[i];
                    take(message->m_conn, { (const uint8_t*)message->m_pData, (size_t)message->m_cbSize }, events);
                    message->Release();
                }
            }
        }

        // The waiting room's roster, to everyone in: at once when someone comes, goes or gets ready, and every 2 s
        // for the round trips.
        const double t = now();
        if (is_host && entry_open) {
            Roster now_roster = roster();
            Roster without_trips = now_roster, sent_without = sent_roster;
            for (int port = 0; port < max_players; port++) {
                without_trips.seats[port].round_trip_ms = sent_without.seats[port].round_trip_ms = 0;
            }
            if (!(without_trips == sent_without) || t >= next_roster) {
                for (const Peer& peer : peers) {
                    if (peer.port >= 0) send_to(peer.connection, now_roster);
                }
                sent_roster = now_roster;
                next_roster = t + 2.0;
            }
        }
        // Joiners that never said Hello.
        for (size_t i = 0; i < peers.size();) {
            if (is_host && peers[i].port < 0 && peers[i].up && t - peers[i].connected_at > handshake_seconds) {
                drop(peers[i], Reason::timeout, &events);
            }
            else {
                i++;
            }
        }
        return events;
    }

    void Session::take(uint32_t connection, std::span<const uint8_t> data, std::vector<Event>& events) {
        Peer* peer = find(connection);
        if (peer == nullptr) {
            return;
        }
        std::optional<Packet> packet = decode(data);
        if (packet) {
            handle(*peer, *packet, events);
            return;
        }
        // Another protocol version is told so; anything else is a broken packet.
        const bool other_version = data.size() >= 4 && data[0] == 'C' && data[1] == 'N' &&
            (uint16_t)(data[2] | (data[3] << 8)) != protocol_version;
        if (other_version && !is_host && peer->port < 0) {
            // A host of another version refusing this joiner (its Refuse is kept the same): why, and its release.
            if (std::optional<Refuse> refusal = decode_refusal(data)) {
                refusal->reason = Reason::version;
                handle(*peer, *refusal, events);
                return;
            }
        }
        if (other_version && is_host && peer->port < 0) {
            // A joiner of another version: refused as any other, so it reads which release this is.
            refuse(*peer, Reason::version);
            return;
        }
        drop(*peer, other_version ? Reason::version : Reason::protocol, &events);
    }

    void Session::drain(uint32_t connection, std::vector<Event>& events) {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        SteamNetworkingMessage_t* messages[32];
        int count;
        // At most what the library buffers from a peer (recv_buffer_messages), and only while it's a peer.
        for (int taken = 0; taken < recv_buffer_messages && find(connection) != nullptr &&
            (count = sockets->ReceiveMessagesOnConnection(connection, messages, 32)) > 0; taken += count) {
            for (int i = 0; i < count; i++) {
                take(connection, { (const uint8_t*)messages[i]->m_pData, (size_t)messages[i]->m_cbSize }, events);
                messages[i]->Release();
            }
        }
    }

    void Session::handle(Peer& peer, const Packet& packet, std::vector<Event>& events) {
        if (is_host) {
            if (peer.port < 0) {
                const Hello* theirs = std::get_if<Hello>(&packet);
                if (theirs == nullptr) {
                    drop(peer, Reason::protocol, &events);
                    return;
                }
                // The key first, and alone: a wrong one is closed at once, told nothing else (whether its
                // build or ROM would have done).
                if (!same_key(theirs->key, hello.key)) {
                    SteamNetworkingSockets()->CloseConnection(peer.connection, k_ESteamNetConnectionEnd_App_Min + (int)Reason::key,
                        describe(Reason::key), false);
                    peers.erase(peers.begin() + (&peer - peers.data()));
                    return;
                }
                std::optional<Reason> refusal;
                if (theirs->build != hello.build) refusal = Reason::build;
                else if (theirs->rom != hello.rom) refusal = Reason::rom;
                else if (theirs->profile != hello.profile) refusal = Reason::profile;
                else if (theirs->mods != hello.mods) refusal = Reason::mods;
                else if (!entry_open) refusal = Reason::started;
                // Kept out (the host's block list), or posing as the host itself: told only that the host removed it.
                else if (std::find(blocked.begin(), blocked.end(), theirs->player) != blocked.end() || theirs->player == hello.player) {
                    refusal = Reason::kicked;
                }
                // The lowest free ports, one for each of its players.
                uint8_t ports = 0;
                if (!refusal) {
                    uint8_t taken = own_ports;
                    for (const Peer& other : peers) {
                        taken |= other.ports;
                    }
                    for (int p = 0, wanted = theirs->players; p < match_players && wanted > 0; p++) {
                        if ((taken & (1u << p)) == 0) {
                            ports |= (uint8_t)(1u << p);
                            wanted--;
                        }
                    }
                    if (std::popcount(ports) != theirs->players) refusal = Reason::full;
                }
                if (refusal) {
                    refuse(peer, *refusal);
                    return;
                }
                peer.port = std::countr_zero(ports);
                peer.ports = ports;
                peer.player = theirs->player;
                peer.platform = theirs->platform;
                send_to(peer.connection, Welcome{ ports, (uint8_t)match_players });
                Event event{ Event::Type::joined };
                event.port = peer.port;
                event.ports = ports;
                event.players = match_players;
                events.push_back(std::move(event));
                return;
            }
            // Ready, only before the start.
            if (const Ready* ready = std::get_if<Ready>(&packet)) {
                // One the joiner sent as the match started (Start on its way to it) is too late, not wrong.
                if (entry_open) {
                    peer.ready = ready->ready != 0;
                }
                return;
            }
            // Inputs and hashes only once the match has started (before, a player can still leave and
            // another take the port), and Inputs no further ahead than the game can have got (set_horizon):
            // anything else is a broken or hostile peer, refused before it's passed on.
            const Inputs* inputs = std::get_if<Inputs>(&packet);
            const Hash* hash = std::get_if<Hash>(&packet);
            // Each port's Inputs go on from the last read of its earlier ones, and a Hash's read is within the
            // horizon too: else the others would end the match on it (lockstep checks the same), where dropping
            // the peer lets them play on without it.
            if ((inputs != nullptr || hash != nullptr) &&
                (entry_open || (inputs != nullptr && ((uint64_t)inputs->first_read + inputs->count > horizon ||
                    inputs->first_read != next_inputs[inputs->port])) || (hash != nullptr && hash->read > horizon))) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            // One hash per read, in order: a repeat is a broken or hostile peer.
            if (hash != nullptr) {
                if ((int64_t)hash->read <= peer.last_hash) {
                    drop(peer, Reason::protocol, &events);
                    return;
                }
                peer.last_hash = hash->read;
            }
            if (inputs != nullptr && (peer.ports & (1u << inputs->port)) != 0) {
                next_inputs[inputs->port] = inputs->first_read + inputs->count;
                // Passed on to the others, as they only connect to the host.
                for (const Peer& other : peers) {
                    if (other.port >= 0 && other.connection != peer.connection) {
                        send_to(other.connection, *inputs);
                    }
                }
            }
            else if (const Bye* bye = std::get_if<Bye>(&packet)) {
                Event event{ Event::Type::left };
                event.port = peer.port;
                event.ports = peer.ports;
                event.reason = bye->reason;
                SteamNetworkingSockets()->CloseConnection(peer.connection, 0, nullptr, false);
                peers.erase(peers.begin() + (&peer - peers.data()));
                events.push_back(std::move(event));
                return;
            }
            else if (!std::holds_alternative<Hash>(packet)) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = peer.port;
            event.packet = packet;
            events.push_back(std::move(event));
            return;
        }

        // A joiner, from the host.
        if (own_ports == 0) {
            const Welcome* welcome = std::get_if<Welcome>(&packet);
            if (welcome != nullptr && std::popcount(welcome->ports) == hello.players) {
                own_ports = welcome->ports;
                match_players = welcome->players;
                Event event{ Event::Type::welcomed };
                event.port = std::countr_zero(own_ports);
                event.ports = own_ports;
                event.players = match_players;
                events.push_back(std::move(event));
            }
            else if (std::holds_alternative<Refuse>(packet) || std::holds_alternative<Bye>(packet)) {
                // Refused, or dropped before being welcomed (the host's handshake time ran out): with why.
                Event event{ Event::Type::ended };
                event.reason = std::holds_alternative<Refuse>(packet) ? std::get<Refuse>(packet).reason : std::get<Bye>(packet).reason;
                if (const Refuse* refusal = std::get_if<Refuse>(&packet)) {
                    event.release = refusal->release;
                }
                SteamNetworkingSockets()->CloseConnection(peer.connection, 0, nullptr, false);
                peers.clear();
                events.push_back(std::move(event));
            }
            else {
                drop(peer, Reason::protocol, &events);
            }
            return;
        }
        if (const Roster* roster = std::get_if<Roster>(&packet)) {
            // The waiting room's, before the start, with this machine's ports in it.
            bool mine = true;
            for (int port = 0; port < max_players; port++) {
                mine = mine && ((own_ports & (1u << port)) == 0 || roster->seats[port].player == hello.player);
            }
            if (started || roster->players != match_players || !mine) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            host_roster = *roster;
            return;
        }
        if (const Start* start = std::get_if<Start>(&packet)) {
            if (started || start->players != match_players) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            started = true;
            Event event{ Event::Type::started };
            event.own_view = start->own_view != 0;
            event.delay = start->delay;
            event.pairs = start->pairs;
            event.rollback = start->rollback;
            events.push_back(std::move(event));
        }
        else if (const Inputs* inputs = std::get_if<Inputs>(&packet); inputs != nullptr) {
            if (!started || (own_ports & (1u << inputs->port)) != 0 || inputs->port >= match_players ||
                (uint64_t)inputs->first_read + inputs->count > horizon) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = inputs->port;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (const Hash* hash = std::get_if<Hash>(&packet); hash != nullptr) {
            // Once started, one per read, in order.
            if (!started || (int64_t)hash->read <= peer.last_hash) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            peer.last_hash = hash->read;
            Event event{ Event::Type::packet };
            event.port = 0;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (const Gone* gone = std::get_if<Gone>(&packet); gone != nullptr) {
            // Once started, about another machine's player.
            if (!started || (own_ports & (1u << gone->port)) != 0 || gone->port >= match_players) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = gone->port;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (std::holds_alternative<Delay>(packet)) {
            // Once started.
            if (!started) {
                drop(peer, Reason::protocol, &events);
                return;
            }
            Event event{ Event::Type::packet };
            event.port = 0;
            event.packet = packet;
            events.push_back(std::move(event));
        }
        else if (const Bye* bye = std::get_if<Bye>(&packet)) {
            Event event{ Event::Type::ended };
            event.reason = bye->reason;
            SteamNetworkingSockets()->CloseConnection(peer.connection, 0, nullptr, false);
            peers.clear();
            events.push_back(std::move(event));
        }
        else {
            drop(peer, Reason::protocol, &events);
        }
    }

    void Session::flush() {
        // Long enough for a few resends at a bad network's round trip; a peer that's gone isn't waited for.
        constexpr double flush_seconds = 2.0;
        const double until = now() + flush_seconds;
        while (now() < until) {
            bool pending = false;
            for (const Peer& peer : peers) {
                SteamNetConnectionRealTimeStatus_t status;
                if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(peer.connection, &status, 0, nullptr) == k_EResultOK &&
                    status.m_eState == k_ESteamNetworkingConnectionState_Connected &&
                    status.m_cbPendingReliable + status.m_cbSentUnackedReliable > 0) {
                    pending = true;
                }
            }
            if (!pending) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void Session::set_release(const std::string& name) {
        const bool valid = name.size() <= max_release_name &&
            std::all_of(name.begin(), name.end(), [](char c) { return c > ' ' && c <= '~'; });
        release = valid ? name : std::string();
    }

    void Session::refuse(Peer& peer, Reason reason) {
        send_to(peer.connection, Refuse{ reason, release });
        SteamNetworkingSockets()->CloseConnection(peer.connection, k_ESteamNetConnectionEnd_App_Min + (int)reason,
            describe(reason), true);
        peers.erase(peers.begin() + (&peer - peers.data()));
    }

    void Session::drop(Peer& peer, Reason reason, std::vector<Event>* events) {
        send_to(peer.connection, Bye{ reason });
        SteamNetworkingSockets()->CloseConnection(peer.connection, k_ESteamNetConnectionEnd_App_Min + (int)reason,
            describe(reason), true);
        if (events != nullptr && (!is_host || peer.port >= 0)) {
            Event event{ is_host ? Event::Type::left : Event::Type::ended };
            event.port = peer.port;
            event.ports = peer.ports;
            event.reason = reason;
            events->push_back(std::move(event));
        }
        peers.erase(peers.begin() + (&peer - peers.data()));
    }

    // Everything is sent reliably (resent until it arrives, in order), and at once: lockstep needs every
    // input, and waits for it anyway.
    void Session::send_to(uint32_t connection, const Packet& packet) {
        uint8_t buffer[max_packet_size];
        const size_t size = encode(packet, buffer);
        if (size == 0) {
            std::fprintf(stderr, "[netplay] not sending an invalid packet\n");
            return;
        }
        SteamNetworkingSockets()->SendMessageToConnection(connection, buffer, (uint32)size,
            k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
    }

    void Session::send(const Inputs& inputs) {
        for (const Peer& peer : peers) {
            if (peer.port >= 0) {
                send_to(peer.connection, inputs);
            }
        }
    }

    void Session::send(const Hash& hash) {
        for (const Peer& peer : peers) {
            if (peer.port >= 0) {
                send_to(peer.connection, hash);
            }
        }
    }

    void Session::send(const Gone& gone) {
        for (const Peer& peer : peers) {
            if (peer.port >= 0) {
                send_to(peer.connection, gone);
            }
        }
    }

    void Session::send(const Delay& delay) {
        for (const Peer& peer : peers) {
            if (peer.port >= 0) {
                send_to(peer.connection, delay);
            }
        }
    }

    void Session::leave(Reason reason) {
        // The goodbyes first, then a moment for everything sent to arrive, as on a lossy network some of it is
        // still being resent: the program quitting would drop what isn't acknowledged yet (a quitting player's
        // last inputs and goodbye among it), and the others would wait for the connection to time out.
        for (const Peer& peer : peers) {
            send_to(peer.connection, Bye{ reason });
        }
        flush();
        while (!peers.empty()) {
            SteamNetworkingSockets()->CloseConnection(peers.back().connection, k_ESteamNetConnectionEnd_App_Min + (int)reason,
                describe(reason), true);
            peers.pop_back();
        }
        // The listen socket stays open (nobody more is let in) until the session goes: closing it closes
        // every connection accepted on it at once, goodbyes not yet sent and all.
        entry_open = false;
    }
}
