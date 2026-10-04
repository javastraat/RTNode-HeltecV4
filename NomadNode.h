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
// attempt doesn't push every later one back too): ~5s, 1min, 5min, 10min,
// 15min, then the normal NOMADNODE_ANNOUNCE_INTERVAL_S cycle from there.
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
    5UL * 60UL * 1000UL,
    10UL * 60UL * 1000UL,
    15UL * 60UL * 1000UL,
};
#define NOMADNODE_BOOT_SCHEDULE_LEN (sizeof(NOMADNODE_BOOT_SCHEDULE_MS) / sizeof(NOMADNODE_BOOT_SCHEDULE_MS[0]))

static RNS::Destination nomadnode_destination = {RNS::Type::NONE};
static bool     nomadnode_initialised     = false;
static uint32_t nomadnode_boot_ms              = 0;
static uint8_t  nomadnode_boot_schedule_index  = 0;
static uint32_t nomadnode_next_announce_ms = 0;
static bool     nomadnode_manual_pending   = false;

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
static void mu_title(std::string& mu, const std::string& subtitle) {
    mu += "`c`F0a0`!" + nomadnode_name() + "`!`f`a\n";
    mu += "`ca " + subtitle + "`a\n";
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

// ─── Pages (Micron markup) ───────────────────────────────────────────────────
// Signature fixed by RNS::RequestHandler::response_generator — unused params
// (path/data/request_id/link_id/remote_identity/requested_at) are required by
// that signature even though these pages don't use them.

static RNS::Bytes nomadnode_page_index(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "NomadNet status node");

    mu += "A `!RTNode`! LoRa mesh repeater.\n\n";

    char buf[48];
    snprintf(buf, sizeof(buf), "`cRadio: %s   ·   Up %s`a\n\n",
             radio_online ? "online" : "offline", mu_uptime().c_str());
    mu += buf;

    mu += ">Pages\n";
    mu += "`[Stats`/page/stats.mu]\n";
    mu += "`[Info`/page/info.mu]\n";
    mu += "`[Hardware`/page/hardware.mu]\n";
    mu += "`[Config`/page/config.mu]\n\n";

    mu += "-\n";
    mu += "`c`F888Reach it on the mesh \xE2\x80\x94 LoRa or WiFi.`f`a\n";

    return mu_finish(mu, "index.mu");
}

static RNS::Bytes nomadnode_page_stats(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "radio stats");

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

    mu += "\n`[<< Back`/page/index.mu]\n";
    return mu_finish(mu, "stats.mu");
}

// Split from a single bigger page -- see NOMADNODE_PAGE_BUDGET's own
// comment above for why.
static RNS::Bytes nomadnode_page_info(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "node info");

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
    mu_title(mu, "hardware");

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

static RNS::Bytes nomadnode_page_config(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    mu_title(mu, "config");

    mu_row(mu, "WiFi", firewall_state.wifi_enabled ? "on" : "off");
    if (firewall_state.backbones[0].enabled) {
        mu_row(mu, "TCP backbone", "on (%s:%u)", firewall_state.backbones[0].host, firewall_state.backbones[0].port);
    } else {
        mu_row(mu, "TCP backbone", "off");
    }
    mu_row(mu, "IFAC", firewall_state.ifac_enabled ? "on" : "off");
    mu_row(mu, "Advertise", firewall_state.advert_enabled ? "on (map)" : "off");
#ifdef RTNODE_BLE
    mu_row(mu, "Bluetooth", "%s (%u peers)", firewall_state.ble_running ? "on" : "off", (unsigned)firewall_state.ble_peers);
#endif

    mu += "\n`[<< Back`/page/index.mu]\n";
    return mu_finish(mu, "config.mu");
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
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/config.mu"),   nomadnode_page_config,   RNS::Type::Destination::ALLOW_ALL);

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
    nomadnode_boot_ms             = millis();
    nomadnode_next_announce_ms    = nomadnode_boot_ms + NOMADNODE_BOOT_SCHEDULE_MS[0];
    nomadnode_boot_schedule_index = 1;

    RNS::info("[NomadNode] Node page server ready: " + nomadnode_name());
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
    int32_t delta = (int32_t)(now - nomadnode_next_announce_ms);
    if (delta < 0 && !nomadnode_manual_pending) return;

    nomadnode_destination.announce(RNS::Bytes(nomadnode_name()));
    nomadnode_manual_pending = false;

    if (nomadnode_boot_schedule_index < NOMADNODE_BOOT_SCHEDULE_LEN) {
        // Absolute offset from boot, not from "now" -- a late-firing attempt
        // (channel contention) doesn't drag every later one in the taper
        // back with it.
        nomadnode_next_announce_ms = nomadnode_boot_ms + NOMADNODE_BOOT_SCHEDULE_MS[nomadnode_boot_schedule_index];
        nomadnode_boot_schedule_index++;
    } else {
        nomadnode_next_announce_ms = now + (NOMADNODE_ANNOUNCE_INTERVAL_S * 1000UL);
    }
}

#endif // FIREWALL_MODE
#endif // NOMADNODE_H
