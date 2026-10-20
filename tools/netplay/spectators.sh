#!/bin/sh
# The spectators check (netplay, issue #21): a spectator (--netplay-watch) gets every input of a match and sends none,
# and must see exactly the game the players do (its hash log a solo replay's), never holding them up. Run it after
# building with CONKER_NETPLAY=ON:
#
#   tools/netplay/spectators.sh [--exe host/build/ConkerRecomp] [--rom conker/baserom.us.z64]
#                               [--reads 1800] [--seed 7] [--timeout 300] [--out <folder>]
#
# Each match is a host and a joiner, each with its own port's pads from a made-up 2-port recording (the other's
# scrambled), and a spectator given a recording of other pads altogether (it must play none of its own), all
# headless and --fast:
#   watch      a spectator from the start: all three must match the solo replay
#   late       the spectator joins as the host stops a while at read 900 (--netplay-stall-at): it must load the
#              host's game and match the solo replay from the read it loaded before; the players, all of it
#   desync     the spectator changes a byte of its memory at read 600: the host must send it its game alone (the
#              players never told), which it loads; the players match solo, the spectator from the read it loaded
#   rollback   the host's --rollback 8, the joiner 40 ms of lag: the spectator, which never predicts, matches solo
#   leave      the spectator quits at read 900: the players play on to the end, and match solo
#   refused    a host with --netplay-no-spectators refuses a spectator, and the players play on
# It exits 0 if all is as it should be, 1 if not. The logs are kept in --out; without it they're in a temporary
# folder, deleted if all passed. It uses UDP ports 27340 to 27345.
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
        -h|--help) sed -n '2,/^set -u$/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[ -x "$EXE" ] || { echo "no game at $EXE: build it with CONKER_NETPLAY=ON first" >&2; exit 2; }
[ -f "$ROM" ] || { echo "no ROM at $ROM" >&2; exit 2; }
# Absolute: the game changes its working folder.
EXE=$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")
ROM=$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")
KEEP=1
[ -n "$OUT" ] || { OUT=$(mktemp -d "${TMPDIR:-/tmp}/netplay-spectators.XXXXXX"); KEEP=0; }
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

# loaded <name>: the read before which the spectator <name> last loaded the host's game.
loaded() {
    sed -n "s/^\[netplay\] .*the host's game was loaded before read \([0-9]*\).*/\1/p" "$OUT/$1.out" | tail -n 1
}

# match <name> <port> [host options] -- [spectator options]: the host and the joiner, then the spectator.
match() {
    m=$1 m_port=$2; shift 2
    host_options=
    while [ $# -gt 0 ] && [ "$1" != -- ]; do host_options="$host_options $1"; shift; done
    [ $# -gt 0 ] && shift
    # (lib.sh's start uses $name: these have names of their own.)
    # shellcheck disable=SC2086
    pads=1.pads start "$m-host" --netplay-host "$m_port" --netplay-address 127.0.0.1 $host_options
    code=$(code_of "$m-host")
    # ($wait_pattern, $join_options and $watch_first: set before, and cleared after, the one match they're for. A
    # spectator first comes before the joiner, and is refused or in before the match can start.)
    if [ -n "${watch_first:-}" ]; then
        pads=other.pads start "$m-watch" --netplay-join "$code" --netplay-watch "$@"
        wait_for "$m-watch" "$watch_first"
    fi
    # shellcheck disable=SC2086
    pads=2.pads start "$m-join" --netplay-join "$code" ${join_options:-}
    [ -n "${wait_pattern:-}" ] && wait_for "$m-host" "$wait_pattern"
    [ -z "${watch_first:-}" ] && pads=other.pads start "$m-watch" --netplay-join "$code" --netplay-watch "$@"
}

echo "watch, late, desync"
match watch 27340
wait_pattern="the game stops"
match late 27341 --netplay-stall-at 900:6000
wait_pattern=
match desync 27342 -- --netplay-desync-at 600
if finish "was the last: quitting" watch-host watch-join watch-watch late-host late-join late-watch desync-host desync-join desync-watch; then
    for copy in watch-host watch-join watch-watch late-host late-join desync-host desync-join; do
        same "$copy" || failed=1
    done
    if ! grep -q "the match starts: 2 players, this is a spectator" "$OUT/watch-watch.out"; then
        echo "  watch-watch didn't watch from the start"
        failed=1
    fi
    at=$(loaded late-watch)
    if [ -z "$at" ] || ! grep -q "watching, a match of 2 under way" "$OUT/late-watch.out"; then
        echo "  late-watch didn't come in under way"
        failed=1
    else
        same_from late-watch solo "$at" || failed=1
    fi
    at=$(loaded desync-watch)
    if [ -z "$at" ] || ! grep -q "spectator 1's game memory isn't this one's" "$OUT/desync-host.out"; then
        echo "  desync-watch wasn't set right"
        failed=1
    elif grep -q "desync recovery\|will load" "$OUT/desync-join.out"; then
        echo "  the joiner was told of the spectator's desync"
        failed=1
    else
        same_from desync-watch solo "$at" || failed=1
    fi
else
    failed=1
fi

echo "rollback, leave, refused"
join_options="--netplay-fake-network lag=40"
match rollback 27343 --rollback 8
join_options=
match leave 27344 -- --stop-after 900
watch_first="couldn.t join"
match refused 27345 --netplay-no-spectators
watch_first=
if finish "was the last: quitting\|couldn't join" rollback-host rollback-join rollback-watch leave-host leave-join leave-watch \
    refused-host refused-join refused-watch; then
    for copy in rollback-host rollback-join rollback-watch leave-host leave-join refused-host refused-join; do
        same "$copy" || failed=1
    done
    if ! grep -q "spectator 1 left" "$OUT/leave-host.out"; then
        echo "  the host didn't see the spectator leave"
        failed=1
    fi
    if ! grep -q "couldn't join: the host takes no spectators" "$OUT/refused-watch.out"; then
        echo "  the spectator wasn't refused"
        failed=1
    fi
else
    failed=1
fi

if [ $failed -ne 0 ]; then
    echo "FAILED (logs: $OUT)"
    exit 1
fi
if [ $KEEP -eq 1 ]; then echo "logs: $OUT"; else rm -rf "$OUT"; fi
echo "passed: every spectator saw the players' game, coming in under way and after a desync too, and nobody waited for one"
