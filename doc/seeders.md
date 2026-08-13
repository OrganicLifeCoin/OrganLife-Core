# Seeders (peer discovery)

This codebase currently ships with **no DNS seeds or fixed IP seeds** for mainnet or testnet (see `src/chainparams.cpp`). Until public VPS listeners exist, every early node must be given at least one reachable peer explicitly.

- **Manual peers**: add known nodes in `organiclifecoin.conf` using `addnode=` / `seednode=`.
- **Hard-coded IP seeds**: populate `vFixedSeeds` in `src/chainparams.cpp` (advanced).
- **DNS seeders**: run one (or more) DNS seed services and add hostnames to `vSeeds` in `src/chainparams.cpp`.

## Minimal bootstrap (no DNS seeder yet)

1. Bring up the first public full node with a static IP.
2. On every other testnet node/wallet, add that reachable peer, for example:

```
addnode=<testnet-vps-ip>:49616
```

The equivalent command-line option is `-addnode=<testnet-vps-ip>:49616`. Once one node is reachable, connected nodes can learn about more peers via address gossip.

## DNS seeders (recommended for public networks)

DNS seeders are a separate service that:

- crawls the network (connects to nodes, checks if they’re reachable / on the right chain),
- and serves fresh peer IPs via DNS.

Typical flow:

1. Deploy a DNS seeder implementation on a server (often based on Bitcoin’s `dnsseed`).
2. Point it at your network (magic bytes, default port, etc).
3. Add the seeder hostname(s) in `src/chainparams.cpp`:

```
vSeeds.emplace_back("seed1.example.org", true);
```

Operate **at least 2** DNS seeders on different infrastructure for resilience.
