// ─────────────────────────────────────────────────────────────────────────────
//  BleInterface.h — Bluetooth LE peers for RTNode (PERFORMANCE_STRATEGY.md,
//  order of work step 7). V4 firewall builds with RTNODE_BLE.
//
//  A GATT peripheral speaking two protocols on one service, and advertising
//  ble-reticulum v0.3.0's peripheral-only flag so that centrals connect to
//  us and RTNode never has to connect out:
//
//    service   37145b00-442d-4a94-917f-8f42c5da28e3
//    TX  …e4   read, notify   Columba: us → central, fragments
//    RX  …e5   write          Columba: central → us, identity then fragments
//    ID  …e6   read           Columba: our 16-byte transport identity hash
//    CTL …e7   write, notify  Prns: Hello → Welcome (or Close)
//    DAT …e8   write, notify  Prns: fragments, both ways
//
//  Columba (ble-reticulum v2.2, 07d9413, as pinned by Columba 2.2.6): a
//  central's first write of exactly 16 bytes to RX is its identity; a 1-byte
//  0x00 write is a keepalive. Prns (d48e9fc, prns-core bluetooth_auto): the
//  central writes a 23-byte Hello to CTL — [01][identity 16][endpoint 2]
//  [L2CAP PSM][link MTU u16 BE][RSSI] — and we answer Welcome ([02], ours)
//  by notification, or Close [03][reason] if it is our own identity. A
//  central that finds CTL uses Prns's protocol, otherwise Columba's. Both
//  frame data the same way: [type][sequence u16 BE][total u16 BE][data],
//  type 01 start, 02 continue, 03 end; a one-fragment packet is 01 with
//  total 1.
//
//  Each peer holds one of BLE_SLOTS fixed slots, and each slot is its own
//  Reticulum interface, registered at boot and online while its peer is
//  identified. The slots are trusted local interfaces, exactly like LoRa. When a peer leaves, the paths, links and reverse entries
//  through its slot are forgotten, so the slot's next peer never receives
//  them.
//
//  NimBLE runs its host in its own task on core 0. Its callbacks only copy
//  what happened into queues allocated at boot; loop() does everything else.
//  Outgoing fragments go out as notifications from loop(), one per slot per
//  pass. When NimBLE is out of buffers (ENOMEM) the fragment waits for the
//  next pass: that is the only backpressure signal NimBLE 1.4 gives for
//  notifications, and each occurrence is counted.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef BLE_INTERFACE_H
#define BLE_INTERFACE_H

#if defined(FIREWALL_MODE) && defined(RTNODE_BLE)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <vector>
#include <Interface.h>
#include <Transport.h>
#include <Identity.h>
#include <Log.h>
// Config.h defines MTU (508), which NimBLE's headers use as a parameter name.
#pragma push_macro("MTU")
#undef MTU
#include <NimBLEDevice.h>
#pragma pop_macro("MTU")

namespace ble {

static const char* SERVICE_UUID  = "37145b00-442d-4a94-917f-8f42c5da28e3";
static const char* TX_UUID       = "37145b00-442d-4a94-917f-8f42c5da28e4";
static const char* RX_UUID       = "37145b00-442d-4a94-917f-8f42c5da28e5";
static const char* IDENTITY_UUID = "37145b00-442d-4a94-917f-8f42c5da28e6";
static const char* CONTROL_UUID  = "37145b00-442d-4a94-917f-8f42c5da28e7";
static const char* DATA_UUID     = "37145b00-442d-4a94-917f-8f42c5da28e8";

// Prns control messages (prns-core bluetooth_auto/handshake.rs).
static const uint8_t  CONTROL_HELLO   = 0x01;
static const uint8_t  CONTROL_WELCOME = 0x02;
static const uint8_t  CONTROL_CLOSE   = 0x03;
static const uint8_t  CLOSE_SELF_CONNECTION = 0x01;
static const size_t   GREETING_LEN    = 23;
static const uint8_t  ENDPOINT_ESP32[2] = {0x05, 0x00};
static const uint16_t PRNS_LINK_MTU   = 500;
static const uint8_t  RSSI_UNKNOWN    = 0x80;

static const uint8_t  FRAG_START    = 0x01;
static const uint8_t  FRAG_CONTINUE = 0x02;
static const uint8_t  FRAG_END      = 0x03;
static const size_t   FRAG_HEADER   = 5;
static const size_t   IDENTITY_LEN  = 16;
static const size_t   VALUE_MAX     = 512;   // largest attribute value
static const uint16_t PACKET_MAX    = 512;   // Reticulum packets are ≤ 500
static const int      SLOTS         = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
static const int      TX_DEPTH      = 8;     // packets queued per slot
static const int      CONTROL_DEPTH = 32;
static const int      WRITE_DEPTH   = 16;
static const uint16_t ADV_INTERVAL_MIN = 160;   // 100 ms, in 0.625 ms units
static const uint16_t ADV_INTERVAL_MAX = 240;   // 150 ms
// v0.3.0 manufacturer data: company 0xFFFF (little-endian), version 3, flags.
static const uint8_t  ADV_FLAG_PERIPHERAL_ONLY = 0x01;

// ─── Events from the NimBLE host task ────────────────────────────────────────
enum ControlKind : uint8_t { CONTROL_CONNECT, CONTROL_DISCONNECT, CONTROL_MTU };
struct ControlEvent {
    uint8_t  kind;
    uint16_t conn;
    uint16_t value;    // MTU, or disconnect reason
};
enum Channel : uint8_t { CHANNEL_COLUMBA_RX, CHANNEL_CONTROL, CHANNEL_DATA };
struct WriteEvent {
    uint16_t conn;
    uint16_t len;
    uint8_t  channel;
    uint8_t  data[VALUE_MAX];
};

static QueueHandle_t control_queue = nullptr;
static QueueHandle_t write_queue   = nullptr;
static WriteEvent    host_write;     // NimBLE host task only
static WriteEvent    loop_write;     // loop() only
// Written by the host task; read by loop().
static volatile uint32_t control_drops = 0, write_drops = 0;

// ─── A slot: one peer, one Reticulum interface ──────────────────────────────
class BlePeerInterface : public RNS::InterfaceImpl {
public:
    explicit BlePeerInterface(const char* name) : RNS::InterfaceImpl(name) {
        _IN = true;
        _OUT = true;
        _HW_MTU = 500;
        _FIXED_MTU = true;
        _bitrate = 700000;   // Columba's BLEInterface.BITRATE_GUESS
    }

    enum Protocol : uint8_t { PROTOCOL_COLUMBA, PROTOCOL_PRNS };

    // Peer state, owned by loop().
    bool     used = false;
    bool     identified = false;
    Protocol protocol = PROTOCOL_COLUMBA;
    uint16_t conn = 0;
    uint16_t att_mtu = 23;
    uint8_t  identity[IDENTITY_LEN] = {};
    uint32_t connected_ms = 0;

    // Reassembly of the fragments the peer writes.
    uint8_t  rx[PACKET_MAX];
    uint16_t rx_len = 0, rx_total = 0, rx_next = 0;
    bool     rx_active = false;

    // Packets waiting to go out, and how far the first one has got.
    struct Packet { uint16_t len; uint8_t data[PACKET_MAX]; };
    Packet   tx[TX_DEPTH];
    uint8_t  tx_head = 0, tx_count = 0;
    uint16_t tx_seq = 0, tx_total = 0;

    // Counters since boot.
    uint32_t rx_packets = 0, rx_bytes = 0, tx_packets = 0, tx_bytes = 0;
    uint32_t tx_drops = 0, bad_fragments = 0, keepalives = 0, before_identity = 0;
    uint32_t enomem = 0, notify_errors = 0, peers = 0, prns_peers = 0, bad_control = 0;

    void attach(uint16_t handle) {
        used = true;
        identified = false;
        conn = handle;
        att_mtu = 23;
        connected_ms = millis();
        rx_active = false;
        tx_head = tx_count = 0;
        tx_seq = tx_total = 0;
    }

    void release() {
        used = false;
        identified = false;
        _online = false;
        rx_active = false;
        tx_head = tx_count = 0;
        tx_seq = tx_total = 0;
        // The next peer is a new source for ingress control; its held
        // announces go with it, as a detached spawned interface's do.
        _ingress = RNS::IngressState();
    }

    void set_online(Protocol which) {
        identified = true;
        protocol = which;
        _online = true;
        peers++;
        if (which == PROTOCOL_PRNS) prns_peers++;
    }

    std::string label() const { return toString(); }

    void deliver(const uint8_t* data, size_t len) {
        rx_packets++;
        rx_bytes += len;
        handle_incoming(RNS::Bytes(data, len));
    }

protected:
    virtual void send_outgoing(const RNS::Bytes& data) override {
        if (!_online) return;
        if (tx_count >= TX_DEPTH || data.size() > PACKET_MAX) {
            if (tx_drops++ == 0) {
                Serial.printf("[BLE] %s: outgoing queue full, dropping (counted)\r\n", toString().c_str());
            }
            return;
        }
        Packet& packet = tx[(tx_head + tx_count) % TX_DEPTH];
        packet.len = data.size();
        memcpy(packet.data, data.data(), data.size());
        tx_count++;
        InterfaceImpl::handle_outgoing(data);
    }
};

static BlePeerInterface*     slots[SLOTS] = {};
static std::vector<RNS::Interface> slot_interfaces;   // Transport's handles; they own the slots
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* control_char = nullptr;
static NimBLECharacteristic* data_char = nullptr;
static uint8_t               our_identity[IDENTITY_LEN];
static uint8_t               our_address[6];   // static random, most significant byte first
static uint32_t              orphan_writes = 0, self_connections = 0, replaced = 0;
static uint32_t              last_report_ms = 0;
static bool                  started = false;

static BlePeerInterface* slot_for(uint16_t conn, int* index = nullptr) {
    for (int i = 0; i < SLOTS; i++) {
        if (slots[i] && slots[i]->used && slots[i]->conn == conn) {
            if (index) *index = i;
            return slots[i];
        }
    }
    return nullptr;
}

static std::string short_identity(const uint8_t* identity) {
    char text[9];
    snprintf(text, sizeof(text), "%02x%02x%02x%02x", identity[0], identity[1], identity[2], identity[3]);
    return text;
}

// ─── NimBLE host task: copy and hand over, nothing else ─────────────────────
static int on_gap_event(ble_gap_event* event, void* arg) {
    ControlEvent control = {};
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) return 0;
            control.kind = CONTROL_CONNECT;
            control.conn = event->connect.conn_handle;
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            control.kind = CONTROL_DISCONNECT;
            control.conn = event->disconnect.conn.conn_handle;
            control.value = event->disconnect.reason;
            break;
        case BLE_GAP_EVENT_MTU:
            control.kind = CONTROL_MTU;
            control.conn = event->mtu.conn_handle;
            control.value = event->mtu.value;
            break;
        default:
            return 0;
    }
    if (xQueueSend(control_queue, &control, 0) != pdTRUE) control_drops++;
    return 0;
}

class WriteCallbacks : public NimBLECharacteristicCallbacks {
public:
    explicit WriteCallbacks(Channel which) : channel(which) {}
    void onWrite(NimBLECharacteristic* characteristic, ble_gap_conn_desc* desc) override {
        NimBLEAttValue value = characteristic->getValue();
        host_write.conn = desc->conn_handle;
        host_write.channel = channel;
        host_write.len = value.length() < VALUE_MAX ? value.length() : VALUE_MAX;
        memcpy(host_write.data, value.data(), host_write.len);
        if (xQueueSend(write_queue, &host_write, 0) != pdTRUE) write_drops++;
    }
private:
    Channel channel;
};
static WriteCallbacks rx_callbacks(CHANNEL_COLUMBA_RX);
static WriteCallbacks control_callbacks(CHANNEL_CONTROL);
static WriteCallbacks data_callbacks(CHANNEL_DATA);

// ─── Boot ────────────────────────────────────────────────────────────────────
// Before Reticulum starts: the slots' interfaces, offline until a peer comes.
inline void register_interfaces() {
    static const char* names[] = {"BLE1", "BLE2", "BLE3", "BLE4", "BLE5", "BLE6", "BLE7", "BLE8"};
    static_assert(SLOTS <= 8, "name the extra slots");
    slot_interfaces.reserve(SLOTS);
    for (int i = 0; i < SLOTS; i++) {
        slots[i] = new BlePeerInterface(names[i]);
        slot_interfaces.emplace_back(slots[i]);
        slot_interfaces[i].mode(RNS::Type::Interface::MODE_FULL);
        RNS::Transport::register_interface(slot_interfaces[i]);
        // Trusted and local, exactly like LoRa (James, 2026-09-28).
        RNS::Transport::register_local_client_interface(slot_interfaces[i]);
    }
}

// Once Transport has its identity: the GATT service and advertising.
inline void start() {
    if (started || !slots[0] || !RNS::Transport::identity()) return;
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    control_queue = xQueueCreate(CONTROL_DEPTH, sizeof(ControlEvent));
    // The write queue's storage (8 KB) goes in PSRAM; only loop() and the
    // NimBLE task touch it, never an interrupt.
    static StaticQueue_t write_queue_state;
    uint8_t* write_storage = (uint8_t*)heap_caps_malloc(WRITE_DEPTH * sizeof(WriteEvent), MALLOC_CAP_SPIRAM);
    write_queue = xQueueCreateStatic(WRITE_DEPTH, sizeof(WriteEvent), write_storage, &write_queue_state);
    const RNS::Bytes& identity_hash = RNS::Transport::identity().hash();
    memcpy(our_identity, identity_hash.data(), IDENTITY_LEN);

    NimBLEDevice::init("");
    // Our own address: static random, fixed per chip (a hash of its MAC), as
    // Prns does. The public address is the one this board had under stock
    // RNode firmware, and a phone once paired with that keeps using its
    // cached RNode service table ("Reticulum service not found"). A static
    // random address has its top two bits set, so it also sorts high, and
    // v2.2 peers that decide by address comparison dial us.
    {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BT);
        RNS::Bytes seed(mac, sizeof(mac));
        seed.append((const uint8_t*)"rtnode-ble", 10);
        RNS::Bytes digest = RNS::Identity::full_hash(seed);
        memcpy(our_address, digest.data(), sizeof(our_address));
        our_address[0] |= 0xC0;
        uint8_t little_endian[6];
        for (int i = 0; i < 6; i++) little_endian[i] = our_address[5 - i];
        ble_hs_id_set_rnd(little_endian);
        NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    }
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setCustomGapHandler(on_gap_event);

    NimBLEServer* server = NimBLEDevice::createServer();
    NimBLEService* service = server->createService(SERVICE_UUID);
    tx_char = service->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rx_char =
        service->createCharacteristic(RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx_char->setCallbacks(&rx_callbacks);
    NimBLECharacteristic* id_char = service->createCharacteristic(IDENTITY_UUID, NIMBLE_PROPERTY::READ);
    id_char->setValue(our_identity, IDENTITY_LEN);
    control_char = service->createCharacteristic(CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    control_char->setCallbacks(&control_callbacks);
    data_char = service->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    data_char->setCallbacks(&data_callbacks);
    service->start();

    NimBLEAdvertisementData data;
    data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    data.setCompleteServices(NimBLEUUID(SERVICE_UUID));
    const uint8_t manufacturer[4] = {0xFF, 0xFF, 0x03, ADV_FLAG_PERIPHERAL_ONLY};
    data.setManufacturerData(std::string((const char*)manufacturer, sizeof(manufacturer)));
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setAdvertisementData(data);
    advertising->setScanResponse(false);
    advertising->setMinInterval(ADV_INTERVAL_MIN);
    advertising->setMaxInterval(ADV_INTERVAL_MAX);
    advertising->start();

    started = true;
    last_report_ms = millis();
    char address_text[18];
    snprintf(address_text, sizeof(address_text), "%02x:%02x:%02x:%02x:%02x:%02x", our_address[0], our_address[1],
             our_address[2], our_address[3], our_address[4], our_address[5]);
    Serial.printf("[BLE] up: %d slots, identity %s, address %s (random); internal %u -> %u (%+d), "
                  "psram %u -> %u (%+d)\r\n",
                  SLOTS, short_identity(our_identity).c_str(), address_text,
                  (unsigned)internal_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (int)internal_before,
                  (unsigned)psram_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) - (int)psram_before);
}

// ─── loop(): peers come and go ───────────────────────────────────────────────
static void on_connect(uint16_t conn) {
    for (int i = 0; i < SLOTS; i++) {
        if (!slots[i]->used) {
            slots[i]->attach(conn);
            break;
        }
    }
    int used = 0;
    for (int i = 0; i < SLOTS; i++) used += slots[i]->used ? 1 : 0;
    // Advertising stops on connect; keep it up while a slot is free.
    if (used < SLOTS) NimBLEDevice::startAdvertising();
}

static void on_disconnect(uint16_t conn, uint16_t reason) {
    int index = -1;
    BlePeerInterface* slot = slot_for(conn, &index);
    if (!slot) return;
    size_t forgotten = 0;
    if (slot->identified) {
        forgotten = RNS::Transport::forget_interface_routes(slot_interfaces[index]);
        Serial.printf("[BLE] %s: peer %s left (reason 0x%x) after %lus; forgot %u routes through it\r\n",
                      slot->label().c_str(), short_identity(slot->identity).c_str(), reason,
                      (unsigned long)((millis() - slot->connected_ms) / 1000), (unsigned)forgotten);
    }
    slot->release();
}

static bool notify(uint16_t conn, NimBLECharacteristic* characteristic, const uint8_t* data, size_t len) {
    os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return false;
    return ble_gattc_notify_custom(conn, characteristic->getHandle(), om) == 0;   // consumes om
}

// The central's identity: its first 16-byte write to RX (Columba), or the
// identity in its Hello (Prns).
static void on_identity(int index, const uint8_t* identity, BlePeerInterface::Protocol protocol) {
    BlePeerInterface* slot = slots[index];
    if (memcmp(identity, our_identity, IDENTITY_LEN) == 0) {
        self_connections++;
        if (protocol == BlePeerInterface::PROTOCOL_PRNS) {
            const uint8_t close[2] = {CONTROL_CLOSE, CLOSE_SELF_CONNECTION};
            notify(slot->conn, control_char, close, sizeof(close));
        }
        NimBLEDevice::getServer()->disconnect(slot->conn);
        return;
    }
    // The same peer on another slot: it reconnected (Android rotates its
    // address) and the old link is gone from its side. Keep the new one.
    for (int i = 0; i < SLOTS; i++) {
        if (i != index && slots[i]->used && slots[i]->identified &&
            memcmp(slots[i]->identity, identity, IDENTITY_LEN) == 0) {
            replaced++;
            NimBLEDevice::getServer()->disconnect(slots[i]->conn);
        }
    }
    memcpy(slot->identity, identity, IDENTITY_LEN);
    slot->set_online(protocol);
    Serial.printf("[BLE] %s: %s peer %s identified, MTU %u\r\n", slot->label().c_str(),
                  protocol == BlePeerInterface::PROTOCOL_PRNS ? "Prns" : "Columba",
                  short_identity(identity).c_str(), (unsigned)slot->att_mtu);
}

// A Prns Hello, answered with our Welcome.
static void on_control(int index, const uint8_t* data, size_t len) {
    BlePeerInterface* slot = slots[index];
    if (len >= 2 && data[0] == CONTROL_CLOSE) {
        NimBLEDevice::getServer()->disconnect(slot->conn);
        return;
    }
    if (slot->identified || len < GREETING_LEN || data[0] != CONTROL_HELLO) {
        slot->bad_control++;
        return;
    }
    uint8_t welcome[GREETING_LEN];
    welcome[0] = CONTROL_WELCOME;
    memcpy(welcome + 1, our_identity, IDENTITY_LEN);
    memcpy(welcome + 1 + IDENTITY_LEN, ENDPOINT_ESP32, sizeof(ENDPOINT_ESP32));
    welcome[19] = 0x00;                    // no L2CAP channel
    welcome[20] = PRNS_LINK_MTU >> 8;
    welcome[21] = PRNS_LINK_MTU & 0xFF;
    welcome[22] = RSSI_UNKNOWN;
    const uint8_t* identity = data + 1;
    if (memcmp(identity, our_identity, IDENTITY_LEN) != 0 && !notify(slot->conn, control_char, welcome, sizeof(welcome))) {
        // The Welcome could not go out, so the link would never settle.
        slot->notify_errors++;
        NimBLEDevice::getServer()->disconnect(slot->conn);
        return;
    }
    on_identity(index, identity, BlePeerInterface::PROTOCOL_PRNS);
}

static void on_write(const WriteEvent& write) {
    int index = -1;
    BlePeerInterface* slot = slot_for(write.conn, &index);
    if (!slot) {
        orphan_writes++;
        return;
    }
    const uint8_t* data = write.data;
    size_t len = write.len;
    if (write.channel == CHANNEL_CONTROL) {
        on_control(index, data, len);
        return;
    }
    if (len == 1 && data[0] == 0x00) {
        slot->keepalives++;
        return;
    }
    if (!slot->identified) {
        if (write.channel == CHANNEL_COLUMBA_RX && len == IDENTITY_LEN) {
            on_identity(index, data, BlePeerInterface::PROTOCOL_COLUMBA);
        }
        else {
            slot->before_identity++;
        }
        return;
    }
    // Each protocol's fragments arrive on its own characteristic.
    bool expected = slot->protocol == BlePeerInterface::PROTOCOL_PRNS ? write.channel == CHANNEL_DATA
                                                                        : write.channel == CHANNEL_COLUMBA_RX;
    if (!expected) {
        slot->bad_fragments++;
        return;
    }
    if (len < FRAG_HEADER) {
        slot->bad_fragments++;
        return;
    }
    uint8_t  type  = data[0];
    uint16_t seq   = ((uint16_t)data[1] << 8) | data[2];
    uint16_t total = ((uint16_t)data[3] << 8) | data[4];
    const uint8_t* payload = data + FRAG_HEADER;
    size_t payload_len = len - FRAG_HEADER;
    if (type < FRAG_START || type > FRAG_END || total == 0 || seq >= total) {
        slot->bad_fragments++;
        slot->rx_active = false;
        return;
    }
    if (seq == 0) {
        slot->rx_len = 0;
        slot->rx_total = total;
        slot->rx_next = 0;
        slot->rx_active = true;
    }
    else if (!slot->rx_active || total != slot->rx_total || seq != slot->rx_next) {
        // GATT delivers one link's writes in order, so a gap means a lost
        // write (write_drops) or a confused peer; drop the packet.
        slot->bad_fragments++;
        slot->rx_active = false;
        return;
    }
    if (slot->rx_len + payload_len > PACKET_MAX) {
        slot->bad_fragments++;
        slot->rx_active = false;
        return;
    }
    memcpy(slot->rx + slot->rx_len, payload, payload_len);
    slot->rx_len += payload_len;
    slot->rx_next++;
    if (slot->rx_next == slot->rx_total) {
        slot->rx_active = false;
        slot->deliver(slot->rx, slot->rx_len);
    }
}

// One fragment of the slot's first queued packet, as a notification.
static void send_next_fragment(BlePeerInterface* slot) {
    static uint8_t fragment[VALUE_MAX];
    const BlePeerInterface::Packet& packet = slot->tx[slot->tx_head];
    size_t usable = slot->att_mtu > 3 ? slot->att_mtu - 3 : 20;
    if (usable > VALUE_MAX) usable = VALUE_MAX;
#ifdef RTNODE_BLE_CHUNK_MAX
    // Bench only: small fragments, to exercise a peer's reassembly.
    if (usable > RTNODE_BLE_CHUNK_MAX + FRAG_HEADER) usable = RTNODE_BLE_CHUNK_MAX + FRAG_HEADER;
#endif
    size_t chunk = usable - FRAG_HEADER;
    if (slot->tx_seq == 0) slot->tx_total = (packet.len + chunk - 1) / chunk;
    size_t offset = (size_t)slot->tx_seq * chunk;
    size_t n = packet.len - offset < chunk ? packet.len - offset : chunk;
    uint8_t type = slot->tx_seq == 0 ? FRAG_START
                 : (slot->tx_seq == slot->tx_total - 1 ? FRAG_END : FRAG_CONTINUE);
    fragment[0] = type;
    fragment[1] = slot->tx_seq >> 8;
    fragment[2] = slot->tx_seq & 0xFF;
    fragment[3] = slot->tx_total >> 8;
    fragment[4] = slot->tx_total & 0xFF;
    memcpy(fragment + FRAG_HEADER, packet.data + offset, n);

    os_mbuf* om = ble_hs_mbuf_from_flat(fragment, FRAG_HEADER + n);
    if (!om) {
        slot->enomem++;
        return;
    }
    NimBLECharacteristic* out = slot->protocol == BlePeerInterface::PROTOCOL_PRNS ? data_char : tx_char;
    int rc = ble_gattc_notify_custom(slot->conn, out->getHandle(), om);   // consumes om
    if (rc == BLE_HS_ENOMEM) {
        slot->enomem++;
        return;
    }
    if (rc != 0) {
        // The link is going or gone; this packet will not arrive whole.
        slot->notify_errors++;
        slot->tx_seq = 0;
        slot->tx_head = (slot->tx_head + 1) % TX_DEPTH;
        slot->tx_count--;
        return;
    }
    slot->tx_seq++;
    if (slot->tx_seq == slot->tx_total) {
        slot->tx_packets++;
        slot->tx_bytes += packet.len;
        slot->tx_seq = 0;
        slot->tx_head = (slot->tx_head + 1) % TX_DEPTH;
        slot->tx_count--;
    }
}

static void report() {
    Serial.printf("[BLE] t=%lu drops control/write %lu/%lu orphans %lu self %lu replaced %lu internal %u\r\n",
                  (unsigned long)millis(), (unsigned long)control_drops, (unsigned long)write_drops,
                  (unsigned long)orphan_writes, (unsigned long)self_connections, (unsigned long)replaced,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    for (int i = 0; i < SLOTS; i++) {
        BlePeerInterface* s = slots[i];
        if (!s->used && s->peers == 0) continue;
        Serial.printf("[BLE]   %s %s %s peer %s mtu %u peers %lu (prns %lu) rx %lu/%lu tx %lu/%lu queued %u "
                      "drops %lu bad %lu/%lu keepalive %lu early %lu enomem %lu errors %lu\r\n",
                      s->label().c_str(), s->used ? (s->identified ? "up" : "joining") : "free",
                      s->identified ? (s->protocol == BlePeerInterface::PROTOCOL_PRNS ? "prns" : "columba") : "-",
                      s->identified ? short_identity(s->identity).c_str() : "-", (unsigned)s->att_mtu,
                      (unsigned long)s->peers, (unsigned long)s->prns_peers,
                      (unsigned long)s->rx_packets, (unsigned long)s->rx_bytes,
                      (unsigned long)s->tx_packets, (unsigned long)s->tx_bytes, (unsigned)s->tx_count,
                      (unsigned long)s->tx_drops, (unsigned long)s->bad_fragments, (unsigned long)s->bad_control,
                      (unsigned long)s->keepalives, (unsigned long)s->before_identity,
                      (unsigned long)s->enomem, (unsigned long)s->notify_errors);
    }
}

inline void loop() {
    if (!started) return;

    ControlEvent control;
    while (xQueueReceive(control_queue, &control, 0) == pdTRUE) {
        if (control.kind == CONTROL_CONNECT) on_connect(control.conn);
        else if (control.kind == CONTROL_DISCONNECT) on_disconnect(control.conn, control.value);
        else if (control.kind == CONTROL_MTU) {
            BlePeerInterface* slot = slot_for(control.conn);
            if (slot) slot->att_mtu = control.value;
        }
    }
    // A few writes per pass, so a busy peer cannot hold up the loop.
    for (int i = 0; i < 4 && xQueueReceive(write_queue, &loop_write, 0) == pdTRUE; i++) {
        on_write(loop_write);
    }
    for (int i = 0; i < SLOTS; i++) {
        BlePeerInterface* slot = slots[i];
        if (slot->used && slot->identified && slot->tx_count > 0) send_next_fragment(slot);
    }

    uint32_t now = millis();
    if (now - last_report_ms >= 60000) {
        last_report_ms = now;
        report();
    }
}

}  // namespace ble

#endif // FIREWALL_MODE && RTNODE_BLE
#endif // BLE_INTERFACE_H
