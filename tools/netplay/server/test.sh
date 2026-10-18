#!/bin/sh
# Tests the relay with the real servers: starts compose.yaml, the server built from here (compose.build.yaml;
# with compose.test.yaml), behind NAT as at home (the home profile, with home-net: a made-up public
# address on this machine for the relays), as the project conker-netplay-test. Then plays matches through
# a room with netplay_probe, relay-only with the TURN credentials the server hands out: two players (the
# joiner must be told only relay addresses, never the host's own), and three with 20% of packets lost.
# Then a credential coturn must refuse; the peers coturn relays to and denies (a public address yes;
# private ones, this machine's LAN address and cloud metadata no); and home-net's guard: a relay can't
# reach this machine but on the relay ports. Stops the servers after. Linux, with Docker (and nftables).
#   sh tools/netplay/server/test.sh <build folder of host/src/netplay/net> [output folder]
set -u
here=$(cd "$(dirname "$0")" && pwd)
probe=$(cd "$1" && pwd)/netplay_probe
out=${2:-.}
mkdir -p "$out" && cd "$out" || exit 1
failed=0

# This machine's address on its network stands for the server's LAN address (coturn can't relay on
# loopback: the library refuses it), and a made-up public address, which home-net puts on this machine
# (on lo, while the servers run), for the router's; the servers find it by name (PUBLIC_HOST;
# compose.test.yaml maps it), as they would a real one.
TEST_IP=$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')
[ -n "$TEST_IP" ] || { echo "no network address"; exit 1; }
export TEST_PUBLIC_IP=5.5.5.5 PUBLIC_HOST=netplay-test.invalid PUBLIC_IP= TURN_SECRET=test-only-secret-0123456789
export HOME_LAN_IP=$TEST_IP COMPOSE_PROFILES=home
compose="docker compose -p conker-netplay-test -f $here/compose.yaml -f $here/compose.build.yaml -f $here/compose.test.yaml"
# However it ends, the servers stop.
trap '$compose down > /dev/null 2>&1' EXIT
trap 'exit 130' INT TERM
$compose up -d --build --wait > compose.txt 2>&1 || { cat compose.txt; echo "the servers didn't start"; exit 1; }
# coturn waits for home-net (which installs nftables first).
i=0
until $compose logs turn 2>&1 | grep -q "behind NAT"; do
    i=$((i + 1))
    [ $i -le 120 ] || { $compose logs > servers.txt 2>&1; echo "coturn didn't start"; exit 1; }
    sleep 0.5
done
sleep 1

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

# A joiner with a password the server didn't make: coturn refuses it, and relay-only it can't connect.
"$probe" host room --reads 60 $ice > bad-password.host.txt 2>&1 &
host=$!
room=$(room_of bad-password.host.txt)
"$probe" join "$room" --reads 60 $ice --turn-password not-the-servers > bad-password.join.txt 2>&1
bad_status=$?
kill $host 2>/dev/null

# A TURN client of coturn's own (turnutils_uclient) with good credentials, asking to relay to <peer>:<port>.
uclient() {
    user="$(($(date +%s) + 600)):0123456789abcdef"
    password=$(printf '%s' "$user" | openssl dgst -sha1 -hmac "$TURN_SECRET" -binary | base64)
    # (Not as nobody, coturn's user: home-net's guard would hold it to the relay ports too.)
    timeout 10 docker run --rm --user 1000 --network host --entrypoint turnutils_uclient coturn/coturn:4.6.3 \
        -u "$user" -w "$password" -p 3478 -e "$1" -r "$2" -n 3 -m 1 "$TEST_IP" > /dev/null 2>&1
}
# The deny list: a public address is relayed to; private ones and cloud metadata aren't.
for peer in 8.8.8.8 10.1.2.3 169.254.169.254 192.168.1.1 "$HOME_LAN_IP"; do
    uclient "$peer" 3480
done
$compose logs turn > turn.txt 2>&1
denied_ok=1
grep -q "peer IP 8.8.8.8 denied" turn.txt && { echo "  coturn denies a public address"; denied_ok=0; }
for peer in 10.1.2.3 169.254.169.254 192.168.1.1 "$HOME_LAN_IP"; do
    grep -q "peer IP $peer denied" turn.txt || { echo "  coturn doesn't deny $peer"; denied_ok=0; }
done
if [ $denied_ok -eq 1 ]; then echo "denied-peers: passed"; else echo "denied-peers: FAILED"; failed=1; fi
# home-net's guard: relayed to this machine (its public address), a port outside the relay range gets
# nothing; one inside it does.
relayed_to() {
    python3 -c 'import socket, sys; s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((sys.argv[1], int(sys.argv[2]))); s.settimeout(12)
try: s.recvfrom(2048); print("received")
except socket.timeout: print("nothing")' "$TEST_PUBLIC_IP" "$1" > "listen-$1.txt" 2>&1 &
    sleep 0.5
    uclient "$TEST_PUBLIC_IP" "$1"
    wait
}
relayed_to 39999
relayed_to 49659
if grep -q nothing listen-39999.txt && grep -q received listen-49659.txt; then echo "guard: passed"; else
    echo "guard: FAILED (outside the range: $(cat listen-39999.txt); inside: $(cat listen-49659.txt))"; failed=1; fi

$compose logs > servers.txt 2>&1
# ...and it was coturn that refused it (its credentials' user ends with the joiner's id).
bad_id=$(sed -n 's/^registered with .* as \([0-9a-f]*\).*/\1/p' bad-password.join.txt)
if [ $bad_status -ne 0 ] && grep -q "rendezvous" bad-password.join.txt && [ -n "$bad_id" ] &&
    grep "$bad_id" servers.txt | grep -q "check_stun_auth"; then echo "bad-password: passed"; else echo "bad-password: FAILED"; failed=1; fi

# Both looked the name up.
if [ "$(grep -c "public address $TEST_PUBLIC_IP\$" servers.txt)" = 2 ]; then echo "public-host: passed"; else
    echo "public-host: FAILED"; failed=1; fi
[ $failed -eq 0 ] && echo "all passed" || echo "FAILED"
exit $failed
