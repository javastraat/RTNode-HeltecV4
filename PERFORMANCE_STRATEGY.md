# RTNode Performance and Memory Strategy

Read with [CORE_PRINCIPLES.md](CORE_PRINCIPLES.md) and the workspace
`DESIGN_PRINCIPLES.md`. This file sets the budgets, the measurements, and the
order of work for any change that spends RTNode's CPU, RAM, flash, airtime or
radio time.

It was written on 2026-09-27, ahead of three pieces of work that all draw on
the same budget:

- **#43** — WiFi degrades under transport load (30–55 % ICMP loss, backbone
  link drops), stable when idle.
- **#44** — Heltec V4 boards with 8 MB octal PSRAM run with no PSRAM at all.
- **Bluetooth LE** — an ad-hoc interface speaking the Prns-native protocol,
  with Columba compatibility.

Each can be made to work on its own and still break the others. Bluetooth in
particular takes RAM from the same pool WiFi uses and airtime from the same
2.4 GHz radio, so it must not start until #43 is understood and the harness
below exists.

## The machine we are budgeting

| Resource | Heltec V4 (2 MB PSRAM) | Notes |
|---|---|---|
| CPU | 2 × Xtensa LX7, 240 MHz | Core 0: WiFi, lwIP, Bluetooth controller (pinned by the SDK). Core 1: Arduino `loop()` — all RTNode and Reticulum work. |
| Internal SRAM | 320 KB heap region. Static use 73,776 B (22.5 %) in the 2026-09-27 build. 170–200 KB free in field logs. | WiFi RX buffers, lwIP, task stacks, and (later) the Bluetooth controller and host all live here. Watchdog sheds state at 28 KB free and resets at 20 KB. |
| PSRAM | 2 MB QSPI, meant to hold the TLSF pool and so all Reticulum objects — but field logs show it ~98 % free, so the pool is not being created (see "PSRAM is not backing the Reticulum heap"). 8 MB octal on newer boards, not initialised by today's build (#44). | Not usable for DMA. `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` lets WiFi/lwIP spill into it. |
| Flash | 16 MB; app 1.33 MB (20.3 %) | A flash write or erase disables the cache and stalls **both** cores for its duration. |
| 2.4 GHz radio | WiFi only today | Bluetooth will time-share this one radio with WiFi. |
| LoRa | e.g. SF10 / BW125 ≈ 1 kbps | One 167-byte announce is 0.6–2 s of airtime. |

## Rules for every change

1. **Measure before theorising.** A performance problem is investigated by
   making its stalls and drops report themselves (see Instrumentation), not by
   reasoning from symptoms. #43 already has several plausible causes; only
   measurement separates them.
2. **State the cost up front.** Every change says what it adds to internal
   heap (steady and peak), PSRAM, static RAM, flash, loop time per packet, and
   airtime — and the harness confirms it.
3. **Nothing blocks `loop()`.** Waiting on hardware or the network completes
   on an event: the SX1262 TxDone interrupt, a Bluetooth write confirmation, a
   socket becoming writable. No busy-waits, no blocking connects or writes.
   (DESIGN_PRINCIPLES §1: name the event.)
4. **Every queue is bounded, allocated at boot, and counts its drops.** The
   first drop is logged; all drops are counted and appear in the resource line.
5. **No per-packet heap allocation where a pool will do; no node-based
   containers** ([ESP32_HEAP_FRAGMENTATION.md](ESP32_HEAP_FRAGMENTATION.md)).
6. **Work that scales with what peers send is bounded where it enters.** The
   firewall already does this for WAN traffic. LAN-side *transport* peers — a
   `rnsd` on the local TCP server today, a `prnsd` laptop over Bluetooth
   tomorrow — can inject backbone-scale announce load through the trusted
   side (CORE_PRINCIPLES.md, "LAN-Side Rule"). They get an explicit ingress
   budget.
7. **Internal SRAM is for WiFi, Bluetooth, DMA and stacks.** Bulk state goes to
   PSRAM through the TLSF pool.
8. **Flash writes are budgeted like airtime.** No per-packet flash writes on
   paths whose rate a peer controls.
9. **Done means the harness passes on hardware**, on the private bench first
   (the workspace rule: production is not the test bed), not that it compiles.

## Budgets

Proposed values; the "baseline" column is filled in by step 1 of the order of
work. A change that moves a metric past its budget does not merge until the
budget is renegotiated here, in writing.

| Metric | Budget (proposed) | Baseline | Why this number |
|---|---|---|---|
| Min free internal heap, 30-min standard load | ≥ 48 KB | — | 20 KB above the 28 KB shed threshold |
| Largest free internal block | ≥ 24 KB | — | WiFi RX and lwIP need contiguous buffers |
| Static RAM (`.data` + `.bss`) | ≤ 25 % | 22.5 % | today plus Bluetooth tables |
| Main-loop iteration, p99 / max | ≤ 5 ms / ≤ 50 ms | — | keeps TCP, LoRa RX and timers serviced |
| Time `loop()` spends blocked in LoRa TX | 0 | — | rule 3 |
| Flash writes per minute, standard load | ≤ 6 | — | each can stall both cores (rule 8) |
| WiFi health under standard load | ICMP loss < 1 %, RTT p95 < 150 ms, 0 TCP link drops per 30 min | 30–55 % loss (#43) | the #43 reporter's measure |
| Watchdog resets, 30-min soak | 0 | — | |
| LoRa airtime | within the configured airtime locks and regional duty cycle | — | |

## Instrumentation: every stall and drop reports itself

Build this first (step 1). Counters on hot paths; logs only on transitions.

- **Resource line** — one `[RES]` line every 60 s: internal free / min /
  largest block, PSRAM free, TLSF used / peak, loop iterations with p99 and max
  iteration time, milliseconds `loop()` spent blocked (by cause: LoRa TX, flash,
  TCP connect/write), LoRa TX queue depth / high-water / drops, announces
  accepted and dropped per interface, flash writes and total flash-busy ms,
  TCP per-client backlog and drops, WiFi RSSI and disconnects; Bluetooth
  slots, handshakes, and drops once it exists. It folds in the existing
  `[HEAP]` line and the Transport status line.
- **Event marks** with a millisecond timestamp: LoRa TX start/end, flash
  write start/end, heap-pressure stage changes, TCP link up/down. These line
  up against host-side ping timestamps to attribute each loss burst.
- Everything a drop path does is counted — "silent failures mask each other".

## The load harness

The gate every change passes. It runs on the bench: a V4 (and a V4 with
octal PSRAM once #44 lands), a stock RNode as the LoRa peer, the home AP, and
a host running `rnsd`. The staging RPi joins for Bluetooth (Linux `prnsd`).

| Scenario | What runs | Measures |
|---|---|---|
| A. Idle | 10 min, no peers | baseline of every metric |
| B. LAN transport flood (#43) | `rnsd` as a transport instance, `TCPClientInterface` into the local TCP server, fed by `test-harnesses/staging/py/announce_flood.py` at 1, 3 and 10 announces/s; LoRa SF10/BW125 | ping 5/s to the node for 10 min (loss, RTT p50/p95/max), TCP link drops, `[RES]` lines |
| C. WAN flood | same stream through a backbone slot | firewall path cost |
| D. LoRa flood | announces and path requests sent over LoRa from a stock RNode running `rnsd` (earlier runs were ad-hoc scripts, see LEARNED_SO_FAR.md 2026-05-10; script it) | LoRa ingress cost |
| E. Soak | B at 3/s for 30 min | watchdog resets, heap trend |
| F. Bluetooth (later) | B plus four Bluetooth peers: `prnsd` on the Mac, `prnsd` on the RPi, a Hopspot on a spare V4, a Columba phone | the same, plus coexistence effect on WiFi |

A run produces a results directory: the serial log, host ping output, flood
output, and a one-line summary appended to the Results log below. The script
lives in `tests/` alongside the proof-probe harnesses.

Host-side logic (codecs, parsers, the Bluetooth policy state machine) is
unit-tested without a board by `tests/native/run.sh`.

## Issue #43: what the code says

Read-only investigation, 2026-09-27. "Proven" means read in the code;
"inferred" needs the harness to confirm. Line numbers are at v1.0.50.

**Proven**

1. **LoRa TX drains the whole queue in one blocking call.** At SF10/BW125
   the modem bitrate (976 bps) is under the rate-limit thresholds, so
   `tx_queue_handler` always calls `flush_queue()`
   ([RNode_Firmware.ino:2742](RNode_Firmware.ino)), which transmits every
   queued packet back to back ([1606–1628](RNode_Firmware.ino)) with no CSMA
   between them. Each `sx126x::endPacket()` busy-polls the IRQ status over
   SPI until TxDone ([sx126x.cpp:466–473](sx126x.cpp)); DIO1 is not routed to
   TxDone. A full queue of announces (~36 × 1.6 s) blocks `loop()` for about
   a minute.
2. **That can reset the node.** `loop()` is the only task-watchdog feed, and
   the watchdog panics after 60 s ([RNode_Firmware.ino:80, 493](RNode_Firmware.ino)).
3. **While `loop()` is blocked, TCP is not serviced.** Accept, reads and
   keepalives all run from `loop()` ([TcpInterface.h](TcpInterface.h)).
   Once a socket's receive mailbox (6 segments) fills, lwIP stops ACKing, and
   a Linux `rnsd` peer aborts the connection after its 24 s
   `TCP_USER_TIMEOUT` (RNS `TCPInterface.py:84`) — a match for the reported
   "link DOWN".
4. **Firewall builds send every announce to LoRa, twice, with no cap.**
   `FIREWALL_MODE` compiles out announce bandwidth caps
   ([Transport.cpp:1084–1096](lib/microReticulum/src/Transport.cpp)), ingress
   `rate_blocked` is hard-wired false, and the LoRa interface never sets
   `_bitrate` or `_announce_cap`, so Transport sees LoRa as 1 kbps with no
   cap. Each accepted announce is rebroadcast at ~0–0.5 s and again ~5.5 s
   later. The Python reference caps LoRa announces at 2 % of bitrate.
5. **Every path-changing announce is written to flash as its own file**
   (`cache_packet(packet, true)`,
   [Transport.cpp:2459](lib/microReticulum/src/Transport.cpp)); every 60 s
   `clean_caches` deletes the unreferenced ones and `persist_data` rewrites
   `/time_offset`.
6. **Amplifiers:** Ed25519 announce verification runs before the replay
   check; the replay window holds only 16 entries; the announce table
   retires at most one entry per second; `VERBOSE` logging calls
   `Serial.flush`, which waits up to 100 ms per line with a USB host attached;
   the display pushes ~1 KB over I2C about 7 times a second; `WiFiClient::write`
   can block for 10 s or more on a weak link.
7. **Nothing sets WiFi power save**, so the core default (modem sleep) applies.

**Inferred**

- The whole-queue flush explains the TCP drops, but not pings lost *during*
  airtime: WiFi and lwIP run on core 0 and answer ICMP without `loop()`.
  Two candidates remain: the flash writes in 5 (a flash write stalls both
  cores, and this SDK build does not keep WiFi/lwIP in IRAM), and RF or supply
  coupling from the V4's PA, which defaults to 28 dBm. The harness separates
  them by lining host ping timestamps up against the `[Boundary] TXCFG` /
  `TXDONE` serial marks and the flash-write marks, then repeating at 2 dBm and
  with announce caching disabled.
- The reporter's setup is the CORE_PRINCIPLES hazard exactly: a transport
  `rnsd` on the trusted LAN side relaying backbone announces.

**Fix direction** (each measured on its own, rule 1)

- Event-driven LoRa TX: route TxDone to DIO1, send one packet per TxDone
  event, run CSMA per packet, and return to `loop()` between packets.
- Reference-parity announce policy on LoRa: set the interface bitrate from
  SF/BW and restore an announce cap in firewall builds.
- An announce-ingress budget for LAN-side transport peers (rule 6).
- Flash writes off the per-announce path if the flash A/B implicates them.

## PSRAM is not backing the Reticulum heap (quad V4, today)

The TLSF pool is created on the first C++ `new`: it takes 80 % of PSRAM if
`ESP.getPsramSize()` is non-zero, otherwise it latches plain `malloc` for
good ([OS.cpp:52–77](lib/microReticulum/src/Utilities/OS.cpp)). The V4 build
does not initialise PSRAM at boot (`CONFIG_SPIRAM_BOOT_INIT` is off); the
Arduino core does it later in `initArduino()`, which runs after C++ static
constructors. A `new` in any static constructor therefore sees no PSRAM.
Field evidence: `log.log` from a V4 reports `psram=2054639` bytes free, so
no ~1.6 MB pool was ever taken.

Consequence: every Reticulum object — paths, announces, links, packets —
lives in the same internal SRAM that WiFi, lwIP and the watchdog thresholds
depend on, and the V4's PSRAM sits idle. This changes every internal-heap
number above. Confirm with a boot log line (step 1), then fix the init order
so the pool is created once PSRAM is up (step 2).

## Issue #44: octal PSRAM

Read-only investigation, 2026-09-27, using Prns's V4-R8 board support as the
reference (Prns has not tested its R8 support on hardware either).

- **The board:** an ESP32-S3R8 (8 MB octal PSRAM in package), 16 MB DIO flash.
  Radio, front-end detection, display bus, button and VBAT pins match the V4.
- **Pins that must change:** octal PSRAM uses GPIO33–37 as data and strobe
  lines. RTNode's V4 map drives three of them — GPIO35 (LED), GPIO36 (Vext),
  GPIO37 (battery-divider control). On the R8, Vext is GPIO40 and the divider
  is read ungated. Driving 35–37 in an octal build would take lines away from
  PSRAM and corrupt it; in today's quad build on an R8 it is harmless, but
  Vext is never switched on, so the OLED is probably dark.
- **Today on an R8:** PSRAM init fails, the node runs with none (V3-like
  memory), and both flashers classify it as a V4 because the chip reports
  "PSRAM".
- **Two builds are required.** The prebuilt quad (`dio_qspi`) and octal
  (`dio_opi`) SDK variants link different PSRAM drivers. The bootloader and
  partition images are shared; only the app differs. A wrong build does not
  brick either board (both boot without PSRAM).
- **Detection:** esptool's chip features report "Embedded PSRAM 8MB" or
  "2MB" from eFuse (esptool.py 4.x, and esptool-js 0.4.6 used by the web
  flasher). Prns's own flashers do not tell the variants apart. The
  bundled esptool 4.5.1 reports neither, and would misread any V4 as a V3.
- **Open:** the R8's LED pin, its real eFuse value, and whether the octal
  build's 80 MHz PSRAM clock is stable (Prns chose 40 MHz). Need a board.
- **Touch list:** a new `dio_opi` env; a V4-R8 sub-variant in Boards.h
  (Vext 40, no LED on 35, no divider control on 37) and guards at every LED
  and Vext use; a boot line reporting PSRAM mode and size; flash.py and the
  web flasher's detection and firmware map; release packaging, mirror and
  versions lists; README.

## Bluetooth LE: its share of the budget

- **RAM decided at boot.** Firewall builds release the Bluetooth controller's
  memory at startup, and that cannot be undone without a reboot, so Bluetooth
  is a boot-time setting. The controller plus a NimBLE host costs internal
  SRAM that has to be measured on a V4 before any protocol work (the spike in
  step 6).
- **One radio.** Bluetooth advertising, scanning and connection events take
  time from WiFi. Scan duty trades directly against #43-style WiFi loss, so
  scenario F measures WiFi health with Bluetooth on. Prns's ESP32-S3 values
  (idle scan 200 ms per 1 s; discovery backs off while links are busy) are the
  starting point.
- **WiFi power save.** ESP-IDF's coexistence guidance expects WiFi modem sleep
  while Bluetooth is enabled, so `WiFi.setSleep(false)` — the #43 reporter's
  partial workaround — may not be available with Bluetooth on. Verify in the
  spike.
- **Four fixed peer slots**, each its own interface, registered at boot.
  Reassembly buffers and queues are allocated once.
- **Access control is IFAC.** Neither the Prns-native nor the Columba protocol
  has a password or pairing step; Reticulum's IFAC (network name and
  passphrase, as on LoRa) is the only compatible gate. Three limits:
  - It filters packets, not connections, so a stranger can still hold one of
    the four slots. A peer whose packets fail IFAC is closed and its identity
    backed off — the failed check is the deterministic event.
  - Peers must be configured to match. `prnsd` documents IFAC for every
    interface; the Prns Hopspot has no IFAC setting at all; Columba is
    unknown.
  - It costs one Ed25519 signature per packet in each direction (outbound
    sign at [Transport.cpp:894](lib/microReticulum/src/Transport.cpp); inbound
    verification re-signs). Measure it before offering it on Bluetooth.
- **Bluetooth peers are LAN-side** (decision below), so rule 6 applies to
  them from day one: a `prnsd` laptop with backbone connections is a LAN
  transport peer.

## Order of work

1. **Instrumentation, harness, baseline.** Build `[RES]`, the event marks and
   a boot line reporting whether the TLSF pool is in PSRAM; script scenarios
   A–E; run them on today's firmware. Scenario B should reproduce #43. Fill in
   the baseline column.
2. **Put the Reticulum heap in PSRAM** (quad V4): fix the TLSF init order.
   Small and high-value — it moves every Reticulum object out of the internal
   SRAM WiFi needs — so it goes first and gets its own re-baseline, which the
   #43 work is then measured against.
3. **#43.** Event-driven LoRa TX; reference-parity announce cap on LoRa; an
   ingress budget for LAN transport peers; flash writes off the per-announce
   path if the A/B implicates them. One change at a time; scenario B must pass
   before moving on.
4. **#44.** V4-R8 build variant, safe pin map, detection in both flashers.
   Needs an R8 on the bench.
5. **IFAC publishing.** The device advertisement includes the IFAC network
   name *and passphrase* whenever IFAC and advertising are both on
   ([Advertise.h](Advertise.h), `include_ifac`), although the portal tells
   users IFAC restricts access. The reference RNS publishes them only with
   `publish_ifac = yes`, which defaults to off. Make it opt-in, default off,
   before Bluetooth adds a second IFAC-protected medium.
6. **Bluetooth spike.** NimBLE on a V4, advertising, scanning and holding four
   connections, under scenario B. Record heap and WiFi-health cost.
7. **Bluetooth native GATT** → **Columba characteristics** → **L2CAP**, each
   through scenario F.

Each step's results go in the log below before the next step starts.

## Decisions

- 2026-09-27 — Bluetooth peers are LAN-side peers, alongside LAN TCP and LoRa;
  never the backbone zone.
- 2026-09-27 — The Bluetooth interface hosts the Columba characteristics as
  well as the Prns-native ones.
- 2026-09-27 — Bluetooth targets the V4 only: the 2 MB board now, the
  octal-PSRAM board once #44 ships. Not the V3.

## Results log

| Date | Firmware | Board | Scenario | Min heap | Loop p99 / max | Ping loss / p95 | Link drops | Notes |
|---|---|---|---|---|---|---|---|---|
| | | | | | | | | |
