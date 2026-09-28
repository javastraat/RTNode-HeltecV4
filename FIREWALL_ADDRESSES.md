# Firewall Address Taxonomy

Every packet that reaches the firewall has its addresses extracted, checked against the whitelists (WL#1 + WL#2), and conditionally seeded.

## Two tiers

| Tier | Purpose | Examples |
|------|---------|----------|
| **Tier 1 — seed + check** | User-facing destinations. Added to WL#2 when the packet passes. Checked for `any_known`. | `destination_hash`, `link_id`, queried destination, ratchet hash |
| **Tier 2 — check only** | Relay infrastructure. Can PERMIT a packet through if already known, but NEVER added to WL#2. | `transport_id` |
| **Removed** | Useless or harmful. | `getTruncatedHash()` (per-packet content hash, not an identity) |

---

## By packet type

### ANNOUNCE (`packet_type == 1`)

A node telling the network "I exist, here's how to reach me."

| Field | What it is | Seed? | Check? |
|-------|------------|:-----:|:------:|
| `packet.destination_hash()` | The hash being announced (the destination's identity) | ✅ | ✅ |
| `packet.transport_id()` | Relay transport instance (HEADER_2 only) | ❌ | ✅ |
| Ratchet pubkey hash | `Identity::truncated_hash(packet.data().mid(84,32))` — the destination's ratchet encryption key (only when `context_flag == FLAG_SET` and `data().size() >= 116`) | ✅ | ✅ |

### LINKREQUEST (`packet_type == 2`)

A node asking another node to establish an encrypted link.

| Field | What it is | Seed? | Check? |
|-------|------------|:-----:|:------:|
| `packet.destination_hash()` | The destination being requested | ✅ | ✅ |
| `packet.transport_id()` | Relay transport instance (HEADER_2 only) | ❌ | ✅ |
| `Link::link_id_from_lr_packet(packet)` | Link identifier — needed so the returning LRPROOF can pass the firewall | ✅ | ✅ |

### DATA (`packet_type == 0`)

General-purpose data. Path requests are DATA packets addressed to a control destination.

| Field | What it is | Seed? | Check? |
|-------|------------|:-----:|:------:|
| `packet.destination_hash()` | Where the packet is going | ✅ | ✅ |
| `packet.transport_id()` | Relay transport instance (HEADER_2 only) | ❌ | ✅ |
| Queried destination hash | `packet.data().left(16)` — present only when the DATA packet is a **path request**: `destination_hash` matches a control hash (path.request or tunnel.synthesize) AND `data().size() >= 16`. This is the hash the sender wants to discover a path to. | ✅ | ✅ |

### PROOF (`packet_type == 3`)

Delivery confirmation or link request proof (LRPROOF).

| Field | What it is | Seed? | Check? |
|-------|------------|:-----:|:------:|
| `packet.destination_hash()` | For LRPROOF: the `link_id` from the original LINKREQUEST. For regular proofs: the destination hash. | ✅ | ✅ |
| `packet.transport_id()` | Relay transport instance (HEADER_2 only) | ❌ | ✅ |

A proof from the backbone seeds nothing, and passes only when it answers
this node's traffic: a link id in the link table, a packet hash in the
reverse table, or one of the node's own receipts.

---

## How a path request is identified

A packet is a path request when:
1. It's a **DATA** packet (`packet_type == 0`)
2. Its `destination_hash` matches a **control hash** (`_control_hashes` — contains `path.request` and `tunnel.synthesize`)
3. Its data is at least **16 bytes** (the queried destination hash)

The packet's `destination_hash` is *who processes it* (the path.request endpoint, e.g. `6b9f6601`).  
The packet's first 16 bytes of data are *what it's asking about* (the queried destination, e.g. `0358192f` for Browser).

---

## What is NOT extracted from incoming packets

| Field | Why removed |
|-------|-------------|
| `packet.getTruncatedHash()` | This is a **per-packet content hash** (hash of the packet body), not a node identity. It is unique per packet and can never match any whitelist entry from another source. Extracting it from incoming packets and adding to WL#2 wastes a slot with garbage. |

## What IS whitelisted at forwarding time (not by packet extraction)

When the firewall node forwards a packet from LAN to WAN, the **return path** must be whitelisted so the proof/reply can come back through the firewall.

| Field | When added | Why |
|-------|------------|-----|
| `packet.getTruncatedHash()` | At **forwarding time** (both FWD-TRANSPORT and FWD-LOCAL paths), when creating the `ReverseEntry` for the return route | The returning PROOF packet uses this hash as its `destination_hash`. If it's not in WL#2, the firewall silently drops the proof. |

This is NOT done during the per-packet address extraction loop — it's done at the point where we decide to forward the packet and create a `ReverseEntry`:

```cpp
// At both reverse-entry creation sites:
Bytes th = packet.getTruncatedHash();
_reverse_table.push_back({th, reverse_entry});
// Whitelist so the return proof isn't blocked:
_firewall_mentioned_addresses.push_back(th);
```

---

## Control hash filtering

Addresses matching `_control_hashes` (path.request, tunnel.synthesize) are **never added to WL#2**. These are infrastructure plumbing, not user traffic. Seeding them would allow all backbone path requests to pass — which is how the ~80-path-request flood happened.

---

## Firewall logic summary

```
For every packet:
  1. Extract Tier 1 addresses (seed + check)
  2. Extract Tier 2 addresses (check only)
  3. any_known = any Tier 1 or Tier 2 address is in WL#1 or WL#2,
                 or is a link id in the link table
  
  If backbone:
    PROOF answering us (destination in the reverse table, or one of our
      own receipts) → PASS, seed nothing
    any_known == false → BLOCK
    any_known == true  → PASS, seed Tier 1 addresses into WL#2
                         (a PROOF seeds nothing: it names only what is known)
  
  If local (LoRa, local TCP server, Bluetooth):
    Always PASS, seed Tier 1 addresses into WL#2
```

## Keeping what is in use

Each whitelist holds 200 addresses and is culled from the front. Every hit
(a check that finds the address, or a seed of one already there) moves the
address to the back, so an address still in use is never the one culled.
Link ids are also known through the link table, which a link enters only
when its request passed this filter and leaves when it goes stale — so a
link keeps working however many new addresses the LAN mentions meanwhile
(`wan-link-survives-churn` in [CONTRACT_TESTS.md](CONTRACT_TESTS.md)).
