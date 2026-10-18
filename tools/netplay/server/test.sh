#!/bin/sh
# Tests the relay with the real servers: starts compose.yaml, the server built from here (compose.build.yaml;
# with compose.test.yaml, so coturn relays
# between addresses on this machine) as the project conker-netplay-test, then plays matches through it
# with netplay_probe: two players relay-only (the joiner must be told only relay addresses, never the
# host's own), and three with 20% of packets lost. Stops the servers after. Linux, with Docker.
#   sh tools/netplay/server/test.sh <build folder of host/src/netplay/net> [output folder]
set -u
here=$(cd "$(dirname "$0")" && pwd)
probe=$(cd "$1" && pwd)/netplay_probe
out=${2:-.}
mkdir -p "$out" && cd "$out" || exit 1
failed=0

# This machine's address on its network: coturn can't relay on loopback (the library refuses it).
PUBLIC_IP=$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')
export PUBLIC_IP TURN_USER=conker TURN_PASSWORD=test-only
[ -n "$PUBLIC_IP" ] || { echo "no network address"; exit 1; }
compose="docker compose -p conker-netplay-test -f $here/compose.yaml -f $here/compose.build.yaml -f $here/compose.test.yaml"
$compose up -d --build --wait > compose.txt 2>&1 || { cat compose.txt; echo "the servers didn't start"; exit 1; }
sleep 1

ice="--signal 127.0.0.1:27100 --turn $PUBLIC_IP:3478 --turn-user conker --turn-password test-only"

ticket_of() {
    i=0
    while [ $i -lt 100 ]; do
        ticket=$(sed -n 's/^ticket //p' "$1")
        [ -n "$ticket" ] && { echo "$ticket"; return; }
        sleep 0.1
        i=$((i + 1))
    done
}

# relay <name> <players> [probe options...]: a host and players - 1 joiners, relay-only; all must succeed.
relay() {
    name=$1; players=$2; shift 2
    "$probe" host ice --players "$players" $ice "$@" > "$name.host.txt" 2>&1 &
    pids=$!
    ticket=$(ticket_of "$name.host.txt")
    i=1
    while [ $i -lt "$players" ]; do
        "$probe" join "$ticket" $ice "$@" > "$name.join$i.txt" 2>&1 &
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
    # Only relay candidates, from either side.
    if grep -h "Got remote candidate" "$name".*.txt | grep -v -q "typ relay"; then
        echo "$name: a peer was told an address that isn't the relay's"
        ok=0
    fi
    grep -q "Got remote candidate" "$name.join1.txt" || ok=0
    if [ $ok -eq 1 ]; then echo "$name: passed"; else echo "$name: FAILED"; failed=1; fi
}
relay relay-two 2 --reads 150
relay relay-three-lossy 3 --reads 150 --loss 20

$compose logs > servers.txt 2>&1
$compose down > /dev/null 2>&1
[ $failed -eq 0 ] && echo "all passed" || echo "FAILED"
exit $failed
