# Netplay's servers

Players who play through a room (rather than by a join code with the host's address) need two servers:

- **The signalling server** (`netplay_server`, from `host/src/netplay/net/signal_server.cpp`): players
  register a random id with it. A host makes a room there and gets a room code (`7KQ2-M9XD`, no address
  in it) to send the others; they look the code up to find the host. The room lasts as long as the host
  stays connected. The server passes GameNetworkingSockets' ICE signals between ids so that two players
  can find a way to each other, and hands each player TURN credentials of their own. It never sees the
  game's packets. It uses little bandwidth.
- **coturn**, the TURN relay: when the players' networks don't let them connect directly, or when they
  choose relay-only (the default, so nobody sees anybody's address), their packets go through it.
  Netplay only sends inputs, so a whole match uses a few tens of KB/s.

Anyone can run them. The game takes the server's address (`host:27100`) as a setting: the Netplay menu's
Server, or `--netplay-server`.

## Running them

On a Linux machine with Docker and a public IP address, you need only `compose.yaml` and `.env.example`
from this folder: the server's image is published to GitHub's container registry
(`ghcr.io/easymod0/conker-netplay-server`, for amd64 and arm64) by the `netplay-server` workflow.

```sh
mkdir conker-netplay && cd conker-netplay
curl -fsSLO https://raw.githubusercontent.com/easymod0/CBFD-Recompiled-Netplay/netplay/tools/netplay/server/compose.yaml
curl -fsSL https://raw.githubusercontent.com/easymod0/CBFD-Recompiled-Netplay/netplay/tools/netplay/server/.env.example -o .env
nano .env                 # set PUBLIC_IP, and TURN_SECRET (openssl rand -hex 24)
docker compose up -d
docker compose logs -f
```

To build the server from a clone of this repository instead:
`docker compose -f compose.yaml -f compose.build.yaml up -d --build`.

Open in the firewall:

| Port | What |
|---|---|
| UDP 27100 | signalling (`SIGNAL_PORT`) |
| UDP and TCP 3478 | TURN |
| UDP 49160-49659 | the relays themselves (one per player and network interface while they play) |

Both containers use the host's network, as coturn's relay ports are a range that Docker's port mapping
handles badly.

`docker compose pull && docker compose up -d` updates them; `docker compose down` stops them.

## What's limited

- **Signalling:** a peer must register within 5 s, with an id nobody else has (nor had in the last
  hour). At most `MAX_PER_ADDRESS` connections from one IP address (8) and 4096 in all, and 20 packets a
  second each (bursts of 60). One room each. From one IP address, at most 10 lookups of room codes that
  aren't there, then one each 6 s, so codes can't be guessed (a code is one of a trillion, and isn't
  given out again for an hour after its room closes). Anything else closes the connection. The server
  logs counts, never addresses or codes.
- **coturn:** at most 200 KB/s per relay, `TURN_TOTAL_QUOTA` relays in all (400), and never into private
  or loopback networks (so it can't be used to reach the server's own network).

## The relay's credentials

coturn takes no fixed user: it checks credentials made from `TURN_SECRET`, which only it and the
signalling server know (coturn's `use-auth-secret`). The signalling server gives each player a user
`<expiry>:<id>` and its password (HMAC-SHA1 of the user, base64) as they register; they work for
`TURN_HOURS` (12 by default), and coturn refuses them after. So only players of this server can use the
relay, and only for a while. Changing the secret stops every credential given out so far.

## Testing

`test.sh` starts these servers on this machine (with `compose.test.yaml`, which lets the relay reach
local addresses), plays relay-only matches through a room with `netplay_probe` (with the credentials
the server hands out), checks that no player is told an address but the relay's and that coturn refuses
a password the server didn't make, and stops them:

```sh
cmake -S host/src/netplay/net -B build-net -G Ninja && cmake --build build-net
sh tools/netplay/server/test.sh build-net /tmp/relay-test
```

It uses the project name `conker-netplay-test` but the same ports (27100, 3478, 49160-49659), so don't
run it on a machine that is already hosting.
