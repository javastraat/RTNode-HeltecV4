// ─────────────────────────────────────────────────────────────────────────────
//  BleSpike.h — Bluetooth LE cost spike (PERFORMANCE_STRATEGY.md, order of
//  work step 6). Bench only: env rtnode_heltec_v4_bench_ble.
//
//  Hosts the Prns/Columba GATT service and advertises it as Prns does (27
//  bytes, no scan response), scans for other Prns nodes, accepts up to
//  CONFIG_BT_NIMBLE_MAX_CONNECTIONS peers, and echoes every write to the data
//  characteristic back as a notification, so tests/bench_ble.py can time
//  round trips while bench_load.py measures WiFi health. No Reticulum traffic.
//
//  NimBLE runs its host in its own task on core 0. Its callbacks only count
//  and copy: a write crosses to loop() through a queue allocated at boot (full
//  = counted drop), and loop() sends the echo.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef BLE_SPIKE_H
#define BLE_SPIKE_H

#if defined(FIREWALL_MODE) && defined(RTNODE_BLE_SPIKE)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
// Config.h defines MTU (508), which NimBLE's headers use as a parameter name.
#pragma push_macro("MTU")
#undef MTU
#include <NimBLEDevice.h>
#pragma pop_macro("MTU")

// Scan duty cycle in percent of RTNODE_BLE_SCAN_INTERVAL_MS; 0 = no scanning.
// Prns's ESP32-S3 idle scan is 200 ms in every 1000.
#ifndef RTNODE_BLE_SCAN_PERCENT
#define RTNODE_BLE_SCAN_PERCENT 10
#endif
#ifndef RTNODE_BLE_SCAN_INTERVAL_MS
#define RTNODE_BLE_SCAN_INTERVAL_MS 500
#endif

namespace ble_spike {

// Prns commit d48e9fc. Columba uses the same service with e4–e6.
static const char* SERVICE_UUID    = "37145b00-442d-4a94-917f-8f42c5da28e3";
static const char* COLUMBA_TX_UUID = "37145b00-442d-4a94-917f-8f42c5da28e4";
static const char* COLUMBA_RX_UUID = "37145b00-442d-4a94-917f-8f42c5da28e5";
static const char* COLUMBA_ID_UUID = "37145b00-442d-4a94-917f-8f42c5da28e6";
static const char* CONTROL_UUID    = "37145b00-442d-4a94-917f-8f42c5da28e7";
static const char* DATA_UUID       = "37145b00-442d-4a94-917f-8f42c5da28e8";

static const uint16_t SCAN_INTERVAL_MS  = RTNODE_BLE_SCAN_INTERVAL_MS;
static const uint16_t ADV_INTERVAL_MIN  = 160;   // 100 ms, in 0.625 ms units
static const uint16_t ADV_INTERVAL_MAX  = 240;   // 150 ms
static const size_t   FRAME_MAX         = 512;
static const int      ECHO_DEPTH        = 8;

struct Frame {
    uint16_t len;
    uint8_t  data[FRAME_MAX];
};

static QueueHandle_t          echo_queue = nullptr;
static NimBLECharacteristic*  data_char  = nullptr;
static Frame                  host_scratch;   // NimBLE host task only
static Frame                  loop_scratch;   // loop() only

// Written by the NimBLE host task, read by loop().
static volatile uint32_t rx_frames = 0, rx_bytes = 0, echo_drops = 0;
static volatile uint32_t notify_ok = 0, notify_fail = 0;
static volatile uint32_t connects = 0, disconnects = 0, scan_seen = 0, prns_seen = 0;
static volatile uint16_t connected = 0, mtu = 0;
// loop() only.
static uint32_t tx_frames = 0, tx_bytes = 0, last_report_ms = 0;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
        connects++;
        connected = server->getConnectedCount();
        // Advertising stops on connect; keep it up while there is room.
        if (connected < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) NimBLEDevice::startAdvertising();
    }
    void onDisconnect(NimBLEServer* server, ble_gap_conn_desc* desc) override {
        disconnects++;
        connected = server->getConnectedCount();
    }
    void onMTUChange(uint16_t value, ble_gap_conn_desc* desc) override {
        mtu = value;
    }
};

class DataCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, ble_gap_conn_desc* desc) override {
        NimBLEAttValue value = characteristic->getValue();
        rx_frames++;
        rx_bytes += value.length();
        host_scratch.len = value.length() < FRAME_MAX ? value.length() : FRAME_MAX;
        memcpy(host_scratch.data, value.data(), host_scratch.len);
        if (xQueueSend(echo_queue, &host_scratch, 0) != pdTRUE) echo_drops++;
    }
    // NimBLE reports every notification's outcome here (NimBLECharacteristic::
    // notify itself discards it).
    void onStatus(NimBLECharacteristic* characteristic, Status status, int code) override {
        if (status == SUCCESS_NOTIFY) notify_ok++;
        else notify_fail++;
    }
};

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* device) override {
        static const NimBLEUUID service(SERVICE_UUID);
        scan_seen++;
        if (device->isAdvertisingService(service)) prns_seen++;
    }
};

static ServerCallbacks server_callbacks;
static DataCallbacks   data_callbacks;
static ScanCallbacks   scan_callbacks;

inline void init() {
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_before    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    echo_queue = xQueueCreate(ECHO_DEPTH, sizeof(Frame));

    NimBLEDevice::init("");
    NimBLEDevice::setMTU(517);

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(&server_callbacks, false);
    NimBLEService* service = server->createService(SERVICE_UUID);
    data_char = service->createCharacteristic(DATA_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    data_char->setCallbacks(&data_callbacks);
    service->createCharacteristic(CONTROL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    service->createCharacteristic(COLUMBA_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    service->createCharacteristic(COLUMBA_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* identity = service->createCharacteristic(COLUMBA_ID_UUID, NIMBLE_PROPERTY::READ);
    uint8_t identity_bytes[16];
    esp_fill_random(identity_bytes, sizeof(identity_bytes));
    identity->setValue(identity_bytes, sizeof(identity_bytes));
    service->start();

    // Prns's advertisement: flags 06, the full service ID, manufacturer data
    // ff ff 03 <flags> — 27 bytes, no scan response.
    NimBLEAdvertisementData data;
    data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    data.setCompleteServices(NimBLEUUID(SERVICE_UUID));
    data.setManufacturerData(std::string("\xff\xff\x03\x00", 4));
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setAdvertisementData(data);
    advertising->setScanResponse(false);
    advertising->setMinInterval(ADV_INTERVAL_MIN);
    advertising->setMaxInterval(ADV_INTERVAL_MAX);
    advertising->start();

    if (RTNODE_BLE_SCAN_PERCENT > 0) {
        NimBLEScan* scan = NimBLEDevice::getScan();
        scan->setAdvertisedDeviceCallbacks(&scan_callbacks, false);
        scan->setActiveScan(false);
        scan->setInterval(SCAN_INTERVAL_MS);
        scan->setWindow(SCAN_INTERVAL_MS * RTNODE_BLE_SCAN_PERCENT / 100);
        scan->setMaxResults(0);
        scan->start(0, nullptr, false);
    }

    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t psram_after    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[BLE] init: internal %u -> %u (%+d), largest %u; psram %u -> %u (%+d); "
                  "max connections %d, scan %d%% of %u ms, address %s\r\n",
                  (unsigned)internal_before, (unsigned)internal_after,
                  (int)internal_after - (int)internal_before,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)psram_before, (unsigned)psram_after,
                  (int)psram_after - (int)psram_before,
                  CONFIG_BT_NIMBLE_MAX_CONNECTIONS, RTNODE_BLE_SCAN_PERCENT, (unsigned)SCAN_INTERVAL_MS,
                  NimBLEDevice::getAddress().toString().c_str());
    last_report_ms = millis();

#ifdef RTNODE_BLE_PS_PROBE
    // PERFORMANCE_STRATEGY.md asks whether WiFi can leave modem sleep with
    // Bluetooth on (ESP-IDF's coexistence guidance says it must not).
    wifi_ps_type_t ps_before = WIFI_PS_MIN_MODEM, ps_after = WIFI_PS_MIN_MODEM;
    esp_wifi_get_ps(&ps_before);
    esp_err_t set_none = esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_get_ps(&ps_after);
    Serial.printf("[BLE] WiFi power save %d; setting none returned %s; now %d\r\n",
                  (int)ps_before, esp_err_to_name(set_none), (int)ps_after);
    esp_wifi_set_ps(ps_before);
#endif
}

inline void loop() {
    // Echo what peers wrote, a few per pass.
    for (int i = 0; i < 4 && xQueueReceive(echo_queue, &loop_scratch, 0) == pdTRUE; i++) {
        data_char->notify(loop_scratch.data, loop_scratch.len);
        tx_frames++;
        tx_bytes += loop_scratch.len;
    }

    uint32_t now = millis();
    if (now - last_report_ms < 60000) return;
    last_report_ms = now;
    Serial.printf("[BLE] t=%lu conn=%u mtu=%u rx=%lu/%lu tx=%lu/%lu notify=%lu/%lu echo_drops=%lu "
                  "connects=%lu disconnects=%lu scan=%lu prns=%lu internal=%u/%u/%u\r\n",
                  (unsigned long)now, (unsigned)connected, (unsigned)mtu,
                  (unsigned long)rx_frames, (unsigned long)rx_bytes,
                  (unsigned long)tx_frames, (unsigned long)tx_bytes,
                  (unsigned long)notify_ok, (unsigned long)notify_fail,
                  (unsigned long)echo_drops, (unsigned long)connects, (unsigned long)disconnects,
                  (unsigned long)scan_seen, (unsigned long)prns_seen,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

}  // namespace ble_spike

#endif // FIREWALL_MODE && RTNODE_BLE_SPIKE
#endif // BLE_SPIKE_H
