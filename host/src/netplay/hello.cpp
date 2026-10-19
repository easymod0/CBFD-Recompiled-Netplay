// Netplay's handshake hashes (hello.hpp). The ROM and the profile are hashed as for the harness's
// recordings (harness.cpp), so a recording and a match of the same game carry the same ones.

#include "hello.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "librecomp/game.hpp"

#include "../conker.hpp"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

std::string conker::netplay::release_url(const std::string& release) {
    // v<digits and dots>-netplay.<digits>: the release workflow's tags.
    const size_t at = release.find("-netplay.");
    const auto digits = [](std::string_view s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c); }); };
    if (release.size() < 2 || release[0] != 'v' || at == std::string::npos || at < 2 ||
        !std::all_of(release.begin() + 1, release.begin() + at, [](char c) { return std::isdigit((unsigned char)c) || c == '.'; }) ||
        !digits(std::string_view(release).substr(at + 9))) {
        return {};
    }
    return "https://github.com/easymod0/CBFD-Recompiled-Netplay/releases/tag/" + release;
}

conker::netplay::Hello conker::netplay::local_hello() {
    return hello_for_rom(recomp::get_rom());
}

conker::netplay::Hello conker::netplay::hello_for_rom(std::span<const uint8_t> rom) {
    Hello hello;
    hello.build = XXH3_64bits(build_id(), std::strlen(build_id()));
    hello.rom = XXH3_64bits(rom.data(), rom.size());
    hello.profile = conker::netplay_profile::hash();
    hello.mods = conker::netplay_profile::mods_hash();
    return hello;
}
