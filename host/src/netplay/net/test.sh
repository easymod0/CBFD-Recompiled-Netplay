#!/bin/sh
# Tests the netplay transport from a build of this folder (CMakeLists.txt): the packets' and join codes'
# tests, then matches between copies of netplay_probe over loopback (2 players; 3 with 20% of packets
# lost, which also tests the host passing inputs on; 3 and 4 with two players on the host or on a joiner,
# also lossy; a joiner with more players than ports left, which must be refused), joiners with another match's key, another ROM or
# other mods, which must be refused, joiners sending Inputs early or far ahead or a Hash twice, which
# must be dropped, as must one sending Inputs for a port that isn't its own, one that never says Hello, which must be dropped too, and one sending a message bigger
# than any packet, which the library must close. Then a match through ICE, by a room on the signalling
# server netplay_server on loopback (directly: the relay needs a TURN server, tools/netplay/server/test.sh);
# a room gone with its host, the public room list (by game, mods and platform), a relay-only host on a
# server without one, and guessing room codes, which the server stops.
#   sh host/src/netplay/net/test.sh <build folder>
set -u
bin=$1
probe=$bin/netplay_probe
failed=0

"$bin/packets_test" || failed=1
"$bin/join_code_test" || failed=1
"$bin/signal_packets_test" || failed=1
"$bin/turn_credentials_test" || failed=1

# The join code a host printed in <file> (its key is random), once it has.
code_of() {
    i=0
    while [ $i -lt 50 ]; do
        code=$(sed -n 's/^join code //p' "$1")
        [ -n "$code" ] && { echo "$code"; return; }
        sleep 0.1
        i=$((i + 1))
    done
}

# match <name> <port> <players> [probe options...]: a host and players - 1 joiners, by its code; all
# must succeed.
match() {
    name=$1; port=$2; players=$3; shift 3
    "$probe" host "$port" --players "$players" "$@" > "$name.host.txt" 2>&1 &
    pids=$!
    to=$(code_of "$name.host.txt")
    i=1
    while [ $i -lt "$players" ]; do
        "$probe" join "$to" "$@" > "$name.join$i.txt" 2>&1 &
        pids="$pids $!"
        sleep 0.5
        i=$((i + 1))
    done
    ok=1
    for pid in $pids; do
        wait "$pid" || ok=0
    done
    for f in "$name".*.txt; do
        echo "-- $f"
        grep -v '^\[netplay\]' "$f"
    done
    if [ $ok -eq 1 ]; then echo "$name: passed"; else echo "$name: FAILED"; failed=1; fi
}

match two 27200 2 --reads 150 --own-view 1
# The host's own-view rule reaches the joiner with Start.
if grep -q "started (own view)" two.join1.txt; then echo "own view: passed"; else echo "own view: FAILED"; failed=1; fi
match three-lossy 27201 3 --reads 150 --loss 20

# couch <name> <port> <players> <host's players here> <each joiner's...>: a lossy match with more than one
# player on some machines (joiners started half a second apart); all must succeed.
couch() {
    name=$1; port=$2; players=$3; host_local=$4; shift 4
    "$probe" host "$port" --players "$players" --local "$host_local" --reads 150 --loss 20 > "$name.host.txt" 2>&1 &
    pids=$!
    to=$(code_of "$name.host.txt")
    i=1
    for local in "$@"; do
        "$probe" join "$to" --local "$local" --reads 150 --loss 20 > "$name.join$i.txt" 2>&1 &
        pids="$pids $!"
        sleep 0.5
        i=$((i + 1))
    done
    ok=1
    for pid in $pids; do
        wait "$pid" || ok=0
    done
    for f in "$name".*.txt; do
        echo "-- $f"
        grep -v '^\[netplay\]' "$f"
    done
    if [ $ok -eq 1 ]; then echo "$name: passed"; else echo "$name: FAILED"; failed=1; fi
}
couch couch-host 27210 3 2 1
grep -q "welcomed: ports 4 of 3" couch-host.join1.txt || { echo "couch-host: the joiner isn't on port 2"; failed=1; }
couch couch-join 27211 4 1 2 1
# Lowest free ports first, in the order the handshakes finish (with lost packets, either joiner may be first).
if ! { grep -q "welcomed: ports 6 of 4" couch-join.join1.txt && grep -q "welcomed: ports 8 of 4" couch-join.join2.txt; } &&
   ! { grep -q "welcomed: ports c of 4" couch-join.join1.txt && grep -q "welcomed: ports 2 of 4" couch-join.join2.txt; }; then
    echo "couch-join: the joiners' ports aren't the lowest free ones"
    failed=1
fi

# Another match's key (a code for this address with another key), another ROM, or other mods: refused,
# with the reason (a wrong key's alone, as nothing else is checked).
refused() {
    name=$1; port=$2; option=$3; reason=$4
    "$probe" host "$port" --reads 30 > "$name.host.txt" 2>&1 &
    host=$!
    to=$(code_of "$name.host.txt")
    [ "$option" = "--key" ] && to=$("$probe" code "127.0.0.1:$port" 1234567890ABCDEF) && option=
    "$probe" join "$to" --reads 30 ${option:+$option 2} > "$name.join.txt" 2>&1
    if grep -q "ended: $reason" "$name.join.txt"; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        cat "$name.join.txt"
        failed=1
    fi
    kill $host 2>/dev/null
}
refused refused-key 27207 --key "not this match's join code"
refused refused-rom 27202 --rom "a different ROM"
refused refused-mods 27204 --mods "other mods on than the host's"
# More players on a joiner than the match has ports left (a match of 2: the host and one).
refused refused-full 27212 --local "the match is full"

# A hostile joiner: Inputs before the start (it joins a lobby of 3 alone, so the match hasn't started),
# far beyond what the game could have reached, or the same Hash twice (in a started match of 2). The host
# must drop it as broken (the joiner hears why) and keep running.
hostile() {
    name=$1; port=$2; players=$3; mode=$4
    "$probe" host "$port" --players "$players" --reads 600 > "$name.host.txt" 2>&1 &
    host=$!
    to=$(code_of "$name.host.txt")
    "$probe" join "$to" --reads 600 --hostile "$mode" > "$name.join.txt" 2>&1
    if grep -q "ended: an unexpected or broken packet" "$name.join.txt" && kill -0 $host 2>/dev/null; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        cat "$name.join.txt"
        failed=1
    fi
    kill $host 2>/dev/null
}
hostile hostile-early 27205 3 early
hostile hostile-far 27206 2 far
hostile hostile-hash 27208 2 hash
hostile hostile-port 27213 2 port

# A joiner that never says Hello: dropped after the handshake time (3 s). One that sends a message bigger
# than any packet: closed by the library at once.
raw() {
    name=$1; port=$2; option=$3
    "$probe" host "$port" --reads 30 > "$name.host.txt" 2>&1 &
    host=$!
    code_of "$name.host.txt" > /dev/null
    if "$probe" join "127.0.0.1:$port" "$option" > "$name.join.txt" 2>&1; then
        echo "$name: passed"
    else
        echo "$name: FAILED"
        failed=1
    fi
    grep -v '^\[netplay\]' "$name.join.txt"
    kill $host 2>/dev/null
}
raw silent 27203 --silent
raw oversize 27209 --oversize

# Through ICE: the server, then a host makes a room and two joiners join by its code, lossy.
# (Every probe connects from the same address: more than the server lets one make in a burst.)
"$bin/netplay_server" --port 27220 --connection-burst 1000 > ice.server.txt 2>&1 &
server=$!
room_of() {
    i=0
    while [ $i -lt 100 ]; do
        room=$(sed -n 's/^room //p' "$1")
        [ -n "$room" ] && { echo "$room"; return; }
        sleep 0.1
        i=$((i + 1))
    done
}
"$probe" host room --players 3 --signal 127.0.0.1:27220 --direct 1 --reads 150 --loss 20 > ice.host.txt 2>&1 &
pids=$!
room=$(room_of ice.host.txt)
for i in 1 2; do
    "$probe" join "$room" --signal 127.0.0.1:27220 --direct 1 --reads 150 --loss 20 > "ice.join$i.txt" 2>&1 &
    pids="$pids $!"
    sleep 0.5
done
ok=1
for pid in $pids; do
    wait "$pid" || ok=0
done
for f in ice.host.txt ice.join*.txt; do
    echo "-- $f"
    grep -v '^\[netplay\]' "$f"
done
if [ $ok -eq 1 ] && grep -q "P2P ICE" ice.join1.txt; then echo "ice: passed"; else echo "ice: FAILED"; failed=1; fi
# A host that made a room and left, then hosts again from the same process (a new id: the server keeps the
# old one from reuse).
"$probe" host room --signal 127.0.0.1:27220 --direct 1 --reads 60 --again 1 > ice-again.host.txt 2>&1 &
pids=$!
room=$(room_of ice-again.host.txt)
"$probe" join "$room" --signal 127.0.0.1:27220 --direct 1 --reads 60 > ice-again.join.txt 2>&1 &
pids="$pids $!"
ok=1
for pid in $pids; do
    wait "$pid" || ok=0
done
if [ $ok -eq 1 ] && grep -q "^left room" ice-again.host.txt; then echo "ice-again: passed"; else
    echo "ice-again: FAILED"; cat ice-again.host.txt ice-again.join.txt; failed=1; fi
# expect <name> <text> <probe options...>: the probe fails, saying <text>.
expect() {
    name=$1; text=$2; shift 2
    if ! "$probe" "$@" > "$name.txt" 2>&1 && grep -q "$text" "$name.txt"; then echo "$name: passed"; else
        echo "$name: FAILED"; cat "$name.txt"; failed=1; fi
}
# The room closed with its host: the code finds nothing now.
expect ice-closed "no room $room on the server" join "$room" --signal 127.0.0.1:27220 --direct 1
# Relay-only, but this server has no relay.
expect ice-norelay "offers no TURN relay" host room --signal 127.0.0.1:27220
# The public room list: a listed room of this game (3 players), one with other mods (counted, not listed),
# one on macOS (apart from Linux and Windows) and one not listed. The list shows the first as players join
# it, and as it's played.
# listed <name> <expected "rooms n, others n"> <grep for a room line, or -> <probe options...>
listed() {
    name=$1; count=$2; line=$3; shift 3
    "$probe" list --signal 127.0.0.1:27220 "$@" > "$name.txt" 2>&1
    if grep -qx "$count" "$name.txt" && { [ "$line" = - ] || grep -q "^listed $line" "$name.txt"; }; then echo "$name: passed"; else
        echo "$name: FAILED (wanted $count, $line)"; cat "$name.txt"; failed=1; fi
}
"$probe" host room --players 3 --public 1 --signal 127.0.0.1:27220 --direct 1 --reads 60 > lobby.host.txt 2>&1 &
lobby_host=$!
lobby_room=$(room_of lobby.host.txt)
others=
"$probe" host room --public 1 --mods 2 --signal 127.0.0.1:27220 --direct 1 > lobby.mods.txt 2>&1 &
others="$others $!"
"$probe" host room --public 1 --platform 3 --signal 127.0.0.1:27220 --direct 1 > lobby.mac.txt 2>&1 &
others="$others $!"
"$probe" host room --signal 127.0.0.1:27220 --direct 1 > lobby.private.txt 2>&1 &
others="$others $!"
room_of lobby.mods.txt > /dev/null; room_of lobby.mac.txt > /dev/null; room_of lobby.private.txt > /dev/null
sleep 0.5
listed lobby-list "rooms 1, others 1" "$lobby_room 1/3 direct rollback 0"
listed lobby-windows "rooms 1, others 1" "$lobby_room 1/3"
listed lobby-mac "rooms 1, others 0" - --platform 3
grep -q "^listed $lobby_room" lobby-mac.txt && { echo "lobby-mac: FAILED (Linux's room listed)"; failed=1; }
listed lobby-build "rooms 0, others 0" - --build 2
"$probe" join "$lobby_room" --signal 127.0.0.1:27220 --direct 1 --reads 60 > lobby.join1.txt 2>&1 &
lobby_joiners=$!
sleep 2
listed lobby-joined "rooms 1, others 1" "$lobby_room 2/3"
"$probe" join "$lobby_room" --signal 127.0.0.1:27220 --direct 1 --reads 60 > lobby.join2.txt 2>&1 &
lobby_joiners="$lobby_joiners $!"
sleep 2
listed lobby-playing "rooms 1, others 1" "$lobby_room 3/3 playing"
for pid in $lobby_host $lobby_joiners; do
    wait "$pid" || { echo "lobby-match: FAILED"; failed=1; }
done
# Killed, the others' connections only time out on the server (the library's 10 s): until then, their
# rooms are listed.
kill $others 2>/dev/null
i=0
while [ $i -lt 30 ] && ! "$probe" list --signal 127.0.0.1:27220 2>&1 | grep -qx "rooms 0, others 0"; do
    sleep 1
    i=$((i + 1))
done
listed lobby-gone "rooms 0, others 0" -
# Guessing codes: after 10 that aren't there (one above; a slow machine's may be refilled, one each 6 s),
# the server closes the connection.
i=0
guessed=
while [ $i -lt 20 ] && [ -z "$guessed" ]; do
    "$probe" join "ZZZZ-ZZ$(printf '%02d' $i)" --signal 127.0.0.1:27220 --direct 1 > "ice-guess$i.txt" 2>&1
    grep -q "too many room codes" "ice-guess$i.txt" && guessed=$i
    i=$((i + 1))
done
if [ -n "$guessed" ] && [ "$guessed" -ge 9 ] && grep -q "no room ZZZZ-ZZ00" ice-guess0.txt; then echo "ice-guess: passed (refused at $((guessed + 1)))"; else
    echo "ice-guess: FAILED"; cat ice-guess0.txt "ice-guess$((i - 1)).txt"; failed=1; fi
kill $server 2>/dev/null

[ $failed -eq 0 ] && echo "all passed" || echo "FAILED"
exit $failed
