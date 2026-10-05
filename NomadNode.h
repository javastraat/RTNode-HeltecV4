 // ─────────────────────────────────────────────────────────────────────────────
//  NomadNode.h — Nomad Network status node for RTNode
//
//  Hosts a standard NomadNetwork node destination (app_name "nomadnetwork",
//  aspect "node") so that anyone running the Nomad Network browser can
//  connect to this repeater once it has line of sight and pull up a few
//  Micron (.mu) pages showing its radio health, info and config — the same
//  way you'd check in on it over the WiFi portal, but over the mesh.
//
//  Reference: https://reticulum.network/manual/   (Destination / Link request
//  handlers) and NomadNet's own node implementation for the page paths and
//  announce format (raw UTF-8 display name as app_data).
// ─────────────────────────────────────────────────────────────────────────────
#ifndef NOMADNODE_H
#define NOMADNODE_H

#ifdef FIREWALL_MODE

#include <Arduino.h>
#include <Bytes.h>
#include <Identity.h>
#include <Destination.h>
#include <Transport.h>
#include <Reticulum.h>
#include <Log.h>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <map>
#include <MsgPack.h>

#include "FirewallMode.h"

// Externally-defined state these pages report (see Config.h / RNode_Firmware.ino)
extern uint32_t lora_freq;
extern uint32_t lora_bw;
extern int      lora_sf;
extern int      lora_cr;
extern int      last_rssi;
extern uint8_t  last_snr_raw;
extern uint32_t stat_rx;
extern uint32_t stat_tx;
extern bool     radio_online;
extern volatile uint8_t queue_height;

// Cached node-hash hex string in RTC memory (see RNode_Firmware.ino / Advertise.h).
#ifndef NODE_HASH_RTC_MAGIC
#define NODE_HASH_RTC_MAGIC  0x504B4841UL
#endif
extern uint32_t rtc_node_hash_magic;
extern char     rtc_node_hash_hex[33];

// Re-announce often enough that the node stays reachable, without eating
// into LoRa airtime like a chatty service. The *first* one fires soon after
// init (see nomadnode_init()), not after this full interval -- a client
// sitting on a stale/missing path for up to 30 minutes after every reboot
// is what this interval used to cost before that was shortened.
#define NOMADNODE_ANNOUNCE_INTERVAL_S (30UL * 60UL)
// Tapering schedule of announce times measured from boot (not from the
// previous announce -- each entry is an absolute offset, so a delayed
// attempt doesn't push every later one back too): ~5s, 1min, 2min, 5min,
// 8min, 10min, 12min, 15min, then the normal NOMADNODE_ANNOUNCE_INTERVAL_S
// cycle from there. More points than the original 5, still spread out
// rather than clustered (see below) -- a confirmed live ~40% single-shot
// delivery rate on a busy channel means each extra independent attempt in
// this window meaningfully raises the odds at least one gets through.
//
// The 5s floor is a real settle time, not zero -- confirmed live that
// firing the instant radio_online goes true (inside nomadnode_init(),
// called from setup()) doesn't reliably get through; something (most
// likely CSMA/channel-noise-floor sensing not having taken its first
// readings yet) needs a moment even though the radio itself is nominally
// up by then.
//
// The taper beyond that exists because a flat burst of 3 tries in under a
// minute (the previous scheme here) was confirmed live to still all get
// lost to channel noise on a busy mesh, after which nothing retried again
// for a full 30 minutes -- a cliff, not a gradual backoff. Spreading more
// tries across the first 15 minutes closes that gap instead of requiring
// a manual re-announce (the double-click handler) to rescue it.
static const uint32_t NOMADNODE_BOOT_SCHEDULE_MS[] = {
    5UL * 1000UL,
    60UL * 1000UL,
    2UL * 60UL * 1000UL,
    5UL * 60UL * 1000UL,
    8UL * 60UL * 1000UL,
    10UL * 60UL * 1000UL,
    12UL * 60UL * 1000UL,
    15UL * 60UL * 1000UL,
};
#define NOMADNODE_BOOT_SCHEDULE_LEN (sizeof(NOMADNODE_BOOT_SCHEDULE_MS) / sizeof(NOMADNODE_BOOT_SCHEDULE_MS[0]))

// Resolved once in nomadnode_init() from firewall_state.nomad_announce_interval_min
// (portal-configurable; 0 = keep the NOMADNODE_ANNOUNCE_INTERVAL_S default)
// -- lets an operator run a short interval on the bench for testing and a
// longer one once deployed, without a reflash.
static uint32_t nomadnode_announce_interval_s = NOMADNODE_ANNOUNCE_INTERVAL_S;

static RNS::Destination nomadnode_destination = {RNS::Type::NONE};
static bool     nomadnode_initialised     = false;
static uint32_t nomadnode_boot_ms              = 0;
static uint8_t  nomadnode_boot_schedule_index  = 0;
static uint32_t nomadnode_next_announce_ms = 0;
static bool     nomadnode_manual_pending   = false;

// Forward declaration -- defined below (near nomadnode_loop()), but
// nomadnode_page_advert() (defined earlier, alongside the other pages)
// needs to call it.
inline void nomadnode_request_now();

static std::string rtnode_board_name() {
#if BOARD_MODEL == BOARD_HELTEC32_V4
    #if defined(HELTEC_V4_R8)
        return "Heltec WiFi LoRa 32 V4 (R8)";
    #else
        return "Heltec WiFi LoRa 32 V4";
    #endif
#elif BOARD_MODEL == BOARD_HELTEC32_V3
    return "Heltec WiFi LoRa 32 V3";
#else
    return "Unknown";
#endif
}

static std::string nomadnode_name() {
    if (firewall_state.nomad_name[0] != '\0') return std::string(firewall_state.nomad_name);
    if (firewall_state.node_name[0] != '\0') return std::string(firewall_state.node_name);
    char name_buf[40];
    const char* hex = (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC && rtc_node_hash_hex[0] != '\0')
                      ? rtc_node_hash_hex : "";
    snprintf(name_buf, sizeof(name_buf), "RTNode-%.8s", hex[0] ? hex : "unknown");
    return std::string(name_buf);
}

// ─── Shared Micron helpers ───────────────────────────────────────────────────
// Visual language borrowed from meshpoint's own generated NomadNet pages
// (plugins/apps/reticulum/backend/nomad_node.py) so a node hosted by either
// looks like it belongs to the same family: a centred `F0a0 (teal) name +
// dim centred subtitle banner, `>Section headings, a left-padded "label:
// value" row for every stat, `F888 (dim grey) for secondary/explanatory text.

// Right-padded "label: value" line -- mirrors nomad_node.py's row() helper.
static void mu_row(std::string& mu, const char* label, const std::string& value) {
    char buf[110];
    snprintf(buf, sizeof(buf), "%-11s: %s\n", label, value.c_str());
    mu += buf;
}

static void mu_row(std::string& mu, const char* label, const char* fmt, ...) {
    char value[80];
    va_list args;
    va_start(args, fmt);
    vsnprintf(value, sizeof(value), fmt, args);
    va_end(args);
    mu_row(mu, label, std::string(value));
}

// Centred name + subtitle banner, same shape as nomad_node.py's title block.
// subtitle is shown verbatim -- each call site writes its own complete
// phrase (e.g. "a NomadNet status node", but just "config", "hardware",
// "nodes heard" for the rest), rather than this function gluing an
// indefinite article onto every single one whether it reads naturally or not.
static void mu_title(std::string& mu, const std::string& subtitle) {
    mu += "`c`F0a0`!" + nomadnode_name() + "`!`f`a\n";
    mu += "`c" + subtitle + "`a\n";
    mu += "-\n\n";
}

static std::string mu_uptime() {
    uint32_t up_s = millis() / 1000UL;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lud %luh %lum",
             (unsigned long)(up_s / 86400UL), (unsigned long)((up_s % 86400UL) / 3600UL), (unsigned long)((up_s % 3600UL) / 60UL));
    return std::string(buf);
}

// Conservative raw-page-text ceiling, with margin below the real cutoff: a
// response only sends as a single Link packet when the msgpack-packed
// [request_id, response] array fits Link::MDU (~383 bytes here, computed
// from this port's own MTU/header/crypto-overhead constants in Type.h).
// Anything bigger falls into Link.cpp's unfinished Resource-response path
// (its own "CBA TODO Determine why unused Resource is created here"
// comment) and is silently never delivered -- confirmed live: growing
// info.mu past this cost a real, reproducible "no NomadNet page" bug.
// Warn well before that line instead of finding out the same way again.
#define NOMADNODE_PAGE_BUDGET 350

static RNS::Bytes mu_finish(const std::string& mu, const char* page_name) {
    if (mu.size() > NOMADNODE_PAGE_BUDGET) {
        RNS::warning(std::string("[NomadNode] ") + page_name + " is " + std::to_string(mu.size())
            + " bytes -- over the ~" + std::to_string(NOMADNODE_PAGE_BUDGET) + " byte single-packet "
            + "budget; the response may silently never reach the client (see this file's own notes)");
    }
    return RNS::Bytes(mu);
}

// ─── Nodes heard ─────────────────────────────────────────────────────────────
// Other nomadnetwork.node peers this repeater has itself heard announce --
// mirrors meshpoint's own "Nodes this Meshpoint has heard" page
// (nomad_node.py's _serve_nodes), with one improvement: a real per-entry
// "last heard" time, which that one doesn't show. Fixed-size ring, oldest
// entry evicted once full -- trivial RAM cost (NODESHEARD_MAX * ~54 bytes)
// on a board with 2MB PSRAM.
#define NODESHEARD_MAX 16
#define NODESHEARD_HASH_LEN 16 // TRUNCATED_HASHLENGTH/8, Type.h

struct NodesHeardEntry {
    bool     used;
    uint8_t  hash[NODESHEARD_HASH_LEN];
    char     name[24];
    uint32_t last_seen_ms;
};
static NodesHeardEntry nodesheard_table[NODESHEARD_MAX];

static void nodesheard_record(const RNS::Bytes& destination_hash, const RNS::Bytes& app_data) {
    if (destination_hash.size() < NODESHEARD_HASH_LEN) return;
    const uint8_t* hb = destination_hash.data();

    int slot = -1;
    int oldest = 0;
    for (int i = 0; i < NODESHEARD_MAX; i++) {
        if (nodesheard_table[i].used && memcmp(nodesheard_table[i].hash, hb, NODESHEARD_HASH_LEN) == 0) {
            slot = i;
            break;
        }
        if (!nodesheard_table[i].used) {
            if (slot == -1) slot = i;
        } else if (nodesheard_table[i].last_seen_ms < nodesheard_table[oldest].last_seen_ms) {
            oldest = i;
        }
    }
    if (slot == -1) slot = oldest; // table full -- evict the oldest entry

    NodesHeardEntry& e = nodesheard_table[slot];
    e.used = true;
    memcpy(e.hash, hb, NODESHEARD_HASH_LEN);
    e.last_seen_ms = millis();
    // nomadnetwork.node announces carry the node's display name as raw
    // UTF-8 app_data (NomadNet's own convention, same as this repeater's
    // own announce in nomadnode_loop()) -- not a structured/LXMF payload.
    if (app_data.size() > 0) {
        size_t n = app_data.size();
        if (n > sizeof(e.name) - 1) n = sizeof(e.name) - 1;
        memcpy(e.name, app_data.data(), n);
        e.name[n] = '\0';
    } else {
        e.name[0] = '\0';
    }
}

class NodesHeardAnnounceHandler : public RNS::AnnounceHandler {
public:
    NodesHeardAnnounceHandler() : RNS::AnnounceHandler("nomadnetwork.node") {}
    virtual void received_announce(const RNS::Bytes& destination_hash, const RNS::Identity&, const RNS::Bytes& app_data) override {
        nodesheard_record(destination_hash, app_data);
    }
};
// Kept alive for the program's lifetime -- Transport::register_announce_handler()
// only stores the shared_ptr, so a local/temporary here would be destroyed
// (and silently deregistered) as soon as nomadnode_init() returns.
static RNS::HAnnounceHandler nodesheard_handler = std::make_shared<NodesHeardAnnounceHandler>();

// ─── Peer census ─────────────────────────────────────────────────────────────
// Deduplicated count of distinct peers heard announcing, any aspect -- not
// just nomadnetwork.node like the table above. Mirrors meshpoint's own
// "Known Peers / People / Infrastructure" split (reticulum_dashboard.js):
// People = lxmf.delivery, Infrastructure = everything else. A destination
// hash doesn't carry its aspect string in reverse, so each one is classified
// by testing it against hash_from_name_and_identity("lxmf.delivery",
// identity) -- the same test Transport itself runs internally for a
// filtered handler. Caught via an AnnounceHandler with no aspect_filter
// (empty = every aspect, see Transport.cpp's handler dispatch loop).
// Fixed-size table, no eviction -- once full, new distinct peers simply
// stop being counted rather than bumping out older ones (unlike
// nodesheard_table, there's no "most recent" ordering this needs to
// preserve). Trivial RAM cost (PEER_CENSUS_MAX * ~18 bytes) on a board
// with 2MB PSRAM.
#define PEER_CENSUS_MAX 128
#define PEER_CENSUS_HASH_LEN 16

struct PeerCensusEntry {
    bool     used;
    uint8_t  hash[PEER_CENSUS_HASH_LEN];
};
static PeerCensusEntry peer_census_table[PEER_CENSUS_MAX];
static uint16_t peer_census_people_count = 0;
static uint16_t peer_census_infra_count  = 0;

static void peer_census_record(const RNS::Bytes& destination_hash, const RNS::Identity& identity) {
    if (destination_hash.size() < PEER_CENSUS_HASH_LEN) return;
    const uint8_t* hb = destination_hash.data();

    int slot = -1;
    for (int i = 0; i < PEER_CENSUS_MAX; i++) {
        if (peer_census_table[i].used) {
            if (memcmp(peer_census_table[i].hash, hb, PEER_CENSUS_HASH_LEN) == 0) return; // already counted
        } else if (slot == -1) {
            slot = i;
        }
    }
    if (slot == -1) return; // table full

    bool is_people = (RNS::Destination::hash_from_name_and_identity("lxmf.delivery", identity) == destination_hash);

    PeerCensusEntry& e = peer_census_table[slot];
    e.used = true;
    memcpy(e.hash, hb, PEER_CENSUS_HASH_LEN);
    if (is_people) peer_census_people_count++; else peer_census_infra_count++;
}

class PeerCensusAnnounceHandler : public RNS::AnnounceHandler {
public:
    PeerCensusAnnounceHandler() : RNS::AnnounceHandler() {} // no filter -- every aspect
    virtual void received_announce(const RNS::Bytes& destination_hash, const RNS::Identity& identity, const RNS::Bytes&) override {
        peer_census_record(destination_hash, identity);
    }
};
static RNS::HAnnounceHandler peer_census_handler = std::make_shared<PeerCensusAnnounceHandler>();

// ─── Pages (Micron markup) ───────────────────────────────────────────────────
// Signature fixed by RNS::RequestHandler::response_generator — unused params
// (path/data/request_id/link_id/remote_identity/requested_at) are required by
// that signature even though these pages don't use them.

static RNS::Bytes nomadnode_page_index(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "Welcome to the Repeater NomadNode");

    // Kept deliberately terse -- nomadnode_name() can be up to 32 operator-
    // set characters (FirewallMode.h), and this page's own budget has to
    // hold up even at that worst case (see NOMADNODE_PAGE_BUDGET's notes).
    //
    // "Radio: online/offline" only shown when a TCP backbone is also
    // enabled -- on a LoRa-only repeater it's close to redundant (you
    // could only be viewing this page over LoRa if the radio were
    // already online, so it'll essentially always read "online" the one
    // time anyone actually sees it). With a backbone active, though, this
    // page can be reached over that path even while the local LoRa radio
    // itself has failed -- that's the one case this is real, new
    // information rather than restating the obvious.
    char buf[64];
    if (firewall_any_backbone_enabled()) {
        snprintf(buf, sizeof(buf), "`cRadio: %s   ·   Up %s`a\n\n",
                 radio_online ? "online" : "offline", mu_uptime().c_str());
    } else {
        snprintf(buf, sizeof(buf), "`cUp %s`a\n\n", mu_uptime().c_str());
    }
    mu += buf;

    mu += ">Pages\n";
    mu += "`[Stats`/page/stats.mu]\n";
    mu += "`[Info`/page/info.mu]\n";
    mu += "`[Hardware`/page/hardware.mu]\n";
    mu += "`[Nodes`/page/nodes.mu]\n";
    mu += "`[Config`/page/config.mu]\n";
    mu += "`[Advert`/page/advert.mu]\n";

    return mu_finish(mu, "index.mu");
}

static RNS::Bytes nomadnode_page_stats(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "Repeater radio stats");

    mu_row(mu, "Radio", radio_online ? "online" : "offline");
    if (radio_online) {
        float snr_db = ((signed char)last_snr_raw) * 0.25f;
        mu_row(mu, "RSSI", "%d dBm", last_rssi);
        mu_row(mu, "SNR", "%.1f dB", snr_db);
    }
    mu_row(mu, "Frequency", "%.3f MHz", lora_freq / 1000000.0);
    mu_row(mu, "Bandwidth", "%.1f kHz", lora_bw / 1000.0);
    mu_row(mu, "SF / CR", "%d / %d", lora_sf, lora_cr);
    mu += "\n";
    mu_row(mu, "Packets RX", "%lu", (unsigned long)stat_rx);
    mu_row(mu, "Packets TX", "%lu", (unsigned long)stat_tx);
    mu_row(mu, "Queue depth", "%u", (unsigned)queue_height);
    mu += "\n";
    mu_row(mu, "People", "%u", (unsigned)peer_census_people_count);
    mu_row(mu, "Infra ", "%u", (unsigned)peer_census_infra_count);

    mu += "\n`[<< Back`/page/index.mu]\n";
    return mu_finish(mu, "stats.mu");
}

// Split from a single bigger page -- see NOMADNODE_PAGE_BUDGET's own
// comment above for why.
static RNS::Bytes nomadnode_page_info(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "Repeater info");

    mu += ">Identity\n";
    mu_row(mu, "Hash", "%.16s",
           (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC && rtc_node_hash_hex[0] != '\0') ? rtc_node_hash_hex : "unknown");
    mu_row(mu, "Firmware", "v%d.%d", (int)MAJ_VERS, (int)MIN_VERS);
    mu_row(mu, "Uptime", mu_uptime());
    mu_row(mu, "Free heap", "%u KB", (unsigned)(ESP.getFreeHeap() / 1024));

    mu += "\n`[<< Back`/page/index.mu]\n";
    return mu_finish(mu, "info.mu");
}

static RNS::Bytes nomadnode_page_hardware(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "Repeater hardware");

    mu_row(mu, "Board", rtnode_board_name());
    mu_row(mu, "Chip", "%s rev%d, %dC @ %uMHz",
           ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getChipCores(), (unsigned)ESP.getCpuFreqMHz());
    mu_row(mu, "Flash", "%u MB", (unsigned)(ESP.getFlashChipSize() / (1024UL * 1024UL)));
    if (ESP.getPsramSize() > 0) {
        mu_row(mu, "PSRAM", "%u / %u KB free",
               (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getPsramSize() / 1024));
    }
#if HAS_LORA_PA
    if (lora_pa_model != LORA_PA_UNKNOWN) {
        mu_row(mu, "FEM", lora_pa_model == LORA_PA_KCT8103L ? "KCT8103L (V4.3)" : "GC1109 (V4.2)");
    }
#endif
    {
        uint8_t mac[6];
        WiFi.macAddress(mac);
        mu_row(mu, "MAC", "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    mu += "\n`[<< Back`/page/index.mu]\n";
    return mu_finish(mu, "hardware.mu");
}

static RNS::Bytes nomadnode_page_nodes(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "Nodes we heard");

    uint32_t now = millis();
    bool shown[NODESHEARD_MAX] = {false};
    int shown_count = 0;

    // How many entries actually fit varies with how long each one's name
    // happens to be (a fixed row count either wastes room on short names
    // or risks overflow on long ones) -- so this adds rows, most-recent
    // first, until the NEXT one would push the page past budget, reserving
    // room for the trailing back-link so that never gets squeezed out.
    const size_t reserve_tail = 32;

    for (int pick = 0; pick < NODESHEARD_MAX; pick++) {
        int best = -1;
        for (int i = 0; i < NODESHEARD_MAX; i++) {
            if (!nodesheard_table[i].used || shown[i]) continue;
            if (best == -1 || nodesheard_table[i].last_seen_ms > nodesheard_table[best].last_seen_ms) best = i;
        }
        if (best == -1) break;

        NodesHeardEntry& e = nodesheard_table[best];
        char hashhex[9];
        snprintf(hashhex, sizeof(hashhex), "%02x%02x%02x%02x", e.hash[0], e.hash[1], e.hash[2], e.hash[3]);
        std::string label = (e.name[0] != '\0') ? std::string(e.name) : std::string(hashhex);

        uint32_t age_s = (now - e.last_seen_ms) / 1000UL;
        char ago[8];
        if (age_s < 60) snprintf(ago, sizeof(ago), "%lus", (unsigned long)age_s);
        else if (age_s < 3600) snprintf(ago, sizeof(ago), "%lum", (unsigned long)(age_s / 60UL));
        else snprintf(ago, sizeof(ago), "%luh", (unsigned long)(age_s / 3600UL));

        char line[64];
        snprintf(line, sizeof(line), "%s ago - %s\n", ago, label.c_str());

        if (mu.size() + strlen(line) + reserve_tail > NOMADNODE_PAGE_BUDGET) break;

        shown[best] = true;
        shown_count++;
        mu += line;
    }
    if (shown_count == 0) {
        mu += "(none yet -- no nomadnetwork.node announce heard)\n";
    }

    mu += "\n`[<< Back`/page/index.mu]\n";
    return mu_finish(mu, "nodes.mu");
}

static RNS::Bytes nomadnode_page_config(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "Repeater config");

    mu_row(mu, "WiFi", firewall_state.wifi_enabled ? "on" : "off");
    if (firewall_state.backbones[0].enabled) {
        mu_row(mu, "TCP backbone", "on (%s:%u)", firewall_state.backbones[0].host, firewall_state.backbones[0].port);
    } else {
        mu_row(mu, "TCP backbone", "off");
    }
    mu_row(mu, "IFAC", firewall_state.ifac_enabled ? "on" : "off");
    mu_row(mu, "Advertise", firewall_state.advert_enabled ? "on (map)" : "off");
    mu_row(mu, "Nomad announce", "every %lumin", (unsigned long)(nomadnode_announce_interval_s / 60));
    mu_row(mu, "Advert announce", "every %lumin", (unsigned long)(advertise_announce_interval_ms / 60000UL));
#ifdef RTNODE_BLE
    mu_row(mu, "Bluetooth", "%s (%u peers)", firewall_state.ble_running ? "on" : "off", (unsigned)firewall_state.ble_peers);
#endif

    mu += "\n`[<< Back`/page/index.mu]\n";
    return mu_finish(mu, "config.mu");
}

// Lets anyone who can already reach this node (even over a shaky/stale
// path) ask it to re-announce right now, without needing physical access
// to the double-click button -- useful exactly in the reachability
// situations this session spent so long on. ALLOW_ALL like every other
// page here, so it's reachable by anyone on the mesh, not just the
// operator: a per-visit cooldown stops it being an open spam lever that
// could otherwise force constant re-announces at everyone else's airtime
// expense.
#define NOMADNODE_ADVERT_PAGE_COOLDOWN_MS (60UL * 1000UL)
static uint32_t nomadnode_advert_page_last_ms = 0;

static RNS::Bytes nomadnode_page_advert(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "Repeater advertise");

    uint32_t now = millis();
    if (now - nomadnode_advert_page_last_ms < NOMADNODE_ADVERT_PAGE_COOLDOWN_MS) {
        mu += "Already queued recently -- try again in a bit.\n";
    } else {
        nomadnode_advert_page_last_ms = now;
        nomadnode_request_now();
        advertise_request_now();
        mu += "Announce queued -- it'll go out shortly (CSMA permitting).\n";
    }

    mu += "\n`[<< Back`/page/index.mu]\n";
    return mu_finish(mu, "advert.mu");
}

// ─── Admin action pages ──────────────────────────────────────────────────────
// Password-gated, unlike every page above -- reserved for actions that
// actually warrant it (reboot first; more may follow). Off entirely until
// an operator sets firewall_state.admin_password in the portal (empty =
// every one of these refuses the request outright, not just hides a link
// -- there's deliberately no unauthenticated fallback). Not linked from
// index.mu on purpose: the password is the real gate, but there's no
// reason to also advertise these on the public landing page.
//
// Field submission format is NomadNet's own convention (a NomadNet/
// Sideband-style client bundles every `<name`default> field on the
// current page into the request as a msgpack map, keyed "field_<name>")
// -- NOT yet live-verified against a real client from this firmware,
// since nothing here could exercise it before now. If the password check
// always fails even with the right password, this is the first place to
// look -- confirm the actual submitted key/value shapes against a live
// capture and adjust admin_password_ok() accordingly.
static bool admin_password_ok(const RNS::Bytes& data) {
    if (firewall_state.admin_password[0] == '\0') return false;
    if (data.size() == 0) return false;

    // MsgPack::str_t is Arduino's String on this build (Types.h), not
    // std::string -- std::map<std::string, std::string> compiles but the
    // library's unpack() can't actually fill a plain std::string value,
    // only its own str_t.
    MsgPack::Unpacker unpacker;
    unpacker.feed(data.data(), data.size());
    std::map<MsgPack::str_t, MsgPack::str_t> fields;
    if (!unpacker.unpack(fields)) return false;

    auto it = fields.find("field_password");
    if (it == fields.end()) return false;
    return it->second == firewall_state.admin_password;
}

// Shared login-form body for every admin_*.mu page -- submits back to
// whichever exact path it was served from (RNS matches by exact path, so
// each action is its own registered handler rather than one page with an
// action selector).
static void admin_login_form(std::string& mu, const char* self_path) {
    mu += "`<field_password`>\n\n";
    mu += std::string("`[Submit`") + self_path + "]\n";
}

static RNS::Bytes nomadnode_page_admin(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "admin");

    if (firewall_state.admin_password[0] == '\0') {
        mu += "Admin actions are disabled (no password set in the portal).\n";
    } else {
        mu += "`[Send advert`/page/admin_advert.mu]\n";
        mu += "`[Reboot`/page/admin_reboot.mu]\n";
    }

    mu += "\n`[<< Back`/page/index.mu]\n";
    return mu_finish(mu, "admin.mu");
}

static RNS::Bytes nomadnode_page_admin_advert(const RNS::Bytes&, const RNS::Bytes& data, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "admin / advert");

    if (!admin_password_ok(data)) {
        if (data.size() > 0) mu += "Wrong password.\n\n";
        admin_login_form(mu, "/page/admin_advert.mu");
    } else {
        nomadnode_request_now();
        advertise_request_now();
        mu += "Announce queued -- it'll go out shortly (CSMA permitting).\n";
    }

    mu += "\n`[<< Back`/page/admin.mu]\n";
    return mu_finish(mu, "admin_advert.mu");
}

// Reboot is deferred to a check inside nomadnode_loop() (called every
// main-loop pass) rather than calling ESP.restart() right here. This
// handler runs inline in the same call stack that just queued the
// confirmation response -- RNS::Packet::send() only enqueues bytes into
// the TX queue (LoRaInterface::send_outgoing), it doesn't transmit
// synchronously, and actually getting a frame on air can take a while on
// a busy channel (CSMA backoff, the whole reason this session spent so
// long on announce timing). Restarting immediately would almost
// certainly tear the radio down before the queued confirmation page ever
// reaches the client. Giving it a real window first means the one
// person who should see "rebooting now" actually gets to.
#define NOMADNODE_ADMIN_REBOOT_DELAY_MS (10UL * 1000UL)
static uint32_t nomadnode_admin_reboot_at_ms = 0; // 0 = none pending

static RNS::Bytes nomadnode_page_admin_reboot(const RNS::Bytes&, const RNS::Bytes& data, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "admin / reboot");

    if (!admin_password_ok(data)) {
        if (data.size() > 0) mu += "Wrong password.\n\n";
        admin_login_form(mu, "/page/admin_reboot.mu");
    } else {
        nomadnode_admin_reboot_at_ms = millis() + NOMADNODE_ADMIN_REBOOT_DELAY_MS;
        mu += "Rebooting in ~" + std::to_string(NOMADNODE_ADMIN_REBOOT_DELAY_MS / 1000) + "s.\n";
    }

    mu += "\n`[<< Back`/page/admin.mu]\n";
    return mu_finish(mu, "admin_reboot.mu");
}

// ─── Public API ─────────────────────────────────────────────────────────────

// Initialise the node destination and register its pages. Call once after
// RNS has started and Transport::identity() is available. Safe to call
// multiple times — only the first call has any effect.
inline void nomadnode_init() {
    if (nomadnode_initialised) return;
    if (!RNS::Transport::identity()) return;

    nomadnode_destination = RNS::Destination(
        RNS::Transport::identity(),
        RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE,
        "nomadnetwork",
        "node"
    );

    nomadnode_destination.register_request_handler(RNS::Bytes("/page/index.mu"),    nomadnode_page_index,    RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/stats.mu"),    nomadnode_page_stats,    RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/info.mu"),     nomadnode_page_info,     RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/hardware.mu"), nomadnode_page_hardware, RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/nodes.mu"),    nomadnode_page_nodes,    RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/config.mu"),   nomadnode_page_config,   RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/advert.mu"),   nomadnode_page_advert,   RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/admin.mu"),        nomadnode_page_admin,        RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/admin_advert.mu"), nomadnode_page_admin_advert, RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/admin_reboot.mu"), nomadnode_page_admin_reboot, RNS::Type::Destination::ALLOW_ALL);

    RNS::Transport::register_announce_handler(nodesheard_handler);
    RNS::Transport::register_announce_handler(peer_census_handler);

    nomadnode_initialised = true;

    // Announce soon after boot rather than waiting the full
    // NOMADNODE_ANNOUNCE_INTERVAL_S -- a client can otherwise sit on a
    // stale/missing path for up to 30 minutes after every reboot, confirmed
    // live as the actual cause of a real "no NomadNet page" report that
    // looked like a request bug but was really just a missing path.
    //
    // NOT fired synchronously right here, though -- tried that (an
    // immediate .announce() call, reasoning that startRadio() had already
    // run via validate_status() earlier in setup()) and confirmed live it
    // doesn't reliably get through, even though radio_online is already
    // true by this point. Something else evidently still needs a moment
    // (CSMA/channel-noise-floor sensing hasn't taken its first readings
    // yet, most likely) -- a manual re-announce moments later (the
    // double-click handler) works fine. Deferring to nomadnode_loop()'s
    // normal poll sidesteps whatever that race is, and walking the taper
    // schedule above (NOMADNODE_BOOT_SCHEDULE_MS) instead of a single
    // fixed delay avoids reintroducing the original half-hour-stale
    // problem if an early attempt or two gets lost to channel noise.
    nomadnode_announce_interval_s = firewall_nomad_announce_interval_s(NOMADNODE_ANNOUNCE_INTERVAL_S);
    nomadnode_boot_ms             = millis();
    nomadnode_next_announce_ms    = nomadnode_boot_ms + NOMADNODE_BOOT_SCHEDULE_MS[0];
    nomadnode_boot_schedule_index = 1;

    RNS::info("[NomadNode] Node page server ready: " + nomadnode_name() +
              " (announce every " + std::to_string(nomadnode_announce_interval_s / 60) + "min)");
}

// Request an announce as soon as possible, bypassing the periodic interval
// (button double-click, mirrors advertise_request_now() in Advertise.h).
// No-op before nomadnode_init() has run. Picked up by the next
// nomadnode_loop() call.
inline void nomadnode_request_now() {
    if (!nomadnode_initialised) return;
    nomadnode_manual_pending = true;
}

// Periodic loop hook — call from the main loop(). Re-announces the node on
// its own schedule so it stays reachable without depending on the separate
// interface-discovery announcer (Advertise.h) or its "Advertise Device" toggle.
inline void nomadnode_loop() {
    if (!nomadnode_initialised) return;

    uint32_t now = millis();

    // A password-confirmed admin reboot (nomadnode_page_admin_reboot) --
    // checked ahead of the early-return below so it isn't skipped on a
    // pass with nothing announce-related due.
    if (nomadnode_admin_reboot_at_ms != 0 && (int32_t)(now - nomadnode_admin_reboot_at_ms) >= 0) {
        ESP.restart();
    }

    int32_t delta = (int32_t)(now - nomadnode_next_announce_ms);
    if (delta < 0 && !nomadnode_manual_pending) return;

    RNS::verbose(std::string("[NomadNode] Sending announce (") + (nomadnode_manual_pending ? "manual" : "scheduled")
        + "), name: \"" + nomadnode_name() + "\"");
    nomadnode_destination.announce(RNS::Bytes(nomadnode_name()));
    nomadnode_manual_pending = false;

    if (nomadnode_boot_schedule_index < NOMADNODE_BOOT_SCHEDULE_LEN) {
        // Absolute offset from boot, not from "now" -- a late-firing attempt
        // (channel contention) doesn't drag every later one in the taper
        // back with it.
        nomadnode_next_announce_ms = nomadnode_boot_ms + NOMADNODE_BOOT_SCHEDULE_MS[nomadnode_boot_schedule_index];
        nomadnode_boot_schedule_index++;
    } else {
        nomadnode_next_announce_ms = now + (nomadnode_announce_interval_s * 1000UL);
    }
}

#endif // FIREWALL_MODE
#endif // NOMADNODE_H
