// ─────────────────────────────────────────────────────────────────────────────
//  BleInterface.h — Bluetooth LE peers for RTNode (PERFORMANCE_STRATEGY.md,
//  order of work step 7). V4 firewall builds with RTNODE_BLE.
//
//  Speaks ble-reticulum's protocol v2.2 — Columba's — as a GATT peripheral,
//  and advertises v0.3.0's peripheral-only flag so that centrals (Columba
//  phones, Prns) connect to us and RTNode never has to connect out:
//
//    service   37145b00-442d-4a94-917f-8f42c5da28e3
//    TX  …e4   read, notify   us → central: fragments
//    RX  …e5   write          central → us: identity, then fragments
//    ID  …e6   read           our 16-byte Reticulum transport identity hash
//
//  A central's first write of exactly 16 bytes is its identity. After that,
//  every write is a fragment: [type][sequence u16 BE][total u16 BE][data],
//  type 01 start, 02 continue, 03 end; a packet that fits one fragment is 01
//  with total 1. A 1-byte 0x00 write is a keepalive. Reference: ble-reticulum
//  07d9413 (BLEFragmentation.py, BLE_PROTOCOL_v2.2.md, _v0.3.0.md), as pinned
//  by Columba 2.2.6.
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
struct WriteEvent {
    uint16_t conn;
    uint16_t len;
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

    // Peer state, owned by loop().
    bool     used = false;
    bool     identified = false;
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
    uint32_t enomem = 0, notify_errors = 0, peers = 0;

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

    void set_online() {
        identified = true;
        _online = true;
        peers++;
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
static uint8_t               our_identity[IDENTITY_LEN];
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

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, ble_gap_conn_desc* desc) override {
        NimBLEAttValue value = characteristic->getValue();
        host_write.conn = desc->conn_handle;
        host_write.len = value.length() < VALUE_MAX ? value.length() : VALUE_MAX;
        memcpy(host_write.data, value.data(), host_write.len);
        if (xQueueSend(write_queue, &host_write, 0) != pdTRUE) write_drops++;
    }
};
static RxCallbacks rx_callbacks;

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
    if (started || !RNS::Transport::identity()) return;
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
    Serial.printf("[BLE] up: %d slots, identity %s, address %s; internal %u -> %u (%+d), "
                  "psram %u -> %u (%+d)\r\n",
                  SLOTS, short_identity(our_identity).c_str(),
                  NimBLEDevice::getAddress().toString().c_str(),
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

// The central's identity: its first write of exactly 16 bytes.
static void on_identity(int index, const uint8_t* identity) {
    BlePeerInterface* slot = slots[index];
    if (memcmp(identity, our_identity, IDENTITY_LEN) == 0) {
        self_connections++;
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
    slot->set_online();
    Serial.printf("[BLE] %s: peer %s identified, MTU %u\r\n",
                  slot->label().c_str(), short_identity(identity).c_str(), (unsigned)slot->att_mtu);
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
    if (len == 1 && data[0] == 0x00) {
        slot->keepalives++;
        return;
    }
    if (!slot->identified) {
        if (len == IDENTITY_LEN) on_identity(index, data);
        else slot->before_identity++;
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
    int rc = ble_gattc_notify_custom(slot->conn, tx_char->getHandle(), om);   // consumes om
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
        Serial.printf("[BLE]   %s %s peer %s mtu %u peers %lu rx %lu/%lu tx %lu/%lu queued %u "
                      "drops %lu bad %lu keepalive %lu early %lu enomem %lu errors %lu\r\n",
                      s->label().c_str(), s->used ? (s->identified ? "up" : "joining") : "free",
                      s->identified ? short_identity(s->identity).c_str() : "-", (unsigned)s->att_mtu,
                      (unsigned long)s->peers, (unsigned long)s->rx_packets, (unsigned long)s->rx_bytes,
                      (unsigned long)s->tx_packets, (unsigned long)s->tx_bytes, (unsigned)s->tx_count,
                      (unsigned long)s->tx_drops, (unsigned long)s->bad_fragments,
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
