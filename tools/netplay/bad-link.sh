#!/bin/sh
# The bad link check (netplay, issue #40): one player's bad connection mustn't make the match worse for the
# others. Each player's input delay is their own (the host sizes it for how their inputs reach the others), and
# a LAN keeps a delay of 0. Run it after building with CONKER_NETPLAY=ON:
#
#   tools/netplay/bad-link.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                             [--reads 1800] [--seed 7] [--timeout 300] [--out <folder>]
#
# The matches run at the same time, headless, on the timeline's real time (30 reads a second, as a player's
# game), rollback 12 (the menu's), each copy with its own ports' pads (the others' scrambled):
#   lan        no lag: every port's delay is 0 from start to end, never changed, and nobody catches up
#   uneven     30 ms lag both ways, and 40 ms of jitter on the joiner's sends only (its inputs arrive unevenly,
#              the host's don't): the joiner's delay ends higher than the host's
# It exits 0 if every match gave each copy the same game as a replay of the recording alone and the delays went
# as above, 1 if not. The logs are kept in --out; without it they're in a temporary folder, deleted if all
# passed. It uses UDP ports 27370 to 27379.
set -u

cd "$(dirname "$0")/../.."
EXE=host/build/ConkerRecomp
ROM=conker/baserom.us.z64
READS=1800
SEED=7
TIMEOUT=300
OUT=
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) EXE=$2; shift 2 ;;
        --rom) ROM=$2; shift 2 ;;
        --reads) READS=$2; shift 2 ;;
        --seed) SEED=$2; shift 2 ;;
        --timeout) TIMEOUT=$2; shift 2 ;;
        --out) OUT=$2; shift 2 ;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-bad-link.XXXXXX"); KEEP=0; }
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
. tools/netplay/lib.sh

python3 tools/netplay/make_inputs.py "$OUT/inputs.pads" --reads "$READS" --seed "$SEED" --ports 2 --mouse --own-view || exit 2
python3 tools/netplay/make_inputs.py "$OUT/other.pads" --reads "$READS" --seed "$((SEED + 1))" --ports 2 --mouse --own-view > /dev/null || exit 2
scramble "$OUT/inputs.pads" "$OUT/other.pads" 2 "$OUT/" || exit 2

failed=0
echo "solo"
start solo
finish "was the last: quitting" solo || exit 1

# match <name> <UDP port> <host's fake network> <joiner's>: a 2-player match with rollback 12, on real time.
match() {
    paced=1 pads=1.pads start "$1-host" --netplay-host "$2" --netplay-address 127.0.0.1 --players 2 --rollback 12 \
        --netplay-fake-network "$3"
    paced=1 pads=2.pads start "$1-join" --netplay-join "$(code_of "$1-host")" --netplay-fake-network "$4"
}
# delays_of <name>: the input delays the copy <name> ended with, by port ("0, 0").
delays_of() {
    sed -n 's/^\[netplay\] input delays by port: //p' "$OUT/$1.out"
}

echo "lan, uneven (at once, $READS reads each on real time)"
match lan 27370 "lag=0" "lag=0"
match uneven 27371 "lag=30" "lag=30,jitter=40"
names="lan-host lan-join uneven-host uneven-join"
# shellcheck disable=SC2086
if finish "was the last: quitting" $names; then
    for name in $names; do
        same "$name" || failed=1
        echo "    $(sed -n 's/^\[netplay\] \(rollback: .*\)/\1/p' "$OUT/$name.out")"
        echo "    input delays by port: $(delays_of "$name")"
    done
    for name in lan-host lan-join; do
        if [ "$(delays_of "$name")" != "0, 0" ] || ! grep -q "input delay 0 reads" "$OUT/$name.out" ||
            grep -q "the input delay of" "$OUT/$name.out" || ! grep -q "caught up 0 times" "$OUT/$name.out"; then
            echo "  $name: the delay wasn't 0 throughout, or it caught up"
            failed=1
        fi
    done
    [ $failed -eq 0 ] && echo "  lan: every delay 0 throughout, no catching up"
    # The joiner's inputs are the uneven ones: its own delay is the higher (as both copies say).
    for name in uneven-host uneven-join; do
        host_delay=$(delays_of "$name" | cut -d, -f1)
        join_delay=$(delays_of "$name" | cut -d, -f2 | tr -d ' ')
        if [ -z "$host_delay" ] || [ -z "$join_delay" ] || [ "$join_delay" -le "$host_delay" ]; then
            echo "  $name: the joiner's delay (${join_delay:-?}) isn't higher than the host's (${host_delay:-?})"
            failed=1
        else
            echo "  $name: the joiner's delay is $join_delay, the host's $host_delay"
        fi
    done
else
    failed=1
fi

if [ $failed -eq 0 ]; then
    [ $KEEP -eq 0 ] && rm -rf "$OUT"
    echo "passed: each player's delay was their own, and a LAN's stayed 0"
    exit 0
fi
echo "logs: $OUT"
exit 1
