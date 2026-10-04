 // ─────────────────────────────────────────────────────────────────────────────
//  NomadNode.h — Nomad Network status node for RTNode
//
//  Hosts a standard NomadNetwork node destination (app_name "nomadnetwork",
//  aspect "node") so that anyone running the Nomad Network browser can
//  connect to this repeater once it has line of sight and pull up a few
//  Micron (.mu) pages showing its radio health, identity and config — the
//  same way you'd check in on it over the WiFi portal, but over the mesh.
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

// Re-announce often enough that the node stays reachable soon after the
// repeater goes up, without eating into LoRa airtime like a chatty service.
#define NOMADNODE_ANNOUNCE_INTERVAL_S (30UL * 60UL)
#define NOMADNODE_INITIAL_DELAY_MS    (65UL * 1000UL)

static RNS::Destination nomadnode_destination = {RNS::Type::NONE};
static bool     nomadnode_initialised     = false;
static uint32_t nomadnode_next_announce_ms = 0;
static bool     nomadnode_manual_pending   = false;

static std::string nomadnode_name() {
    if (firewall_state.nomad_name[0] != '\0') return std::string(firewall_state.nomad_name);
    if (firewall_state.node_name[0] != '\0') return std::string(firewall_state.node_name);
    char name_buf[40];
    const char* hex = (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC && rtc_node_hash_hex[0] != '\0')
                      ? rtc_node_hash_hex : "";
    snprintf(name_buf, sizeof(name_buf), "RTNode-%.8s", hex[0] ? hex : "unknown");
    return std::string(name_buf);
}

// ─── Pages (Micron markup) ───────────────────────────────────────────────────
// Signature fixed by RNS::RequestHandler::response_generator — unused params
// (path/data/request_id/link_id/remote_identity/requested_at) are required by
// that signature even though these pages don't use them.

static RNS::Bytes nomadnode_page_index(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    char line[80];

    mu += "`c`!>>" + nomadnode_name() + "`!`a\n";
    mu += "`cRTNode LoRa mesh repeater`a\n";
    mu += "-=\n\n";

    uint32_t up_s = millis() / 1000UL;
    snprintf(line, sizeof(line), "`cRadio: %s   ·   Up %lud %luh %lum`a\n\n",
             radio_online ? "online" : "offline",
             (unsigned long)(up_s / 86400UL), (unsigned long)((up_s % 86400UL) / 3600UL), (unsigned long)((up_s % 3600UL) / 60UL));
    mu += line;

    mu += "-\n\n";

    mu += "`[Stats`/page/stats.mu]\n";
    mu += "`F888Signal, radio parameters, packet counters.`f\n\n";

    mu += "`[Identity`/page/identity.mu]\n";
    mu += "`F888Firmware version, uptime, free memory.`f\n\n";

    mu += "`[Config`/page/config.mu]\n";
    mu += "`F888Backbone, IFAC, advertise, Bluetooth.`f\n\n";

    mu += "-\n";
    mu += "`c`F888Reach it on the mesh \xE2\x80\x94 LoRa or WiFi.`f`a\n";

    return RNS::Bytes(mu);
}

static RNS::Bytes nomadnode_page_stats(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    char line[80];
    mu += ">Stats\n\n";

    mu += radio_online ? "Radio: online\n" : "Radio: offline\n";
    if (radio_online) {
        float snr_db = ((signed char)last_snr_raw) * 0.25f;
        snprintf(line, sizeof(line), "RSSI: %d dBm   SNR: %.1f dB\n", last_rssi, snr_db);
        mu += line;
    }
    snprintf(line, sizeof(line), "Freq: %.3f MHz   BW: %.1f kHz\n", lora_freq / 1000000.0, lora_bw / 1000.0);
    mu += line;
    snprintf(line, sizeof(line), "SF: %d   CR: %d\n", lora_sf, lora_cr);
    mu += line;
    snprintf(line, sizeof(line), "Packets RX: %lu   TX: %lu\n", (unsigned long)stat_rx, (unsigned long)stat_tx);
    mu += line;
    snprintf(line, sizeof(line), "Queue depth: %u\n", (unsigned)queue_height);
    mu += line;

    mu += "\n`[<< Back`/page/index.mu]\n";
    return RNS::Bytes(mu);
}

static RNS::Bytes nomadnode_page_identity(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    char line[80];
    mu += ">Identity\n\n";

    mu += "Name: " + nomadnode_name() + "\n";
    snprintf(line, sizeof(line), "Hash: %.16s\n",
             (rtc_node_hash_magic == NODE_HASH_RTC_MAGIC && rtc_node_hash_hex[0] != '\0') ? rtc_node_hash_hex : "unknown");
    mu += line;
    snprintf(line, sizeof(line), "Firmware: v%d.%d\n", (int)MAJ_VERS, (int)MIN_VERS);
    mu += line;

    uint32_t up_s = millis() / 1000UL;
    snprintf(line, sizeof(line), "Uptime: %lud %luh %lum\n",
             (unsigned long)(up_s / 86400UL), (unsigned long)((up_s % 86400UL) / 3600UL), (unsigned long)((up_s % 3600UL) / 60UL));
    mu += line;

    snprintf(line, sizeof(line), "Free heap: %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));
    mu += line;
    if (ESP.getPsramSize() > 0) {
        snprintf(line, sizeof(line), "Free PSRAM: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
        mu += line;
    }

    mu += "\n`[<< Back`/page/index.mu]\n";
    return RNS::Bytes(mu);
}

static RNS::Bytes nomadnode_page_config(const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Bytes&, const RNS::Identity&, double) {
    std::string mu;
    char line[96];
    mu += ">Config\n\n";

    mu += std::string("WiFi: ") + (firewall_state.wifi_enabled ? "on" : "off") + "\n";

    if (firewall_state.backbones[0].enabled) {
        snprintf(line, sizeof(line), "TCP backbone: on (%s:%u)\n", firewall_state.backbones[0].host, firewall_state.backbones[0].port);
    } else {
        snprintf(line, sizeof(line), "TCP backbone: off\n");
    }
    mu += line;

    mu += std::string("IFAC: ") + (firewall_state.ifac_enabled ? "on" : "off") + "\n";
    mu += std::string("Advertise (map): ") + (firewall_state.advert_enabled ? "on" : "off") + "\n";
#ifdef RTNODE_BLE
    snprintf(line, sizeof(line), "BLE: %s (%u peers)\n", firewall_state.ble_running ? "on" : "off", (unsigned)firewall_state.ble_peers);
    mu += line;
#endif

    mu += "\n`[<< Back`/page/index.mu]\n";
    return RNS::Bytes(mu);
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
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/identity.mu"), nomadnode_page_identity, RNS::Type::Destination::ALLOW_ALL);
    nomadnode_destination.register_request_handler(RNS::Bytes("/page/config.mu"),   nomadnode_page_config,   RNS::Type::Destination::ALLOW_ALL);

    nomadnode_initialised      = true;
    nomadnode_next_announce_ms = millis() + NOMADNODE_INITIAL_DELAY_MS;

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
    nomadnode_manual_pending   = false;
    nomadnode_next_announce_ms = now + (NOMADNODE_ANNOUNCE_INTERVAL_S * 1000UL);
}

#endif // FIREWALL_MODE
#endif // NOMADNODE_H
