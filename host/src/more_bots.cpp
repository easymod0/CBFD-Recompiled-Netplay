// Multiplayer: Characters (discussion #105): more bots in a multiplayer game than the game allows, up to 12 or 16
// characters (players and bots) in all, with any number of players. From hooks in conker.toml.
//
// The bot menus. Each multiplayer game has a record of 10 bytes (0x800AB690 + its index * 10; the index is the
// game state's +0x42: Heist 0, Tank 1, Raptor 2, Deathmatch's maps 3-7, Beach 8, War 9-10, Race 11-12), whose
// +1 picks its limits:
// - Deathmatch and Tank (func_151E1214, a row of bots): the most characters, from a table of a byte for each
//   number of players (0x800AB74C, its row from the byte at 0x8008FEE3 - that +1), such as 10, 8, 4, 4: with 3
//   or 4 players, 1 bot or none. Loaded into $s4 at 0x151E134C.
// - The team games, War, Beach, Raptor and Heist (func_151E1744, bots on each team): a record of 12 bytes
//   (0x800AB710 + that +1 * 12): the teams, then the fewest characters, the most, and the most on a team, for 1-2
//   players (+1 to +3) and for 3-4 (+4 to +6), loaded into $s1, $s3 and $a1 (0x151E18C8-0x151E18E0). War allows 4
//   characters with 3 or 4 players: no bots.
// Nothing else reads them. The game has places for 16 characters (D_8008FE44, each one's slot), so 16 is the most.
// Tank stops at 12: past it the game overruns a heap block (the heap walk, func_10004250, crashes as the level is
// freed). Race isn't changed.
//
// Memory. The game's heap is one block from 0x800E9D10 to D_80038098 (func_10003BD0, as the game starts), under
// the top of memory's areas (0x803F5000 to 0x80400000), and multiplayer fills it: 16 characters, with four views,
// can run it out (the game's allocator stops the game: a syscall at 0x150AD770). The game only uses the first 4MB,
// and has a layout for 8MB from its development (func_10003930, by D_80038080, always 0 in the retail game), the
// top areas at 0x807F5000: with 16 characters that layout is set (not the flag, which changes more), the heap's
// end a page lower, for the rank names (below). The recompilation has 8MB, all of it in save states. It's set as
// the game starts, so 16 takes effect then; until then the limit is 12.
//
// The rank names. The HUD's rank, "0 (11th)" (func_150918EC at 0x15092018), takes its name from a table of 13
// ("P", then "1st" to "12th"); past it are other strings, then pointers that aren't strings (a crash). 13th to
// 16th are written into that page.
//
// In netplay (and a replay or recording) it's the game's own, the same on every machine: the setting changes the
// game's memory, and each machine has its own.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "recomp.h"
#include "recompui/config.h"

#include "conker.hpp"

namespace {
    const std::string characters_option = "mp_characters";
    enum class Characters : uint32_t {
        Original = 0,
        Twelve = 12,
        Sixteen = 16,
    };
    bool option_added = false;

    constexpr uint32_t game_state = 0x8008FDD4;      // gGameState: its +0x42, the multiplayer game's record
    constexpr int tank = 1;
    constexpr int race_first = 11;
    constexpr int race_last = 12;
    constexpr uint32_t teams = 0x800E0BB0;           // D_800E0BB0: the team game's teams
    constexpr uint32_t heap_end = 0x80038098;        // D_80038098
    constexpr uint32_t rank_names = 0x807F4000;      // the page under the 8MB layout's top areas
    constexpr int game_most = 12;                    // without the 8MB layout
    constexpr int most = 16;

    // The rule: the setting, or in netplay the match's.
    int characters() {
        if (conker::netplay_profile::active()) {
            return 0;
        }
        if (!option_added) {
            return 0;
        }
        return (int)std::get<uint32_t>(recompui::config::get_general_config().get_option_value(characters_option));
    }

    // The most characters in this multiplayer game (0: the game's own).
    int limit(uint8_t* rdram) {
        const int wanted = characters();
        if (wanted <= 0) {
            return 0;
        }
        const gpr state = (gpr)(int32_t)MEM_W(0, (gpr)(int32_t)game_state);
        const int game = (int8_t)MEM_B(0x42, state);
        if (game >= race_first && game <= race_last) {
            return 0;
        }
        const bool big_heap = (uint32_t)MEM_W(0, (gpr)(int32_t)heap_end) == rank_names;
        int result = std::min(wanted, big_heap ? most : game_most);
        if (game == tank) {
            result = std::min(result, game_most);
        }
        return result;
    }
}

void conker::more_bots::add_options(recomp::config::Config& config) {
    static const std::vector<recomp::config::ConfigOptionEnumOption> choices = {
        {Characters::Original, "Original", "Original"},
        {Characters::Twelve, "12", "Up to 12"},
        {Characters::Sixteen, "16", "Up to 16"},
    };
    config.add_enum_option(characters_option, "Multiplayer Characters",
        "The most characters, players and bots, in a multiplayer game, with any number of players (the game allows "
        "fewer bots the more players there are: none in a 4-player Deathmatch or War). <recomp-color primary>Original"
        "</recomp-color> for the game's own. Tank stops at 12, and Race is the game's own. 16 takes effect the next "
        "time the game starts (until then it's 12). Netplay matches are the game's own.",
        choices, Characters::Original);
    option_added = true;
}

// func_10003BD0, as the game starts: the 8MB layout for 16 characters.
extern "C" void conker_more_bots_memory(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (characters() > game_most) {
        MEM_W(0, (gpr)(int32_t)0x80038090) = 0x807F5000;
        MEM_W(0, (gpr)(int32_t)0x80038094) = 0x807FE000;
        MEM_W(0, (gpr)(int32_t)0x8003809C) = 0x807FE000;
        MEM_W(0, (gpr)(int32_t)heap_end) = rank_names;
    }
}

// func_151E1214 at 0x151E1350: $s4, the most characters (never fewer than the game's).
extern "C" void conker_more_bots_row(uint8_t* rdram, recomp_context* ctx) {
    const int most_here = limit(rdram);
    if (most_here > (int)(int32_t)ctx->r20) {
        ctx->r20 = most_here;
    }
}

// func_151E1744 at 0x151E18E4: $s3, the most characters, and $a1, the most on a team.
extern "C" void conker_more_bots_teams(uint8_t* rdram, recomp_context* ctx) {
    const int most_here = limit(rdram);
    const int team_count = (int8_t)MEM_B(0, (gpr)(int32_t)teams);
    if (most_here <= 0 || team_count <= 0) {
        return;
    }
    ctx->r19 = std::max<int>((int)(int32_t)ctx->r19, most_here);
    ctx->r5 = std::max<int>((int)(int32_t)ctx->r5, most_here / team_count);
}

// func_150918EC at 0x1509201C: $t8, the rank * 4, and $t9, the name it loaded.
extern "C" void conker_more_bots_rank_name(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t rank = (uint32_t)ctx->r24 / 4;
    if (rank <= (uint32_t)game_most) {
        return;
    }
    // "13th" to "16th" ("th" is the font's 0xB9), 4 bytes each.
    const uint32_t shown = std::min<uint32_t>(rank, most);
    const gpr name = (gpr)(int32_t)(rank_names + (shown - 13) * 4);
    MEM_B(0, name) = (int8_t)('0' + shown / 10);
    MEM_B(1, name) = (int8_t)('0' + shown % 10);
    MEM_B(2, name) = (int8_t)0xB9;
    MEM_B(3, name) = 0;
    ctx->r25 = name;
}
