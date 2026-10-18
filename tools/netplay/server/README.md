# Netplay's servers

Players who connect through ICE (rather than by a join code with the host's address) need two servers:

- **The signalling server** (`netplay_server`, from `host/src/netplay/net/signal_server.cpp`): players
  register a random id with it. It passes GameNetworkingSockets' ICE signals between ids so that two
  players can find a way to each other. It never sees the game's packets. It uses little bandwidth.
- **coturn**, the TURN relay: when the players' networks don't let them connect directly, or when they
  choose relay-only (the default, so nobody sees anybody's address), their packets go through it.
  Netplay only sends inputs, so a whole match uses a few tens of KB/s.

Anyone can run them. The game will take the server's address as a setting.

## Running them

On a Linux machine with Docker and a public IP address:

```sh
cd tools/netplay/server
cp .env.example .env      # set PUBLIC_IP and TURN_PASSWORD
docker compose up -d --build
docker compose logs -f
```

Open in the firewall:

| Port | What |
|---|---|
| UDP 27100 | signalling (`SIGNAL_PORT`) |
| UDP and TCP 3478 | TURN |
| UDP 49160-49659 | the relays themselves (one per player and network interface while they play) |

Both containers use the host's network, as coturn's relay ports are a range that Docker's port mapping
handles badly.

`docker compose pull && docker compose up -d --build` updates them; `docker compose down` stops them.

## What's limited

- **Signalling:** a peer must register within 5 s, with an id nobody else has (nor had in the last
  hour). At most `MAX_PER_ADDRESS` connections from one IP address (8) and 4096 in all, and 20 signals a
  second each (bursts of 60). Anything else closes the connection. The server logs counts, never
  addresses.
- **coturn:** at most 200 KB/s per relay, `TURN_TOTAL_QUOTA` relays in all (400), and never into private
  or loopback networks (so it can't be used to reach the server's own network).

## For now (phase 1 of the relay work)

- The TURN user and password are fixed (`.env`), and players need them too. Next, the signalling server
  will hand out short-lived ones (coturn's `use-auth-secret`), along with rooms and room codes.
- The game itself doesn't connect through ICE yet: `netplay_probe` does (`host ice`, `join <ticket>`).

## Testing

`test.sh` starts these servers on this machine (with `compose.test.yaml`, which lets the relay reach
local addresses), plays relay-only matches through them with `netplay_probe`, checks that no player is
told an address but the relay's, and stops them:

```sh
cmake -S host/src/netplay/net -B build-net -G Ninja && cmake --build build-net
sh tools/netplay/server/test.sh build-net /tmp/relay-test
```

It uses the project name `conker-netplay-test` but the same ports (27100, 3478, 49160-49659), so don't
run it on a machine that is already hosting.
