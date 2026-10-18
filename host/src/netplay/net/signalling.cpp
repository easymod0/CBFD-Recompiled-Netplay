// A peer's side of signalling: see signalling.hpp.

#include "signalling.hpp"
#include "session.hpp"

#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

namespace conker::netplay {
    namespace {
        // Signals waiting to be sent, at most: the library sends a few a second per connection, so more
        // means the server is gone or slow, and the oldest are dropped (the library resends what it needs).
        constexpr size_t max_queued = 64;
        // The most packets one poll takes from the server.
        constexpr int max_messages_per_poll = 64;

        Signaller* current = nullptr;
    }

    void signaller_status_changed(void* info) {
        if (current != nullptr) {
            current->on_status_changed(info);
        }
    }

    namespace {
        void status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
            signaller_status_changed(info);
        }
    }

    std::string resolve_address(const std::string& host_port, std::string& error) {
        // A literal address as it is.
        SteamNetworkingIPAddr parsed;
        if (parsed.ParseString(host_port.c_str()) && parsed.m_port != 0) {
            char out[SteamNetworkingIPAddr::k_cchMaxString];
            parsed.ToString(out, sizeof(out), true);
            return out;
        }
        const size_t colon = host_port.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 == host_port.size()) {
            error = "\"" + host_port + "\" isn't a server's name and port";
            return {};
        }
        const std::string host = host_port.substr(0, colon);
        const std::string port = host_port.substr(colon + 1);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* results = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &results) != 0 || results == nullptr) {
            error = "couldn't find the server \"" + host + "\"";
            return {};
        }
        // The first IPv4 address, else the first IPv6 one.
        SteamNetworkingIPAddr address;
        address.Clear();
        for (int want_v4 = 1; want_v4 >= 0 && address.IsIPv6AllZeros(); want_v4--) {
            for (addrinfo* r = results; r != nullptr; r = r->ai_next) {
                if (want_v4 && r->ai_family == AF_INET) {
                    const auto* in = (const sockaddr_in*)r->ai_addr;
                    address.SetIPv4(ntohl(in->sin_addr.s_addr), ntohs(in->sin_port));
                    break;
                }
                if (!want_v4 && r->ai_family == AF_INET6) {
                    const auto* in6 = (const sockaddr_in6*)r->ai_addr;
                    address.SetIPv6(in6->sin6_addr.s6_addr, ntohs(in6->sin6_port));
                    break;
                }
            }
        }
        freeaddrinfo(results);
        if (address.IsIPv6AllZeros()) {
            error = "couldn't find the server \"" + host + "\"";
            return {};
        }
        char out[SteamNetworkingIPAddr::k_cchMaxString];
        address.ToString(out, sizeof(out), true);
        return out;
    }

    Signaller::Signaller() {
        current = this;
    }

    Signaller::~Signaller() {
        close();
        current = nullptr;
    }

    bool Signaller::connect(const std::string& server, std::string& error) {
        const std::string address_text = resolve_address(server, error);
        if (address_text.empty()) {
            return false;
        }
        SteamNetworkingIPAddr address;
        address.ParseString(address_text.c_str());
        SteamNetworkingConfigValue_t options[4];
        options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)status_changed);
        options[1].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, (int32_t)signal::max_packet_size);
        options[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages, 256);
        options[3].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, 256 * (int32_t)signal::max_packet_size);
        connection = SteamNetworkingSockets()->ConnectByIPAddress(address, 4, options);
        if (connection == k_HSteamNetConnection_Invalid) {
            error = "couldn't connect to the server";
            return false;
        }
        current_state = State::connecting;
        failure.clear();
        return true;
    }

    void Signaller::fail(const std::string& why) {
        if (connection != k_HSteamNetConnection_Invalid) {
            SteamNetworkingSockets()->CloseConnection(connection, 0, nullptr, false);
            connection = k_HSteamNetConnection_Invalid;
        }
        current_state = State::failed;
        failure = why;
    }

    void Signaller::on_status_changed(const void* data) {
        const auto* info = (const SteamNetConnectionStatusChangedCallback_t*)data;
        if (info->m_hConn != connection || connection == k_HSteamNetConnection_Invalid) {
            return;
        }
        switch (info->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_Connected: {
                uint8_t buffer[signal::max_packet_size];
                const size_t size = signal::encode(signal::Register{ local_id() }, buffer);
                SteamNetworkingSockets()->SendMessageToConnection(connection, buffer, (uint32)size, k_nSteamNetworkingSend_Reliable, nullptr);
                current_state = State::ready;
                break;
            }
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                std::string why = info->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally
                    ? "couldn't reach the server" : "the server closed the connection";
                std::string detail;
                for (const char* c = info->m_info.m_szEndDebug; *c != '\0'; c++) {
                    detail += (*c >= 0x20 && *c < 0x7F) ? *c : '?';
                }
                fail(detail.empty() ? why : why + " (" + detail + ")");
                break;
            }
            default:
                break;
        }
    }

    void Signaller::poll() {
        ISteamNetworkingSockets* sockets = SteamNetworkingSockets();
        sockets->RunCallbacks();
        if (current_state != State::ready) {
            return;
        }

        const std::vector<signal::Signal> sending = out->take();
        uint8_t buffer[signal::max_packet_size];
        for (const signal::Signal& s : sending) {
            const size_t size = signal::encode(s, buffer);
            if (size != 0) {
                sockets->SendMessageToConnection(connection, buffer, (uint32)size, k_nSteamNetworkingSend_Reliable, nullptr);
            }
        }

        SteamNetworkingMessage_t* messages[16];
        int count;
        for (int taken = 0; current_state == State::ready && taken < max_messages_per_poll &&
            (count = sockets->ReceiveMessagesOnConnection(connection, messages, 16)) > 0; taken += count) {
            for (int i = 0; i < count; i++) {
                const std::span<const uint8_t> data{ (const uint8_t*)messages[i]->m_pData, (size_t)messages[i]->m_cbSize };
                const std::optional<signal::Packet> packet = signal::decode(data);
                messages[i]->Release();
                if (current_state != State::ready) {
                    continue;
                }
                if (!packet) {
                    fail(signal::other_version(data) ? "the server runs another version" : "the server sent a broken packet");
                }
                else if (const signal::Signal* s = std::get_if<signal::Signal>(&*packet)) {
                    incoming.push_back(*s);
                }
                else if (const signal::Unreachable* u = std::get_if<signal::Unreachable>(&*packet)) {
                    gone.push_back(u->peer);
                }
                else if (const signal::Refused* r = std::get_if<signal::Refused>(&*packet)) {
                    switch (r->reason) {
                        case signal::Refusal::version: fail("the server runs another version"); break;
                        case signal::Refusal::taken: fail("the server refused this id (try again)"); break;
                        case signal::Refusal::busy: fail("the server is busy"); break;
                        default: fail("the server refused a packet"); break;
                    }
                }
                else {
                    fail("the server sent a packet a server doesn't");
                }
            }
        }
    }

    void SignalOutbox::push(uint64_t peer, std::span<const uint8_t> data) {
        if (peer == 0 || data.empty() || data.size() > signal::max_signal) {
            return;
        }
        std::lock_guard lock(mutex);
        if (queue.size() >= max_queued) {
            queue.erase(queue.begin());
        }
        queue.push_back({ peer, { data.begin(), data.end() } });
    }

    std::vector<signal::Signal> SignalOutbox::take() {
        std::lock_guard lock(mutex);
        return std::exchange(queue, {});
    }

    std::vector<signal::Signal> Signaller::received() {
        return std::exchange(incoming, {});
    }

    std::vector<uint64_t> Signaller::unreachable() {
        return std::exchange(gone, {});
    }

    void Signaller::close() {
        if (connection != k_HSteamNetConnection_Invalid) {
            SteamNetworkingSockets()->CloseConnection(connection, 0, nullptr, true);
            connection = k_HSteamNetConnection_Invalid;
        }
        if (current_state != State::failed) {
            current_state = State::idle;
        }
    }
}
