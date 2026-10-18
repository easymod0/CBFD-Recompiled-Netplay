// A peer's side of signalling: one connection to the signalling server (signal_server.cpp), over
// GameNetworkingSockets like everything else, that registers this process's id (session.hpp: local_id())
// and passes the library's ICE signals between it and other peers by their ids (signal_packets.hpp).
// Sessions that connect through ICE (Session::host or join with a Signaller) use it to find a way to each
// other: directly through NAT, or through a TURN relay so neither sees the other's address.
//
// Through it, a host creates a room for its match and gets the room code to pass on (join_code.hpp), and a
// joiner looks the code up to learn the host's id and the match's key. The server may offer its TURN relay
// too, with credentials for this process: ice() makes the session's Ice from them.
//
// A host keeps it connected for as long as its match lasts, so that its room stays open and someone can
// still reach it then (a player rejoining, later).
//
// Single-threaded like the session, except the outbox: the library sends signals from its own thread, so
// they're only queued, and poll() sends them.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "session.hpp"
#include "signal_packets.hpp"

namespace conker::netplay {
    // Signals waiting to be sent to the server. The library's connections hold it too (session.cpp), as it
    // may still send a signal for a connection after the session and the signaller have gone: then it's
    // dropped with the outbox, once the last of them lets go.
    struct SignalOutbox {
        // Queues a signal for `peer` (any thread): at most max_queued, dropping the oldest (the library
        // resends what it needs).
        void push(uint64_t peer, std::span<const uint8_t> data);
        std::vector<signal::Signal> take();
    private:
        std::mutex mutex;
        std::vector<signal::Signal> queue;
    };

    // "host:port" (a name, IPv4 or [IPv6]) to an address the library takes ("1.2.3.4:5000" or "[::1]:5000"),
    // resolving a name; or empty, with `error`.
    std::string resolve_address(const std::string& host_port, std::string& error);

    class Signaller {
    public:
        Signaller();
        ~Signaller();
        Signaller(const Signaller&) = delete;
        Signaller& operator=(const Signaller&) = delete;

        // Connects to the signalling server at `server` ("host:port") and registers this process's id
        // (the library must be started).
        bool connect(const std::string& server, std::string& error);

        // ready: the server has taken this process's id (Registered), so others can reach it.
        enum class State { idle, connecting, ready, failed };
        State state() const { return current_state; }
        // Why it failed (or the connection ended), for the player.
        const std::string& error() const { return failure; }

        // Runs the library's callbacks, sends what was queued and takes what arrived. Call it often.
        void poll();
        // Queues a signal for `peer` (any thread); sent at the next poll once registered.
        void send(uint64_t peer, std::span<const uint8_t> data) { out->push(peer, data); }
        // Where the library's connections queue their signals.
        std::shared_ptr<SignalOutbox> outbox() const { return out; }
        // The signals that have arrived since the last call, from their senders' ids.
        std::vector<signal::Signal> received();
        // The peers the server said aren't there, since the last call.
        std::vector<uint64_t> unreachable();

        // The TURN relay the server offered as this process registered, with its credentials (none: it
        // has none).
        const std::optional<signal::Turn>& turn() const { return offered_turn; }
        // The Ice for sessions through this server: its relay, and only that if `relay_only`; otherwise
        // directly too (with the relay's server for STUN, to learn this machine's public address).
        Ice ice(bool relay_only) const;

        // Once ready: creates a room for this process's match, whose key is `key` (one each); room() is
        // its number once the server has made it (0 until then; join_code.hpp: encode_room_code).
        void create_room(uint64_t key);
        uint64_t room() const { return created_room; }
        // Once ready: looks up `room`'s host. lookup() says how it went, and found() what it found.
        enum class Lookup { none, waiting, found, missing };
        void find_room(uint64_t room);
        Lookup lookup() const { return room_lookup; }
        const signal::RoomFound& found() const { return found_room; }

        // Closes the connection.
        void close();

    private:
        void on_status_changed(const void* info);
        void fail(const std::string& why);
        void send_now(const signal::Packet& packet);
        friend void signaller_status_changed(void* info);

        uint32_t connection = 0;
        State current_state = State::idle;
        bool registering = false;   // connected, Register sent, Registered not here yet
        std::string failure;
        std::shared_ptr<SignalOutbox> out = std::make_shared<SignalOutbox>();
        std::vector<signal::Signal> incoming;
        std::vector<uint64_t> gone;
        std::optional<signal::Turn> offered_turn;
        bool creating_room = false;     // CreateRoom sent, RoomCreated not here yet
        uint64_t asked_room = 0;        // the room looked up
        uint64_t created_room = 0;
        Lookup room_lookup = Lookup::none;
        signal::RoomFound found_room;
    };
}
