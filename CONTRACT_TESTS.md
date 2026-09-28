# RTNode Contract Tests

Read with [CORE_PRINCIPLES.md](CORE_PRINCIPLES.md) (the firewall rules),
[FIREWALL_ADDRESSES.md](FIREWALL_ADDRESSES.md) and
[PERFORMANCE_STRATEGY.md](PERFORMANCE_STRATEGY.md) (the budgets). This file
covers whether the node does what the apps on either side of it rely on:

- **The LAN routes freely.** Announces, path discovery, links and resources
  pass between every pair of LAN media: RTNode's local TCP server, Bluetooth
  (Prns native; Columba's protocol is covered by `tests/bench_ble_columba.py`)
  and LoRa.
- **The WAN firewall holds.** WAN packets without a whitelisted address die
  at reception; unsolicited WAN announces never reach the LAN; LAN traffic
  goes everywhere and teaches the whitelists; the replies it asked for get
  back in.
- **Links and resources work** across every boundary above, both ways, and
  keep working while the LAN churns.
- **WiFi, Bluetooth and LoRa coexist**: all three carry traffic at once and
  the node stays healthy.

Everything runs from `tests/bench_contracts.py`, which starts one
`tests/contract_agent.py` per endpoint. An agent is a Reticulum instance on
exactly one medium, driven over stdin with JSON commands; it owns an
announced destination and a *silent* one that is never announced, so the
only way to find the silent one is a path request that the agent answers.

## The bench

| Piece | What | Notes |
|---|---|---|
| Node | V4.2, `rtnode_heltec_v4_bench` | backbones off, local TCP server on 4242, TX power 2 dBm; Bluetooth on (the default). For the WAN suite, build with `PLATFORMIO_BUILD_FLAGS="'-DRTNODE_BENCH_WAN_HOST=\"<Mac IP>\"' -DRTNODE_BENCH_WAN_PORT=4290"` |
| Node log | host-timestamped serial log (`<time> <line>` per line) | `--node-log`; the WAN flood and the soak read the node's own `[RES]`, `[STALL]`, table and whitelist reports from it |
| TCP | agents on RTNode's local TCP server | `tcp`, `tcp2`, … (a trailing digit starts another agent on the same medium) |
| Bluetooth | prnsd 0.3.7 on the Mac, `tests/prnsd/config`; agent on its loopback port 4270 | start prnsd from Terminal: macOS gives Bluetooth only to apps it can ask |
| LoRa | stock RNode (Heltec V3, fw 1.86) on a serial port | 914.875 MHz, SF10, 125 kHz, CR 4/5, 2 dBm |
| WAN | stand-in backbone: `../.venv/bin/rnsd --config tests/wan-backbone` | one TCP server on 4290 (gateway mode, no ingress control) and nothing else, so no test traffic leaves the bench; the WAN agents connect to it too |

```
../.venv/bin/python tests/bench_contracts.py lan  --media tcp,prnsd,lora --rnode-port /dev/cu.usbserial-0001
../.venv/bin/python tests/bench_contracts.py wan  --media tcp,tcp2 --node-log <serial log>
../.venv/bin/python tests/bench_contracts.py soak --media tcp,prnsd,lora --rnode-port /dev/cu.usbserial-0001 \
    --node-log <serial log> --soak-s 1800
```

Results go to `tests/bench-results/<time>-contracts-<suite>/results.json`.

## The checks

**lan**, for every ordered pair of agents:

| Check | Passes when |
|---|---|
| announce | A's announce reaches B |
| path | A finds B's silent destination (skipped through prnsd unless `--prnsd-gateway`: see below) |
| link | A's Link to B carries 3 echoes (400 B; 200 B with LoRa) and a resource (20 kB; 1.5 kB with LoRa) that B receives intact (SHA-256) |
| packet | A sends B 3 single packets (no link; 10 s apart on LoRa, as an app sends messages); all arrive and all 3 proofs come back |

**wan**, with LAN agents `a`, `b` and a WAN agent `w`:

| Check | Passes when |
|---|---|
| wan-path-unmentioned | `w` finds no path to `a`'s silent destination: the request dies at the boundary |
| wan-announce-held | `w`'s announce reaches no LAN agent |
| lan-announce-out | each LAN agent's announce reaches `w` |
| lan-path-wan, lan-path-wan-silent | `a` finds `w`'s destinations, announced and silent |
| lan-path-lan | `a` finds `b`'s silent destination, which puts it on the whitelist |
| lan-link-wan, wan-link-lan | links both ways carry echoes and an intact resource |
| lan-packet-wan, wan-packet-lan | single packets both ways arrive and their proofs come back (through the reverse table) |
| wan-path-mentioned | `w` finds `b`'s silent destination once the LAN has mentioned it |
| wan-announce-known | `w`'s next announce reaches `a`, which asked for it |
| wan-link-survives-churn | after `b` announces 260 fresh destinations (more than the whitelists hold), open links still carry echoes, the WAN side speaking first as well as the LAN side |
| wan-reaches-quiet-lan | after the churn, `w` can still open a link to `a`, which has stayed quiet |
| wan-flood | a WAN agent announces fresh destinations at 20/s for 60 s: no LAN agent hears one, echoes on a LAN–WAN and a LAN–LAN link keep coming back (≥ 90 %), the node neither reboots nor panics |

**soak**, all at once for `--soak-s`:

| Flow | Traffic | Passes when |
|---|---|---|
| wifi<->bt | TCP ↔ Prns: echo every 1 s (400 B), resource every 60 s (20 kB) | loss ≤ 1 %, every resource intact |
| wifi<->lora | TCP ↔ LoRa: echo every 30 s (100 B) | loss ≤ 10 % |
| bt<->lora | Prns ↔ LoRa: echo every 60 s (100 B) | loss ≤ 10 % |
| wan<->wifi | WAN ↔ TCP: echo every 2 s (400 B), resource every 120 s (20 kB) | loss ≤ 1 %, every resource intact |
| soak-node | the node's own reports | no reboot, no panic |

At SF10 an echo round over LoRa (request, proof, reply) is about 5 s of
airtime; the two LoRa flows keep the channel about a quarter busy.

## What is not an RTNode fault

- **prnsd does not look for unknown paths in full mode.** A transport node
  forwards a request for a destination it does not know only when it arrived
  on a gateway-mode interface (RNS `DISCOVER_PATHS_FOR`; Prns
  `recursively_forwards_unknown_paths`). `tests/prnsd/config` sets `mode =
  gateway`; a prnsd started with it can run the path checks through Bluetooth
  (`--prnsd-gateway`). The prnsd on the bench on 2026-09-28 ran an older copy
  of the config, so those checks were skipped. Bluetooth path discovery
  itself was shown with Columba phones (b0c7a86).
- **RNS 1.5.2 holds repeat path requests for 45 s.** A transport node that
  has passed on a request for a destination batches further requests for it
  until an answer comes or `PATH_REQUEST_GATE_TIMEOUT` (45 s) passes. LAN
  path requests reach the stand-in too (LAN packets go everywhere) and the
  node answers only the LAN requester, so a WAN request for the same
  destination within 45 s — up to 50 s, as the gate entry goes at the next
  5-second table cull — never reaches the node. `wan-path-mentioned` waits
  55 s.
- **LoRa is half duplex.** A step that starts while the last one's frames
  (a link close, a late proof) are still on air collides with them. The
  harness leaves `--lora-settle` (5 s) of quiet after each LoRa step; a lost
  single frame still fails a check, as it would for an app.

## Found and fixed, 2026-09-28

All on `bench/instrumentation`, found by these suites:

1. **Link proofs sent back the way they came** (51b9262). A firewall-mode
   block sent every proof on a link to the link's outbound interface, in
   either direction, before link transport forwarded it properly. On LoRa a
   destination's delivery proof went back on air, colliding with its next
   frame; TCP → LoRa lost 2 of 3 echoes.
2. **Link requests forwarded twice** (21292d6). A firewall-mode block sent
   every LAN link request again, raw, and replaced the link entry forwarding
   had made. Over LoRa the copy went on air while the destination was
   sending its link proof: TCP → LoRa links failed outright.
3. **Split LoRa packets glued to other packets' bytes** (a6d2e37). A second
   half with no first half was taken as a first half, and `loop()` staged
   every received packet through the reassembly buffer. A 275-byte echo
   arrived with its first 21 bytes from the proof before it, typed as a
   proof. `[RES]` now counts discarded halves (`split_drop`).
4. **In-use addresses evicted from the whitelists** (7b682b2). The lists
   were culled oldest-added first at 200 and a hit never refreshed an entry:
   after 260 fresh LAN announces, WAN-to-LAN echoes on an open link went
   from 3/3 to 0/3. Hits now refresh an entry, and link ids in the link
   table count as known.
5. **Busy paths evicted from a 24-entry table** (9afcd4e). Eviction went by
   bitrate over hops, so LoRa destinations went first and multi-hop WAN ones
   next; the churn evicted the path to a WAN endpoint in use and the next
   link to it failed. Eviction now goes by last use, and with Reticulum's
   heap in PSRAM (V4) the table holds 256.

6. **Backbone proofs skipped the whitelist** (5eb13db). WAN proofs
   were exempt, so an unrelated one reached transport processing and the
   packet hashlist, and its destination was added to WL#2 — a WAN peer could
   churn the whitelist with junk proofs. A WAN proof now passes only when it
   answers this node's traffic (a link in the link table, a packet in the
   reverse table, one of its own receipts) and seeds nothing.
   `lan-packet-wan` covers the reverse-table case.

## Open

- **Loop time.** `[STALL]` and `[RES]` now attribute time to `rx` (Reticulum
  processing a received packet), `jobs` (`reticulum.loop()`: Transport jobs,
  every interface's `loop()`, the filesystem), `ble` and `display` (measured
  2026-09-28 on the V4.2 bench build):
  - the display costs 31 ms per push, about 6 pushes a second: ~11 s of
    every minute on the loop core, and any packet can wait up to 31 ms
    behind it;
  - `reticulum.loop()` takes ~25 s of every minute even idle — ~0.24 ms on
    each of ~1,800 passes a second, with Transport's periodic jobs on top
    (100–470 ms per run under churn, most of the once-a-minute stall
    alongside ~0.5 s of path-persistence flash writes);
  - under a 20/s WAN announce flood, receive processing is ~3 ms per packet
    and the loop's p50 LAN → WAN echo RTT rises from ~200 ms to ~750 ms.
  Nothing is lost and the soak passes, but the loop is far over its 50 ms
  budget. Slow passes now say where they went — `[RLOOP]` (a
  `reticulum.loop()` pass of 100 ms or more: housekeeping, interface loops,
  filesystem, Transport), `[JOBS]` (a Transport jobs run of 100 ms or more,
  by section) and `[RJOBS]` (cache clean or persist of 50 ms or more) — and
  under the WAN suite they show three things:
  - the TCP interfaces process a whole burst of received packets in one
    pass (up to 360 ms under the flood) while the other radios wait; a
    per-pass budget in `TcpInterface::loop()` would bound that;
  - releasing one held announce costs ~100 ms (verification and path work),
    every 5 s during a burst;
  - the once-a-minute stall is `persist_data()` (0.3–1.2 s) plus cache clean
    (up to 0.2 s) plus the table cull (0.1–0.17 s).
  The display question stands too: does it need 6 pushes a second?
- **Direct LoRa neighbours may be repeated (code review, untested).**
  FWD-CHECK (firewall mode, from v1.0.41 like fixes 1, 2 and 6) forwards a
  LAN packet that has no transport header whenever the node has a path to
  its destination — including when that path leads back out of the
  interface it came in on. Two LoRa devices talking directly, with the node
  in range and holding a path to the receiver, would have each data packet
  and link request sent again by the node on LoRa; a link set up that way
  gets an entry whose two interfaces are both LoRa, and link transport then
  repeats every packet of it. Python RNS transport nodes never forward a
  packet without a transport header. Showing it needs two LoRa endpoints
  besides the node (the bench has one); the likely fix is to skip FWD-CHECK
  when the outbound interface is the receiving one on a shared medium.
- **Whitelists are 200 entries with a linear search.** In use they no longer
  evict, but a LAN with more than 200 addresses active at once would push
  idle ones out, and the search cost grows with the cap. A set would allow a
  larger cap on PSRAM boards.
- **README's interface modes are stale**: it says LoRa is `MODE_GATEWAY`
  and the backbone `MODE_BOUNDARY`; the firmware sets every interface to
  `MODE_FULL`.

## Results log

| Date | Firmware | Suite | Result | Notes |
|---|---|---|---|---|
| 2026-09-28 | cc6371e | lan tcp,prnsd | 4/6 | paths through prnsd unanswered: prnsd in full mode (above) |
| 2026-09-28 | cc6371e | lan tcp,prnsd,lora | 12/14 | TCP → LoRa echoes 1/3, LoRa → prnsd 0/3: fixes 1 and 3 |
| 2026-09-28 | + 51b9262, a6d2e37 | lan tcp,prnsd,lora | 11/14 | TCP → LoRa and LoRa → TCP links failed: fix 2, and the harness's own collision after a link close (settle time) |
| 2026-09-28 | + 21292d6 | lan tcp,prnsd,lora | **14/14** | LoRa echo RTT p50 8.1–8.8 s (SF10), 1.5 kB resources in 21–23 s |
| 2026-09-28 | 21292d6 + stand-in backbone | wan tcp,tcp2 | 14/15 | wan-path-mentioned: the RNS path gate (above); flood PASS, 1201 announces, none leaked |
| 2026-09-28 | same | wan tcp,tcp2 | 14/16 | wan-link-survives-churn (WAN first) 0/3: fix 4; flood phase's LAN–WAN link failed to open: fix 5 |
| 2026-09-28 | + 7b682b2, 9afcd4e | wan tcp,tcp2 | **16/16** | flood: 1201 announces, 0 leaked; echoes LAN–LAN 41/41 (p50 310 ms), LAN–WAN 33/33 (p50 753 ms); heap min 118 KB; stall max 2.0 s |
| 2026-09-28 | same | soak tcp,prnsd,lora + wan, 30 min | **5/5** | WiFi↔BT 1643/1643 echoes (p50 380, p95 614 ms), 29/29 resources; WAN↔WiFi 891/891 (p50 236, p95 602 ms), 14/14 resources; WiFi↔LoRa 59/60 and BT↔LoRa 29/30 (p50 5.4 s); no reboot or panic, heap min 118 KB, stall max 992 ms, no Bluetooth disconnect, no LoRa queue or split drops |
| 2026-09-28 | + backbone proof filter (5eb13db), loop-time causes | wan tcp,tcp2 | **18/18** | with lan-packet-wan and wan-packet-lan (3/3 arrived, 3/3 proved each way); flood: 1201 announces, 0 leaked, LAN–WAN echo p50 752 ms |
| 2026-09-28 | same | lan tcp,prnsd,lora | **20/20** | with single packets on every pair (3/3 arrived, 3/3 proved, LoRa included); LoRa echo RTT p50 7.8–9.8 s |
