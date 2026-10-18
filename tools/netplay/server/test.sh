#!/bin/sh
# Tests the relay with the real servers: compose.yaml, with the server built from here (compose.build.yaml)
# and compose.test.yaml, as the project conker-netplay-test, twice:
#
#   nat  behind NAT, as at home: the public address is a made-up one (TEST_PUBLIC_IP), which net puts on
#        this machine, translating to and from its LAN address. Matches through a room with
#        netplay_probe, relay-only with the TURN credentials the server hands out: two players (the
#        joiner must be told only the relay's address, never the host's own) and three with 20% of
#        packets lost. Then a credential coturn must refuse; the peers coturn denies (private ones, this
#        machine's LAN address, cloud metadata); net's guard (a relay reaches this machine only on the
#        relay ports); its translation, for a packet from outside (a container on Docker's bridge) to the
#        LAN address on a relay port; that the servers looked the name up; and that the public address is
#        gone from this machine once they stop.
#   vps  the public address already this machine's (put on lo by the test), as on a server with its
#        public address: no translation, only the guard. A match of two, and the guard.
#
# Linux, with Docker (rootful) and nftables, python3 and openssl. It adds and removes TEST_PUBLIC_IP on lo,
# and replaces any conker_netplay nftables table: don't run it on a machine that hosts the servers.
#   sh tools/netplay/server/test.sh <build folder of host/src/netplay/net> [output folder]
set -u
here=$(cd "$(dirname "$0")" && pwd)
probe=$(cd "$1" && pwd)/netplay_probe
out=${2:-.}
mkdir -p "$out" && cd "$out" || exit 1
failed=0

# This machine's address on its network (coturn can't relay on loopback: the library refuses it), and a
# made-up public address; the servers find it by name (PUBLIC_HOST; compose.test.yaml maps it), as they
# would a real one.
TEST_IP=$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')
[ -n "$TEST_IP" ] || { echo "no network address"; exit 1; }
export TEST_PUBLIC_IP=5.5.5.5 PUBLIC_HOST=netplay-test.invalid PUBLIC_IP= HOME_LAN_IP= TURN_SECRET=test-only-secret-0123456789
compose="docker compose -p conker-netplay-test -f $here/compose.yaml -f $here/compose.build.yaml -f $here/compose.test.yaml"
image=conker-netplay-server:local
# A command as root, with network-admin rights, on this machine's network (the vps pass's address).
as_admin() { command=$1; shift; docker run --rm --network host --cap-add NET_ADMIN --user 0 --entrypoint "$command" "$image" "$@"; }
# However it ends, the servers stop and the vps pass's address goes.
trap '$compose down -v > /dev/null 2>&1; as_admin ip addr del "$TEST_PUBLIC_IP/32" dev lo > /dev/null 2>&1' EXIT
trap 'exit 130' INT TERM
$compose build > build.txt 2>&1 || { cat build.txt; echo "the image didn't build"; exit 1; }

# start <pass>: starts the servers, and waits for coturn (which waits for net).
start() {
    $compose up -d --wait > "$1.compose.txt" 2>&1 || { cat "$1.compose.txt"; echo "the servers didn't start"; exit 1; }
    i=0
    until $compose logs turn 2>&1 | grep -q "Total auth threads"; do
        i=$((i + 1))
        [ $i -le 120 ] || { $compose logs > "$1.servers.txt" 2>&1; echo "coturn didn't start"; exit 1; }
        sleep 0.5
    done
    sleep 1
}
stop() {
    $compose logs > "$1.servers.txt" 2>&1
    $compose down -v > /dev/null 2>&1
}
check() { if [ "$2" -eq 1 ]; then echo "$1: passed"; else echo "$1: FAILED"; failed=1; fi; }

ice="--signal 127.0.0.1:27100"

room_of() {
    i=0
    while [ $i -lt 100 ]; do
        room=$(sed -n 's/^room //p' "$1")
        [ -n "$room" ] && { echo "$room"; return; }
        sleep 0.1
        i=$((i + 1))
    done
}

# relay <name> <players> [probe options...]: a host and players - 1 joiners, relay-only; all must succeed.
relay() {
    name=$1; players=$2; shift 2
    "$probe" host room --players "$players" $ice "$@" > "$name.host.txt" 2>&1 &
    pids=$!
    room=$(room_of "$name.host.txt")
    i=1
    while [ $i -lt "$players" ]; do
        "$probe" join "$room" $ice "$@" > "$name.join$i.txt" 2>&1 &
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
    # Only the relay's address, from either side.
    if grep -h "Got remote candidate" "$name".*.txt | grep -v -q "$TEST_PUBLIC_IP [0-9]* typ relay"; then
        echo "$name: a peer was told an address that isn't the relay's"
        ok=0
    fi
    grep -q "Got remote candidate" "$name.join1.txt" || ok=0
    check "$name" $ok
}

# A TURN client of coturn's own (turnutils_uclient) with good credentials, asking to relay to <peer>:<port>.
# (Not as nobody, coturn's user: net's guard would hold it to the relay ports too.)
uclient() {
    user="$(($(date +%s) + 600)):0123456789abcdef"
    password=$(printf '%s' "$user" | openssl dgst -sha1 -hmac "$TURN_SECRET" -binary | base64)
    timeout -k 5 10 docker run --rm --init --user 1000 --network host --entrypoint turnutils_uclient coturn/coturn:4.6.3 \
        -u "$user" -w "$password" -p 3478 -e "$1" -r "$2" -n 3 -m 1 "$TEST_IP" > /dev/null 2>&1
}

# listen <address> <port> <file>: in the background, waits 12 s for a packet at the first free port from
# <port> down (a relay may have one), and says "received" or "nothing" in <file>. Prints the port.
listen() {
    python3 -c 'import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
port = int(sys.argv[2])
while True:
    try:
        s.bind((sys.argv[1], port))
        break
    except OSError:
        port -= 1
print("port", port, flush=True)
s.settimeout(12)
try:
    s.recvfrom(2048)
    print("received")
except socket.timeout:
    print("nothing")' "$1" "$2" > "$3" 2>&1 &
    i=0
    until grep -q '^port' "$3" 2> /dev/null || [ $i -ge 50 ]; do sleep 0.1; i=$((i + 1)); done
    sed -n 's/^port //p' "$3"
}

# guard <pass>: relayed to this machine (its public address), a port outside the relay range gets
# nothing; one inside it does.
guard() {
    port=$(listen "$TEST_PUBLIC_IP" 39999 "$1.outside.txt")
    uclient "$TEST_PUBLIC_IP" "$port"
    wait
    port=$(listen "$TEST_PUBLIC_IP" 49659 "$1.inside.txt")
    uclient "$TEST_PUBLIC_IP" "$port"
    wait
    ok=0
    grep -q nothing "$1.outside.txt" && grep -q received "$1.inside.txt" && ok=1
    [ $ok -eq 1 ] || echo "  outside the relay range: $(tail -n 1 "$1.outside.txt"); inside: $(tail -n 1 "$1.inside.txt")"
    check "$1-guard" $ok
}

echo "== nat"
start nat
ok=0
$compose logs net 2>&1 | grep -q "behind NAT: public address $TEST_PUBLIC_IP on this machine for the relays, to and from $TEST_IP" && ok=1
check nat-detected $ok
relay nat-two 2 --reads 150
relay nat-three-lossy 3 --reads 150 --loss 20

# A joiner with a password the server didn't make: coturn refuses it, and relay-only it can't connect.
"$probe" host room --reads 60 $ice > bad-password.host.txt 2>&1 &
host=$!
room=$(room_of bad-password.host.txt)
"$probe" join "$room" --reads 60 $ice --turn-password not-the-servers > bad-password.join.txt 2>&1
bad_status=$?
kill $host 2>/dev/null

# The deny list: private addresses, this machine's LAN one and cloud metadata. (That a public one is
# relayed to, the matches and the guard show.)
for peer in 10.1.2.3 169.254.169.254 192.168.1.1 "$TEST_IP"; do
    uclient "$peer" 3480
done
$compose logs turn > nat.turn.txt 2>&1
ok=1
for peer in 10.1.2.3 169.254.169.254 192.168.1.1 "$TEST_IP"; do
    grep -q "peer IP $peer denied" nat.turn.txt || { echo "  coturn doesn't deny $peer"; ok=0; }
done
check denied-peers $ok
guard nat

# net's translation: a packet from outside (a container on Docker's bridge) to the LAN address on a relay
# port arrives at the public address.
port=$(listen "$TEST_PUBLIC_IP" 49658 nat.inbound.txt)
docker run --rm busybox:1.36 sh -c "echo x | nc -u -w 1 $TEST_IP $port" > /dev/null 2>&1
wait
ok=0
grep -q received nat.inbound.txt && ok=1
check nat-inbound $ok

stop nat
# ...and it was coturn that refused that password (its credentials' user ends with the joiner's id).
bad_id=$(sed -n 's/^registered with .* as \([0-9a-f]*\).*/\1/p' bad-password.join.txt)
ok=0
[ $bad_status -ne 0 ] && grep -q "rendezvous" bad-password.join.txt && [ -n "$bad_id" ] &&
    grep "$bad_id" nat.servers.txt | grep -q "check_stun_auth" && ok=1
check bad-password $ok
# The signalling server and coturn looked the name up (net too, as it said above).
ok=0
[ "$(grep -c "public address $TEST_PUBLIC_IP\$" nat.servers.txt)" = 2 ] && ok=1
check public-host $ok
# Gone from this machine once net stopped.
ok=1
ip -4 addr show dev lo | grep -q " $TEST_PUBLIC_IP/" && ok=0
check nat-cleanup $ok

echo "== vps"
as_admin ip addr add "$TEST_PUBLIC_IP/32" dev lo || { echo "couldn't add $TEST_PUBLIC_IP"; exit 1; }
start vps
ok=0
$compose logs net 2>&1 | grep -q "public address $TEST_PUBLIC_IP is this machine's" && ok=1
check vps-no-nat $ok
relay vps-two 2 --reads 150
guard vps
stop vps
# net leaves an address it didn't add.
ok=0
ip -4 addr show dev lo | grep -q " $TEST_PUBLIC_IP/" && ok=1
check vps-cleanup $ok
as_admin ip addr del "$TEST_PUBLIC_IP/32" dev lo

[ $failed -eq 0 ] && echo "all passed" || echo "FAILED"
exit $failed
