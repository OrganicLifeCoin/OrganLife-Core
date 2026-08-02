# Seeders (peer discovery)

This codebase currently ships with a **minimal set of hard-coded seeds** (see `src/chainparams.cpp`). For early networks, you should still provide additional peers and/or run DNS seeders so nodes can find peers reliably.

- **Manual peers**: add known nodes in `organiclifecoin.conf` using `addnode=` / `seednode=`.
- **Hard-coded IP seeds**: populate `vFixedSeeds` in `src/chainparams.cpp` (advanced).
- **DNS seeders**: run one (or more) DNS seed services and add hostnames to `vSeeds` in `src/chainparams.cpp`.

## Minimal bootstrap (no DNS seeder yet)

1. Bring up 2–3 public full nodes (static IPs).
2. On every node/wallet, add at least one reachable peer, for example:

```
addnode=46.62.255.185:31616
addnode=195.201.34.89:31616
```

Once one node is reachable, new nodes can learn about more peers via addr gossip.

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
