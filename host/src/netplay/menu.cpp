// The launcher's Netplay menu: host a match or join one by its code, wait for
// every player there, then the game starts on every machine at once. It's a modal of its own over the
// launcher; the connecting is lockstep's (lockstep.hpp), a step at each of the menu's updates, so a
// refusal (another build, ROM or settings, a full match) is said here and the player can try again.
//
// A match started from here doesn't quit when it ends (a desync, the host leaving): the game stops taking
// input and a prompt says why, with Quit.
//
// By default a match goes through a room on a netplay server (tools/netplay/server/): the server gives the
// host a room code, with no address in it, and the players reach each other through its relay, so nobody
// sees another's address (unless direct connections are allowed) and no port needs forwarding. The server's
// address and that choice are settings of their own, kept in netplay.json in the config folder.
//
// A room can be public (this fork's): the server lists it, under a name made from its number, for players
// whose build, ROM, settings and mods are the same, on platforms that play together. The Join tab shows
// that list while it's open, asked for again every few seconds, and joins a room from it. Quick Match joins
// the first open one that fits, or hosts one in public with the Host tab's settings (lockstep's).
//
// A match hosted directly can be visible on this network: games there list it on their Join tab (heard
// from its beacon: lan.hpp) and join it with no code and no server.
//
// Or directly: the host's join code is made, once hosting, from the address the host gives and the match's
// random key: the game can't know its public address without a server to ask, so the host looks it up (or
// gives its local one, for a match at home). The code is the host's address, so the menu says to share it
// only with people the host trusts. The input delay is lockstep's default, the same on every machine.
//
// Rollback (--rollback) is the host's choice too, the same for everyone in the match.

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <SDL.h>

#include "recompui/recompui.h"
#include "elements/ui_button.h"
#include "elements/ui_container.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"
#include "elements/ui_radio.h"
#include "elements/ui_scroll_container.h"
#include "elements/ui_tab_set.h"
#include "elements/ui_text_input.h"
#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "../conker.hpp"
#include "hello.hpp"
#include "lockstep.hpp"
#include "net/join_code.hpp"
#include "net/session.hpp"

namespace {
    constexpr int default_port = 27015;
    // The netplay server players use unless they set another.
    constexpr const char* default_server = "conker.jesp.ca:27100";

    // The server settings, kept in netplay.json in the config folder.
    struct ServerSettings {
        std::string server = default_server;
        bool allow_direct = false;
    };

    std::filesystem::path settings_path() {
        return recomp::get_config_path() / "netplay.json";
    }

    ServerSettings load_settings() {
        ServerSettings settings;
        std::ifstream file(settings_path());
        const nlohmann::json json = file ? nlohmann::json::parse(file, nullptr, false) : nlohmann::json();
        if (json.is_object()) {
            if (json.contains("server") && json["server"].is_string()) settings.server = json["server"].get<std::string>();
            if (json.contains("allow_direct") && json["allow_direct"].is_boolean()) settings.allow_direct = json["allow_direct"].get<bool>();
        }
        return settings;
    }

    void save_settings(const ServerSettings& settings) {
        std::ofstream file(settings_path(), std::ios::trunc);
        if (file) {
            file << nlohmann::json{ { "server", settings.server }, { "allow_direct", settings.allow_direct } }.dump(4) << "\n";
        }
    }

    std::string trimmed(std::string text) {
        while (!text.empty() && text.back() == ' ') text.pop_back();
        while (!text.empty() && text.front() == ' ') text.erase(text.begin());
        return text;
    }

    std::u8string game_id;
    std::filesystem::path stored_rom;
    recompui::Modal* modal = nullptr;

    // From the game thread (the harness), for the main thread: why the match is over.
    std::mutex over_mutex;
    std::string over_text;
    bool over_pending = false;

    class NetplayPage : public recompui::Element {
        enum class State { idle, connecting, starting };
        State state = State::idle;
        recompui::TextInput* port_input = nullptr;
        recompui::TextInput* address_input = nullptr;
        recompui::TextInput* join_input = nullptr;
        recompui::Label* code_label = nullptr;
        recompui::Button* copy_button = nullptr;
        std::string shown_code;             // the join code on show, for Copy
        recompui::Element* status_row = nullptr;   // hidden while there's nothing to say
        recompui::Label* status_label = nullptr;
        recompui::Element* direct_rows = nullptr;  // hosting directly: the port and address
        recompui::Element* room_row = nullptr;     // hosting through a room: private or public
        recompui::Element* rooms_box = nullptr;    // the public rooms, a row each
        recompui::Label* rooms_label = nullptr;    // how the list goes, or what it shows
        std::string shown_rooms;                   // what rooms_box shows, to make it again only when that changes
        bool browsing = false;                     // the Join tab is open: the public list is polled
        recompui::Element* lan_box = nullptr;      // the matches on this network, a row each
        recompui::Label* lan_label = nullptr;
        std::string shown_lan;
        bool lan_listening = false;                // the Join tab is open: this network's matches are polled
        std::chrono::steady_clock::time_point lan_since;   // listening since
        bool lan_shown = false;                    // the list (or that it's empty) is on show
        bool lan_visible = false;                  // hosting directly: visible on this network
        bool join_tab = false;                     // the Join tab is the one open
        bool closing = false;                      // the menu is closing: no room list
        std::optional<conker::netplay::Hello> browse_hello;   // the room list's, made once each time the menu opens
        recompui::Element* host_section = nullptr; // one at a time, by the Host and Join tabs
        recompui::Element* join_section = nullptr;
        recompui::Button* host_button = nullptr;
        recompui::Button* join_button = nullptr;
        recompui::Button* cancel_button = nullptr;
        int players = 2;
        int local_players = 1;              // the players on this computer, hosting or joining
        bool hosting = false;               // a match is hosted (connecting, as the host)
        bool host_directly = false;         // hosting: by a join code (a port), not a room on the server
        bool public_room = false;           // hosting through a room: in the server's public list
        ServerSettings settings = load_settings();
        bool own_view_for_everyone = true;  // hosting: the match's screens rule
        int input_delay = -1;               // hosting: reads, or -1 for auto
        int rollback = 0;                   // hosting: reads predicted, 0 (off) to 12
        std::string port_text = std::to_string(default_port);
        std::string address_text;
        std::string join_text;

        recompui::Label* text(recompui::Element* parent, const std::string& words, recompui::theme::Typography typography,
            recompui::theme::color color = recompui::theme::color::Text) {
            recompui::Label* label = recompui::get_current_context().create_element<recompui::Label>(parent, words, typography);
            label->set_color(color);
            label->set_flex_shrink(0.0f);
            return label;
        }

        recompui::Element* column(recompui::Element* parent, float gap) {
            recompui::Element* box = recompui::get_current_context().create_element<recompui::Element>(parent);
            box->set_display(recompui::Display::Flex);
            box->set_flex_direction(recompui::FlexDirection::Column);
            box->set_gap(gap);
            box->set_width(100.0f, recompui::Unit::Percent);
            box->set_align_items(recompui::AlignItems::FlexStart);
            box->set_flex_shrink(0.0f);
            return box;
        }

        recompui::Element* row(recompui::Element* parent) {
            recompui::Element* box = recompui::get_current_context().create_element<recompui::Element>(parent);
            box->set_display(recompui::Display::Flex);
            box->set_flex_direction(recompui::FlexDirection::Row);
            box->set_align_items(recompui::AlignItems::Center);
            box->set_gap(16.0f);
            box->set_flex_shrink(0.0f);
            return box;
        }

        recompui::TextInput* input(recompui::Element* parent, const std::string& value, float width) {
            recompui::TextInput* field = recompui::get_current_context().create_element<recompui::TextInput>(parent);
            field->set_width(width);
            field->set_min_width(width);
            field->set_flex_shrink(0.0f);
            field->set_text(value);
            return field;
        }

        // The room code, or the join code (it holds the match's key, new each time), once hosting.
        void show_code() {
            const bool blank = address_text.find_first_not_of(' ') == std::string::npos;
            const std::string code = !hosting ? std::string() :
                host_directly ? conker::lockstep::join_code(address_text) : conker::lockstep::room_code();
            if (code != shown_code) {
                copy_button->set_text("Copy");
            }
            shown_code = code;
            copy_button->set_display(code.empty() ? recompui::Display::None : recompui::Display::Flex);
            if (!host_directly) {
                code_label->set_text(!hosting ? "Press Host: the room code to send the others appears here." :
                    code.empty() ? "Making a room..." : "Room code: " + code);
            }
            else if (blank && lan_visible) {
                code_label->set_text(hosting ? "Visible on this network: players there find it on their Join tab." :
                    "Visible on this network (enter your address for a join code too).");
            }
            else if (blank) {
                code_label->set_text("Enter your address to get a join code.");
            }
            else if (!hosting) {
                code_label->set_text("Press Host: the join code to send the others appears here.");
            }
            else if (code.empty()) {
                code_label->set_text("That isn't an address: four numbers such as 203.0.113.7, or an IPv6 one.");
            }
            else {
                code_label->set_text("Join code: " + code);
            }
        }

        void set_state(State next, const std::string& status) {
            state = next;
            if (state == State::idle) {
                hosting = false;
            }
            show_code();
            status_label->set_text(status);
            status_row->set_display(status.empty() && state != State::connecting ? recompui::Display::None : recompui::Display::Flex);
            host_button->set_enabled(state == State::idle);
            join_button->set_enabled(state == State::idle);
            cancel_button->set_display(state == State::connecting ? recompui::Display::Flex : recompui::Display::None);
            // The room list only while nothing is being connected (it's asked for again once it's idle).
            rooms_box->set_display(state == State::idle ? recompui::Display::Flex : recompui::Display::None);
            lan_box->set_display(state == State::idle ? recompui::Display::Flex : recompui::Display::None);
            if (state == State::idle && join_tab && !browsing && !closing) {
                start_browsing();
            }
            // An update applies the text (and, while connecting, polls).
            queue_update();
        }

        // This game's Hello, from the stored ROM (false: there's none yet).
        bool my_hello(conker::netplay::Hello& hello) {
            std::ifstream file(stored_rom, std::ios::binary);
            const std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (rom.empty()) {
                return false;
            }
            hello = conker::netplay::hello_for_rom(rom);
            return true;
        }

        // Hosting or joining: the Hello from the stored ROM, then lockstep's begin.
        void begin() {
            stop_browsing();
            conker::netplay::Hello hello;
            if (!my_hello(hello)) {
                set_state(State::idle, "Load the ROM first (Start Game on the launcher).");
                return;
            }
            std::string error;
            if (!conker::lockstep::begin(hello, error)) {
                set_state(State::idle, "Couldn't: " + error);
                return;
            }
            set_state(State::connecting, "Connecting...");
        }

        // Through the server (hosting a room public if `listed`): false (and says why) if none is set.
        bool use_server(bool listed = false) {
            settings.server = trimmed(settings.server);
            if (settings.server.empty()) {
                set_state(State::idle, "Set a netplay server first (Server, above).");
                return false;
            }
            save_settings(settings);
            conker::lockstep::set_server(settings.server, settings.allow_direct, listed);
            return true;
        }

        // The public room list: asked for while the Join tab is open and nothing is being connected.
        void start_browsing() {
            stop_browsing();
            shown_rooms.clear();
            rooms_box->clear_children();
            settings.server = trimmed(settings.server);
            // Hashing the ROM and the mods takes a moment: once each time the menu opens (the mods can only
            // change while it's closed).
            conker::netplay::Hello hello;
            if (!browse_hello && my_hello(hello)) {
                browse_hello = hello;
            }
            std::string error;
            // This network's matches need no server.
            stop_lan();
            shown_lan.clear();
            lan_box->clear_children();
            if (!browse_hello) {
                lan_label->set_text("");
            }
            else if (!conker::lockstep::lan_browse(*browse_hello, error)) {
                lan_label->set_text("Can't look for matches on this network: " + error + ".");
            }
            else {
                lan_label->set_text("Listening for matches on this network...");
                lan_listening = true;
                lan_since = std::chrono::steady_clock::now();
                lan_shown = false;
            }
            if (settings.server.empty()) {
                rooms_label->set_text("Set a netplay server (Server, above) to see its public rooms.");
            }
            else if (!browse_hello) {
                rooms_label->set_text("Load the ROM first (Start Game on the launcher) to see the public rooms.");
            }
            else {
                conker::lockstep::set_server(settings.server, settings.allow_direct);
                if (!conker::lockstep::browse(*browse_hello, error)) {
                    rooms_label->set_text("Couldn't get the public rooms: " + error + ".");
                }
                else {
                    rooms_label->set_text("Getting the public rooms...");
                    browsing = true;
                }
            }
            queue_update();
        }

        void stop_browsing() {
            if (browsing) {
                conker::lockstep::stop_browsing();
                browsing = false;
            }
            stop_lan();
        }

        void stop_lan() {
            if (lan_listening) {
                conker::lockstep::stop_lan();
                lan_listening = false;
            }
        }

        // A row for a match to join by `code`: its name, how it's going, and Join if it's open.
        void add_room_row(recompui::Element* box, const conker::lockstep::ListedRoom& room, const std::string& code) {
            recompui::ContextId context = recompui::get_current_context();
            recompui::Element* line = row(box);
            const bool open = !room.playing && room.joined < room.players;
            text(line, room.name, recompui::theme::Typography::LabelMD);
            std::string about = std::to_string(room.joined) + " of " + std::to_string(room.players) + " players";
            about += room.playing ? ", playing" : "";
            about += room.rollback > 0 ? ", rollback " + std::to_string(room.rollback) : "";
            about += box == lan_box ? "" : room.direct ? ", direct allowed" : ", relay only";
            text(line, about, recompui::theme::Typography::Body, recompui::theme::color::TextDim);
            if (open) {
                recompui::Button* join_room = context.create_element<recompui::Button>(line, "Join", recompui::ButtonStyle::Secondary);
                join_room->add_pressed_callback([this, code]() {
                    if (state != State::idle) {
                        return;
                    }
                    join_text = code;
                    join_input->set_text(code);
                    on_join();
                });
            }
        }

        // This network's matches as they are now, made again when that changes.
        void show_lan(const std::vector<conker::lockstep::ListedRoom>& matches) {
            std::string summary;
            for (const conker::lockstep::ListedRoom& room : matches) {
                summary += "|" + room.code + " " + std::to_string(room.joined) + (room.playing ? "p" : "");
            }
            // Nothing yet for the first 2 s: a host's beacon comes once a second.
            const bool listening = matches.empty() && std::chrono::steady_clock::now() - lan_since < std::chrono::seconds(2);
            if (listening || (summary == shown_lan && lan_shown)) {
                return;
            }
            shown_lan = summary;
            lan_shown = true;
            lan_label->set_text(matches.empty() ? "No matches on this network. (One there should show here if its host chose "
                "\"Visible\"; if it doesn't, a firewall may be blocking UDP port 27016 on this computer.)" : "Matches on this network:");
            lan_box->clear_children();
            for (const conker::lockstep::ListedRoom& room : matches) {
                add_room_row(lan_box, room, room.code);
            }
        }

        // The list as it is now: a row each, made again when it changes.
        void show_rooms(const conker::lockstep::Browse& list) {
            if (list.state == conker::lockstep::Browse::State::failed) {
                stop_browsing();
                shown_rooms.clear();
                rooms_box->clear_children();
                rooms_label->set_text("Couldn't get the public rooms: " + list.error + ". Press Refresh to try again.");
                return;
            }
            if (list.state != conker::lockstep::Browse::State::listed) {
                return;
            }
            std::string summary = std::to_string(list.others);
            for (const conker::lockstep::ListedRoom& room : list.rooms) {
                summary += "|" + room.code + " " + std::to_string(room.joined) + (room.playing ? "p" : "");
            }
            if (summary == shown_rooms) {
                return;
            }
            shown_rooms = summary;
            const std::string mods = conker::netplay_profile::mods_text();
            std::string words = list.rooms.empty() ? "No public rooms for this game yet: host one (Public, on the Host tab)." :
                "Public rooms for this game";
            words += mods.empty() ? (list.rooms.empty() ? "" : ", with no mods (like yours):") : (list.rooms.empty() ? " Yours would need the same mods: " + mods + "." :
                ", with the same mods as yours (" + mods + "):");
            if (list.others > 0) {
                words += " " + std::to_string(list.others) + (list.others == 1 ? " more room has" : " more rooms have") +
                    " other settings or mods than yours, so it can't be joined from here.";
            }
            rooms_label->set_text(words);
            rooms_box->clear_children();
            for (const conker::lockstep::ListedRoom& room : list.rooms) {
                add_room_row(rooms_box, room, room.code);
            }
        }

        void on_host() {
            const int port = host_directly ? std::atoi(port_text.c_str()) : 0;
            if (host_directly && (port < 1 || port > 65535)) {
                set_state(State::idle, "The port is a number from 1 to 65535.");
                return;
            }
            if (host_directly && !lan_visible && address_text.find_first_not_of(' ') == std::string::npos) {
                set_state(State::idle, "Give your address first: the join code is made from it (or make the match visible on this network).");
                return;
            }
            if (!host_directly && !use_server(public_room)) {
                return;
            }
            if (local_players >= players) {
                set_state(State::idle, "With " + std::to_string(local_players) + " players on this computer, the match needs " +
                    std::to_string(local_players + 1) + " or more players: someone has to join.");
                return;
            }
            conker::lockstep::set_host(port, players, own_view_for_everyone, input_delay, local_players, rollback);
            conker::lockstep::set_lan(host_directly && lan_visible);
            hosting = true;
            begin();
            show_code();
        }

        void on_join() {
            if (join_text.find_first_not_of(' ') == std::string::npos) {
                set_state(State::idle, "Type the host's room code or join code first.");
                return;
            }
            const bool room = conker::netplay::decode_room_code(join_text).has_value();
            if (room && !use_server()) {
                return;
            }
            if (!room && !conker::netplay::decode_join_code(join_text)) {
                set_state(State::idle, "That isn't a room code (8 characters) or a join code (24 or 44): check it.");
                return;
            }
            conker::lockstep::set_join(join_text, local_players);
            begin();
        }

        void on_quick() {
            if (!use_server()) {
                return;
            }
            if (local_players >= players) {
                set_state(State::idle, "Quick Match may host: with " + std::to_string(local_players) + " players on this computer, "
                    "set Players (Host tab) to " + std::to_string(local_players + 1) + " or more.");
                return;
            }
            conker::lockstep::set_quick(players, own_view_for_everyone, input_delay, local_players, rollback);
            begin();
        }

        void on_cancel() {
            conker::lockstep::cancel();
            conker::lockstep::set_host(-1, players);
            set_state(State::idle, "Cancelled.");
        }

    protected:
        std::string_view get_type_name() override { return "NetplayPage"; }

        void process_event(const recompui::Event& e) override {
            if (e.type != recompui::EventType::Update) {
                return;
            }
            if (browsing && state == State::idle) {
                show_rooms(conker::lockstep::poll_browse());
            }
            if (lan_listening && state == State::idle) {
                show_lan(conker::lockstep::poll_lan());
            }
            if ((browsing || lan_listening) && state == State::idle) {
                queue_update();
            }
            if (state == State::connecting) {
                std::string status;
                switch (conker::lockstep::poll_connect(status)) {
                    case conker::lockstep::Progress::waiting:
                        status_label->set_text(status);
                        if (hosting && shown_code.empty()) {
                            show_code();    // the room code, once the server has made it
                        }
                        break;
                    case conker::lockstep::Progress::failed:
                        conker::lockstep::set_host(-1, players);
                        set_state(State::idle, status);
                        break;
                    case conker::lockstep::Progress::started:
                        set_state(State::starting, "Starting the game...");
                        conker::harness::begin_menu_match([](const std::string& why) {
                            std::lock_guard lock(over_mutex);
                            over_text = why;
                            over_pending = true;
                        });
                        recomp::start_game(game_id, {});
                        recompui::hide_all_contexts();
                        return;
                }
                // Polled again next frame while connecting (set_state starts it).
                queue_update();
            }
        }

    public:
        NetplayPage(recompui::ResourceId rid, recompui::Element* parent)
            : Element(rid, parent, recompui::Events(recompui::EventType::Update)) {
            using recompui::theme::Typography;
            using recompui::theme::color;
            recompui::ContextId context = recompui::get_current_context();
            set_display(recompui::Display::Flex);
            set_flex_direction(recompui::FlexDirection::Column);
            set_gap(28.0f);
            set_padding(32.0f);
            set_width(100.0f, recompui::Unit::Percent);
            set_background_color(recompui::theme::color::Background1);
            set_as_navigation_container(recompui::NavigationType::Vertical);

            recompui::Element* intro = column(this, 8.0f);
            text(intro, "Netplay", Typography::Header3);
            text(intro, "Play multiplayer online. Everyone needs this same build and the same ROM, and the same mods "
                "on, at the same versions and settings (texture packs can differ). The game starts from the beginning.",
                Typography::Body, color::TextDim);

            // How it goes: at the top, where it's seen whatever part of the page is scrolled to.
            status_row = row(this);
            status_label = text(status_row, "", Typography::LabelLG);
            cancel_button = context.create_element<recompui::Button>(status_row, "Cancel", recompui::ButtonStyle::Tertiary);
            cancel_button->add_pressed_callback([this]() { on_cancel(); });

            // Who plays on this computer, hosting or joining.
            recompui::Element* here = column(this, 12.0f);
            recompui::Element* here_row = row(here);
            text(here_row, "Players on this computer", Typography::LabelMD);
            recompui::Radio* here_radio = context.create_element<recompui::Radio>(here_row);
            for (int n = 1; n < conker::netplay::max_players; n++) {
                here_radio->add_option(std::to_string(n));
            }
            here_radio->set_index(0);
            here_radio->add_index_changed_callback([this](uint32_t index) { local_players = 1 + (int)index; });
            text(here, "One controller each, in the order they first press a button. The first player also has the "
                "keyboard, mouse and gyro.", Typography::Body, color::TextDim);

            // The netplay server, for rooms.
            recompui::Element* server = column(this, 12.0f);
            recompui::Element* server_row = row(server);
            text(server_row, "Server", Typography::LabelMD);
            recompui::TextInput* server_input = input(server_row, settings.server, 320.0f);
            server_input->add_text_changed_callback([this](const std::string& value) {
                settings.server = value;
                save_settings(settings);
            });
            text(server_row, "Connections", Typography::LabelMD);
            recompui::Radio* direct_radio = context.create_element<recompui::Radio>(server_row);
            direct_radio->add_option("Relay only");
            direct_radio->add_option("Allow direct");
            direct_radio->set_index(settings.allow_direct ? 1 : 0);
            direct_radio->add_index_changed_callback([this](uint32_t index) {
                settings.allow_direct = index == 1;
                save_settings(settings);
            });
            text(server, "Rooms are made on a netplay server (its address and port). Relay only: your game reaches the "
                "others through the server's relay, so nobody sees your address. Allow direct: straight to each "
                "other when your networks allow it, which can be quicker, but the others see your address.",
                Typography::Body, color::TextDim);

            // Hosting or joining: a tab each, so only the one chosen's settings show.
            recompui::TabSet* tabs = context.create_element<recompui::TabSet>(this);
            tabs->set_flex_shrink(0.0f);
            tabs->add_tab("Host a match");
            tabs->add_tab("Join a match");

            // Hosting.
            recompui::Element* host = column(this, 12.0f);
            host_section = host;
            recompui::Element* host_row = row(host);
            text(host_row, "Players", Typography::LabelMD);
            recompui::Radio* count_radio = context.create_element<recompui::Radio>(host_row);
            for (int n = 2; n <= conker::netplay::max_players; n++) {
                count_radio->add_option(std::to_string(n));
            }
            count_radio->set_index(0);
            count_radio->add_index_changed_callback([this](uint32_t index) { players = 2 + (int)index; });
            text(host_row, "Through", Typography::LabelMD);
            recompui::Radio* through_radio = context.create_element<recompui::Radio>(host_row);
            through_radio->add_option("A room on the server");
            through_radio->add_option("Directly");
            through_radio->set_index(0);
            through_radio->add_index_changed_callback([this](uint32_t index) {
                host_directly = index == 1;
                direct_rows->set_display(host_directly ? recompui::Display::Flex : recompui::Display::None);
                room_row->set_display(host_directly ? recompui::Display::None : recompui::Display::Flex);
                show_code();
                queue_update();
            });
            room_row = column(host, 12.0f);
            recompui::Element* listed_row = row(room_row);
            text(listed_row, "Room", Typography::LabelMD);
            recompui::Radio* listed_radio = context.create_element<recompui::Radio>(listed_row);
            listed_radio->add_option("Private");
            listed_radio->add_option("Public");
            listed_radio->set_index(0);
            listed_radio->add_index_changed_callback([this](uint32_t index) { public_room = index == 1; });
            text(room_row, "Private: only those you send the room code to can join. Public: it's also in the server's "
                "room list, for anyone with this build, ROM, settings and mods, under a made-up name.",
                Typography::Body, color::TextDim);
            direct_rows = column(host, 12.0f);
            recompui::Element* direct_row = row(direct_rows);
            text(direct_row, "UDP port", Typography::LabelMD);
            port_input = input(direct_row, port_text, 120.0f);
            port_input->add_text_changed_callback([this](const std::string& value) { port_text = value; });
            text(direct_row, "Your address", Typography::LabelMD);
            address_input = input(direct_row, "", 320.0f);
            address_input->add_text_changed_callback([this](const std::string& value) {
                address_text = value;
                show_code();
            });
            recompui::Element* screens_row = row(host);
            text(screens_row, "Screens", Typography::LabelMD);
            recompui::Radio* screens_radio = context.create_element<recompui::Radio>(screens_row);
            screens_radio->add_option("Everyone sees only their own view");
            screens_radio->add_option("Players choose");
            screens_radio->set_index(0);
            screens_radio->add_index_changed_callback([this](uint32_t index) { own_view_for_everyone = index == 0; });
            recompui::Element* delay_row = row(host);
            text(delay_row, "Input delay", Typography::LabelMD);
            recompui::Radio* delay_radio = context.create_element<recompui::Radio>(delay_row);
            delay_radio->add_option("Auto");
            for (int reads = 0; reads <= 4; reads++) {
                delay_radio->add_option(std::to_string(reads));
            }
            delay_radio->set_index(0);
            delay_radio->add_index_changed_callback([this](uint32_t index) { input_delay = (int)index - 1; });
            text(host, "Auto suits most matches (0 at home). Raise it if the game stutters waiting for a player.",
                Typography::Body, color::TextDim);
            recompui::Element* rollback_row = row(host);
            text(rollback_row, "Rollback", Typography::LabelMD);
            recompui::Radio* rollback_radio = context.create_element<recompui::Radio>(rollback_row);
            // Off, 4, 8 or 12 reads.
            rollback_radio->add_option("Off");
            for (int reads = 4; reads <= 12; reads += 4) {
                rollback_radio->add_option(std::to_string(reads));
            }
            rollback_radio->set_index(0);
            rollback_radio->add_index_changed_callback([this](uint32_t index) { rollback = 4 * (int)index; });
            text(host, "Rollback (experimental): instead of waiting for the others' input, every player's game guesses "
                "it up to that many reads ahead (about 30 a second) and goes back to correct a wrong guess, so lag is "
                "felt less.", Typography::Body, color::TextDim);
            recompui::Element* code_row = row(host);
            code_label = text(code_row, "", Typography::LabelMD);
            copy_button = context.create_element<recompui::Button>(code_row, "Copy", recompui::ButtonStyle::Tertiary);
            copy_button->add_pressed_callback([this]() {
                // Said on the button: the status line is busy while waiting for players.
                if (!shown_code.empty() && SDL_SetClipboardText(shown_code.c_str()) == 0) {
                    copy_button->set_text("Copied");
                    queue_update();
                }
            });
            recompui::Element* lan_row = row(direct_rows);
            text(lan_row, "On this network", Typography::LabelMD);
            recompui::Radio* lan_radio = context.create_element<recompui::Radio>(lan_row);
            lan_radio->add_option("Hidden");
            lan_radio->add_option("Visible");
            lan_radio->set_index(0);
            lan_radio->add_index_changed_callback([this](uint32_t index) {
                lan_visible = index == 1;
                show_code();
                queue_update();
            });
            text(direct_rows, "Visible: games on this network (at home, at a LAN party) list the match on their Join tab "
                "and join it with no code: anyone on the network can join it.", Typography::Body, color::TextDim);
            text(direct_rows, "Directly, with no server: for players outside your home, forward the UDP port on your "
                "router and use your public address (any \"what is my IP\" site). The join code contains your IP "
                "address: share it only with people you trust.", Typography::Body, color::TextDim);
            direct_rows->set_display(recompui::Display::None);
            host_button = context.create_element<recompui::Button>(host, "Host", recompui::ButtonStyle::Primary);
            host_button->add_pressed_callback([this]() { on_host(); });

            // Joining.
            recompui::Element* join = column(this, 12.0f);
            join_section = join;
            recompui::Element* join_row = row(join);
            text(join_row, "Room or join code", Typography::LabelMD);
            join_input = input(join_row, "", 520.0f);
            join_input->add_text_changed_callback([this](const std::string& value) { join_text = value; });
            recompui::Button* paste_button = context.create_element<recompui::Button>(join_row, "Paste", recompui::ButtonStyle::Tertiary);
            paste_button->add_pressed_callback([this]() {
                char* pasted = SDL_GetClipboardText();
                if (pasted != nullptr) {
                    // One line, without the spaces around it (the code reads dashes and spaces anyway).
                    std::string text = pasted;
                    SDL_free(pasted);
                    text = text.substr(0, text.find_first_of("\r\n"));
                    while (!text.empty() && text.back() == ' ') text.pop_back();
                    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
                    join_text = text.substr(0, 64);
                    join_input->set_text(join_text);
                    queue_update();     // which applies the field's new value
                }
            });
            join_button = context.create_element<recompui::Button>(join, "Join", recompui::ButtonStyle::Primary);
            join_button->add_pressed_callback([this]() { on_join(); });
            recompui::Element* quick_row = row(join);
            recompui::Button* quick_button = context.create_element<recompui::Button>(quick_row, "Quick Match", recompui::ButtonStyle::Primary);
            quick_button->add_pressed_callback([this]() {
                if (state == State::idle) {
                    on_quick();
                }
            });
            text(join, "Quick Match joins the first open public room for this game, or hosts one, in public, with the "
                "Host tab's settings, and waits for players there.", Typography::Body, color::TextDim);
            recompui::Element* public_row = row(join);
            text(public_row, "Public rooms", Typography::LabelMD);
            recompui::Button* refresh_button = context.create_element<recompui::Button>(public_row, "Refresh", recompui::ButtonStyle::Tertiary);
            refresh_button->add_pressed_callback([this]() {
                if (state == State::idle && browsing) {
                    conker::lockstep::browse_again();
                }
                else if (state == State::idle) {
                    start_browsing();
                }
            });
            rooms_label = text(join, "", Typography::Body, color::TextDim);
            rooms_box = column(join, 8.0f);
            text(join, "On this network", Typography::LabelMD);
            lan_label = text(join, "", Typography::Body, color::TextDim);
            lan_box = column(join, 8.0f);

            tabs->set_change_tab_callback([this](int index) {
                host_section->set_display(index == 0 ? recompui::Display::Flex : recompui::Display::None);
                join_section->set_display(index == 1 ? recompui::Display::Flex : recompui::Display::None);
                join_tab = index == 1;
                if (index == 1 && state == State::idle) {
                    start_browsing();
                }
                else if (index != 1) {
                    stop_browsing();
                }
                queue_update();
            });
            tabs->set_active_tab(0, false);
            join_section->set_display(recompui::Display::None);

            // What this window shows of the split screen.
            recompui::Element* display = column(this, 12.0f);
            text(display, "Your screen", Typography::LabelLG);
            recompui::Radio* display_radio = context.create_element<recompui::Radio>(display);
            display_radio->add_option("Split screen");
            display_radio->add_option("My view only");
            display_radio->set_index(conker::netplay_view::is_mine_only() ? 1 : 0);
            display_radio->add_index_changed_callback([](uint32_t index) { conker::netplay_view::set_mine_only(index == 1); });
            text(display, "\"My view only\" fills the window with your view (two players here: one above the other). "
                "The host's Screens setting can choose it for everyone.",
                Typography::Body, color::TextDim);

            set_state(State::idle, "");
            show_code();
            queue_update();
        }

        bool connecting() const { return state == State::connecting; }
        // The menu closes: whatever it was connecting is given up.
        void on_close() {
            closing = true;
            stop_browsing();
            if (state == State::connecting) {
                conker::lockstep::cancel();
                conker::lockstep::set_host(-1, players);
                set_state(State::idle, "Cancelled.");
            }
            closing = false;
        }
        // The menu opens again: the room list, if that's the tab open.
        void on_open() {
            browse_hello.reset();
            if (state == State::idle && join_tab) {
                start_browsing();
            }
        }
    };

    NetplayPage* page = nullptr;
}

void conker::netplay_menu::init(const std::u8string& id, const std::filesystem::path& rom) {
    game_id = id;
    stored_rom = rom;
}

void conker::netplay_menu::open() {
    // The launcher's option calls this with the launcher's context open, and only one can be: it's closed
    // while the menu's is used.
    recompui::ContextId launcher = recompui::get_current_context();
    const bool launcher_open = launcher.slot_id != recompui::ContextId::null().slot_id;
    if (launcher_open) {
        launcher.close();
    }
    if (modal == nullptr) {
        modal = recompui::Modal::create_modal(recompui::ModalType::Fullscreen);
        recompui::ContextId context = modal->modal_root_context;
        context.open();
        // In a scroll container: the page is taller than a small window, and squeezed into it its rows and
        // texts would be drawn over each other.
        modal->get_body()->set_overflow_y(recompui::Overflow::Hidden);
        recompui::ScrollContainer* scroll = context.create_element<recompui::ScrollContainer>(modal->get_body(), recompui::ScrollDirection::Vertical);
        scroll->set_background_color(recompui::theme::color::Background1);    // the page's, below it too
        page = context.create_element<NetplayPage>(scroll);
        context.close();
        // Closing it while connecting gives up.
        modal->set_on_close_callback([]() {
            if (page != nullptr) {
                page->on_close();
            }
        });
    }
    else {
        // Opened again: the room list, if the Join tab is open.
        recompui::ContextId context = modal->modal_root_context;
        context.open();
        page->on_open();
        context.close();
    }
    if (launcher_open) {
        launcher.open();
    }
    modal->open();
}

void conker::netplay_menu::update() {
    std::string why;
    {
        std::lock_guard lock(over_mutex);
        if (!over_pending) {
            return;
        }
        over_pending = false;
        why = over_text;
    }
    if (!why.empty()) {
        why[0] = (char)std::toupper((unsigned char)why[0]);
    }
    recompui::open_info_prompt("The match is over",
        why + ". The game goes on without anyone's input from here: quit when you like.", "Quit",
        []() { ultramodern::quit(); });
}
