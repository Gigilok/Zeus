// ============================================================
// wifi_api.cpp - Servidor HTTP REST para controle remoto
// Roda em paralelo com todo o sistema
// Endpoints: /api/*
// ============================================================
#include "wifi_api.h"
#include "config.h"
#include "wifi_handshake.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// Forward declarations de menu.cpp
extern bool scannerRunning;
extern bool capturing;
extern unsigned long captureStartTime;
extern int8_t menuIndex;
extern int8_t menuMaxIndex;
extern MenuItem* currentMenuItems;
extern uint8_t currentMenuItemCount;

static WebServer apiServer(8080);
static bool apiRunning = false;

// ============================================================
// HELPERS JSON
// ============================================================
static void sendJSON(int code, const String& json) {
    apiServer.send(code, "application/json", json);
}

static void sendOK(const String& msg) {
    StaticJsonDocument<256> doc;
    doc["status"] = "ok";
    doc["message"] = msg;
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

static void sendERR(const String& msg) {
    StaticJsonDocument<256> doc;
    doc["status"] = "error";
    doc["message"] = msg;
    String out;
    serializeJson(doc, out);
    sendJSON(400, out);
}

// ============================================================
// ENDPOINTS
// ============================================================

// GET /api/status - Status completo do sistema
static void handleStatus() {
    StaticJsonDocument<512> doc;
    doc["menu"] = (int)currentMenu;
    doc["menu_name"] = currentMenuTitle;
    doc["deauth_active"] = deauthActive;
    doc["eviltwin_active"] = evilTwinActive;
    doc["handshake_status"] = getHandshakeStatus();
    doc["handshake_complete"] = isHandshakeComplete();
    doc["nrf24_jammer"] = nrf24JammerActive;
    doc["nrf24_scanning"] = nrf24IsScanning();
    doc["cc1101_capturing"] = cc1101CopyActive;
    doc["drone_jammer"] = droneJammerActive;
    doc["camera_freeze"] = cameraFreezeActive;
    doc["bt_jammer"] = btJammerActive;
    doc["bruteforce"] = bfRunning;
    doc["network_count"] = getNetworkCount();
    doc["btdevice_count"] = getBTDeviceCount();
    doc["cc1101_signals"] = cc1101GetSavedCount();
    doc["nrf24_signals"] = nrf24GetSavedCount();
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

// GET /api/networks - Lista redes WiFi
static void handleNetworks() {
    StaticJsonDocument<2048> doc;
    JsonArray nets = doc.createNestedArray("networks");
    for (int i = 0; i < (int)getNetworkCount(); i++) {
        NetworkInfo* net = getNetwork(i);
        if (!net) continue;
        JsonObject obj = nets.createNestedObject();
        obj["id"] = i;
        obj["ssid"] = net->ssid;
        obj["channel"] = net->channel;
        obj["rssi"] = net->rssi;
        obj["encrypted"] = net->encrypted;
        char bssid[18];
        snprintf(bssid, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
            net->bssid[0], net->bssid[1], net->bssid[2],
            net->bssid[3], net->bssid[4], net->bssid[5]);
        obj["bssid"] = bssid;
    }
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

// POST /api/networks/scan - Escanear redes
static void handleScanNetworks() {
    scanNetworks();
    sendOK("Scan complete");
}

// POST /api/deauth/start?id=N - Iniciar deauth
static void handleDeauthStart() {
    if (!apiServer.hasArg("id")) {
        sendERR("Missing id parameter");
        return;
    }
    int netIdx = apiServer.arg("id").toInt();
    if (netIdx < 0 || netIdx >= (int)getNetworkCount()) {
        sendERR("Invalid network id");
        return;
    }
    startDeauth(netIdx);
    sendOK("Deauth started");
}

// POST /api/deauth/stop - Parar deauth
static void handleDeauthStop() {
    stopDeauth();
    sendOK("Deauth stopped");
}

// POST /api/eviltwin/start?id=N - Iniciar Evil Twin + Handshake
static void handleEvilTwinStart() {
    if (!apiServer.hasArg("id")) {
        sendERR("Missing id parameter");
        return;
    }
    int netIdx = apiServer.arg("id").toInt();
    if (netIdx < 0 || netIdx >= (int)getNetworkCount()) {
        sendERR("Invalid network id");
        return;
    }
    startEvilTwin(netIdx);
    sendOK("Evil Twin started");
}

// POST /api/eviltwin/stop - Parar Evil Twin
static void handleEvilTwinStop() {
    stopEvilTwin();
    sendOK("Evil Twin stopped");
}

// GET /api/handshake - Status do handshake
static void handleHandshakeStatus() {
    StaticJsonDocument<256> doc;
    doc["capturing"] = isHandshakeCapturing();
    doc["complete"] = isHandshakeComplete();
    doc["frames"] = getHandshakeMessageCount();
    doc["status"] = getHandshakeStatus();
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

// GET /api/handshake/download - Baixar handshake como .cap
static void handleHandshakeDownload() {
    if (getHandshakeMessageCount() == 0) {
        sendERR("No handshake captured");
        return;
    }

    // PCAP Global Header
    uint8_t pcapHeader[24] = {
        0xD4, 0xC3, 0xB2, 0xA1, 0x02, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0x00, 0x00, 0x69, 0x00, 0x00, 0x00
    };

    apiServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
    apiServer.send(200, "application/vnd.tcpdump.pcap", "");
    apiServer.sendContent((const char*)pcapHeader, 24);

    // Frames EAPOL (acessar diretamente do buffer interno)
    // Precisamos exportar uma funcao do wifi_handshake.cpp
    // Por enquanto, enviamos um placeholder
    apiServer.sendContent("", 0);
}

// POST /api/nrf24/jammer/start - NRF24 Jammer
static void handleNRF24JammerStart() {
    if (!nrf24JammerActive) nrf24StartJammer();
    sendOK("NRF24 Jammer started");
}

// POST /api/nrf24/jammer/stop
static void handleNRF24JammerStop() {
    nrf24StopJammer();
    sendOK("NRF24 Jammer stopped");
}

// POST /api/nrf24/scanner/start
static void handleNRF24ScannerStart() {
    scannerRunning = true;
    nrf24SpecStart();
    sendOK("NRF24 Scanner started");
}

// POST /api/nrf24/scanner/stop
static void handleNRF24ScannerStop() {
    scannerRunning = false;
    nrf24SpecStop();
    sendOK("NRF24 Scanner stopped");
}

// GET /api/nrf24/scan - Dados do scanner
static void handleNRF24ScanData() {
    StaticJsonDocument<512> doc;
    const int8_t* bars = nrf24GetScanBarData();
    JsonArray arr = doc.createNestedArray("bars");
    for (int i = 0; i < 16; i++) arr.add(bars[i]);
    doc["packets"] = nrf24GetScanTotalPackets();
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

// POST /api/cc1101/copy - Copiar sinal
static void handleCC1101Copy() {
    if (!capturing) {
        capturing = true;
        captureStartTime = millis();
        cc1101StartCapture();
        capturing = false;
    }
    sendOK("CC1101 copy started");
}

// POST /api/cc1101/replay?id=N
static void handleCC1101Replay() {
    if (!apiServer.hasArg("id")) {
        sendERR("Missing id");
        return;
    }
    int idx = apiServer.arg("id").toInt();
    if (idx < (int)cc1101GetSavedCount()) {
        cc1101ReplaySignal(idx);
        sendOK("CC1101 replay");
    } else {
        sendERR("Invalid signal id");
    }
}

// GET /api/cc1101/signals
static void handleCC1101Signals() {
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.createNestedArray("signals");
    for (int i = 0; i < (int)cc1101GetSavedCount(); i++) {
        SignalData* sig = cc1101GetSignal(i);
        if (!sig || !sig->valid) continue;
        JsonObject obj = arr.createNestedObject();
        obj["id"] = i;
        obj["name"] = sig->name;
        obj["frequency"] = sig->frequency;
    }
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

// POST /api/attack/drone/jammer/start
static void handleDroneJammerStart() {
    if (!droneJammerActive) startDroneJammer();
    sendOK("Drone jammer started");
}

// POST /api/attack/drone/jammer/stop
static void handleDroneJammerStop() {
    stopDroneJammer();
    sendOK("Drone jammer stopped");
}

// POST /api/attack/camera/freeze/start
static void handleCameraFreezeStart() {
    if (!cameraFreezeActive) startCameraFreeze();
    sendOK("Camera freeze started");
}

// POST /api/attack/camera/freeze/stop
static void handleCameraFreezeStop() {
    stopCameraFreeze();
    sendOK("Camera freeze stopped");
}

// POST /api/attack/bt/scan
static void handleBTScan() {
    btDeviceCount = 0;
    startBTScan();
    sendOK("BT scan started");
}

// GET /api/attack/bt/devices
static void handleBTDevices() {
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.createNestedArray("devices");
    for (int i = 0; i < (int)getBTDeviceCount(); i++) {
        BTDevice* dev = getBTDevice(i);
        if (!dev) continue;
        JsonObject obj = arr.createNestedObject();
        obj["id"] = i;
        obj["name"] = dev->name;
        obj["rssi"] = dev->rssi;
    }
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

// POST /api/attack/bt/jammer/start?id=N
static void handleBTJammerStart() {
    if (!apiServer.hasArg("id")) {
        sendERR("Missing id");
        return;
    }
    int idx = apiServer.arg("id").toInt();
    if (idx < (int)getBTDeviceCount()) {
        startBTJammer(idx);
        sendOK("BT jammer started");
    } else {
        sendERR("Invalid device id");
    }
}

// POST /api/attack/bt/jammer/stop
static void handleBTJammerStop() {
    stopBTJammer();
    sendOK("BT jammer stopped");
}

// POST /api/attack/bf/gate/start
static void handleBFGateStart() {
    if (!bfRunning) startGateBruteForce();
    sendOK("BF Gate started");
}

// POST /api/attack/bf/gate/stop
static void handleBFGateStop() {
    stopBruteForce();
    sendOK("BF Gate stopped");
}

// POST /api/attack/bf/car/start?brand=N
static void handleBFCarStart() {
    if (!apiServer.hasArg("brand")) {
        sendERR("Missing brand");
        return;
    }
    int brand = apiServer.arg("brand").toInt();
    if (brand < getCarBrandCount()) {
        if (!bfRunning) startCarBruteForce(brand);
        sendOK("BF Car started");
    } else {
        sendERR("Invalid brand");
    }
}

// POST /api/attack/bf/car/stop
static void handleBFCarStop() {
    stopBruteForce();
    sendOK("BF Car stopped");
}

// GET /api/attack/bf/status
static void handleBFStatus() {
    StaticJsonDocument<512> doc;
    doc["running"] = bfRunning;
    doc["current_index"] = getCurrentBFIndex();
    doc["gate_total"] = getTotalBFCount(0, 0);
    doc["brand_count"] = getCarBrandCount();
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

// POST /api/settings/brightness?value=N
static void handleSetBrightness() {
    if (!apiServer.hasArg("value")) {
        sendERR("Missing value");
        return;
    }
    int val = apiServer.arg("value").toInt();
    if (val >= 0 && val <= 255) {
        screenBrightness = val;
        setBrightness(screenBrightness);
        sendOK("Brightness set");
    } else {
        sendERR("Invalid value (0-255)");
    }
}

// POST /api/menu/navigate?to=MENU_NAME
static void handleMenuNavigate() {
    if (!apiServer.hasArg("to")) {
        sendERR("Missing to parameter");
        return;
    }
    String target = apiServer.arg("to");
    if (target == "MAIN") enterMenu(MENU_MAIN);
    else if (target == "NRF24") enterMenu(MENU_NRF24);
    else if (target == "CC1101") enterMenu(MENU_CC1101);
    else if (target == "ATTACKS") enterMenu(MENU_ATTACKS);
    else if (target == "NETWORKS") enterMenu(MENU_NETWORKS);
    else if (target == "SETTINGS") enterMenu(MENU_SETTINGS);
    else {
        sendERR("Unknown menu");
        return;
    }
    sendOK("Menu changed");
}

// POST /api/btn?action=UP|DOWN|SELECT|BACK
static void handleButton() {
    if (!apiServer.hasArg("action")) {
        sendERR("Missing action");
        return;
    }
    String action = apiServer.arg("action");
    if (action == "UP") {
        if (menuIndex > 0) menuIndex--;
    } else if (action == "DOWN") {
        if (menuIndex < menuMaxIndex) menuIndex++;
    } else if (action == "SELECT") {
        if (currentMenuItems && menuIndex < (int)currentMenuItemCount)
            enterMenu(currentMenuItems[menuIndex].state);
    } else if (action == "BACK") {
        goBack();
    } else {
        sendERR("Unknown action");
        return;
    }
    sendOK("Button " + action + " pressed");
}

// CORS preflight
static void handleCORS() {
    apiServer.sendHeader("Access-Control-Allow-Origin", "*");
    apiServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    apiServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    apiServer.send(204);
}

// ============================================================
// SETUP
// ============================================================
void startAPIServer() {
    if (apiRunning) return;

    apiServer.on("/api/status", HTTP_GET, handleStatus);
    apiServer.on("/api/networks", HTTP_GET, handleNetworks);
    apiServer.on("/api/networks/scan", HTTP_POST, handleScanNetworks);
    apiServer.on("/api/deauth/start", HTTP_POST, handleDeauthStart);
    apiServer.on("/api/deauth/stop", HTTP_POST, handleDeauthStop);
    apiServer.on("/api/eviltwin/start", HTTP_POST, handleEvilTwinStart);
    apiServer.on("/api/eviltwin/stop", HTTP_POST, handleEvilTwinStop);
    apiServer.on("/api/handshake", HTTP_GET, handleHandshakeStatus);
    apiServer.on("/api/handshake/download", HTTP_GET, handleHandshakeDownload);
    apiServer.on("/api/nrf24/jammer/start", HTTP_POST, handleNRF24JammerStart);
    apiServer.on("/api/nrf24/jammer/stop", HTTP_POST, handleNRF24JammerStop);
    apiServer.on("/api/nrf24/scanner/start", HTTP_POST, handleNRF24ScannerStart);
    apiServer.on("/api/nrf24/scanner/stop", HTTP_POST, handleNRF24ScannerStop);
    apiServer.on("/api/nrf24/scan", HTTP_GET, handleNRF24ScanData);
    apiServer.on("/api/cc1101/copy", HTTP_POST, handleCC1101Copy);
    apiServer.on("/api/cc1101/replay", HTTP_POST, handleCC1101Replay);
    apiServer.on("/api/cc1101/signals", HTTP_GET, handleCC1101Signals);
    apiServer.on("/api/attack/drone/jammer/start", HTTP_POST, handleDroneJammerStart);
    apiServer.on("/api/attack/drone/jammer/stop", HTTP_POST, handleDroneJammerStop);
    apiServer.on("/api/attack/camera/freeze/start", HTTP_POST, handleCameraFreezeStart);
    apiServer.on("/api/attack/camera/freeze/stop", HTTP_POST, handleCameraFreezeStop);
    apiServer.on("/api/attack/bt/scan", HTTP_POST, handleBTScan);
    apiServer.on("/api/attack/bt/devices", HTTP_GET, handleBTDevices);
    apiServer.on("/api/attack/bt/jammer/start", HTTP_POST, handleBTJammerStart);
    apiServer.on("/api/attack/bt/jammer/stop", HTTP_POST, handleBTJammerStop);
    apiServer.on("/api/attack/bf/gate/start", HTTP_POST, handleBFGateStart);
    apiServer.on("/api/attack/bf/gate/stop", HTTP_POST, handleBFGateStop);
    apiServer.on("/api/attack/bf/car/start", HTTP_POST, handleBFCarStart);
    apiServer.on("/api/attack/bf/car/stop", HTTP_POST, handleBFCarStop);
    apiServer.on("/api/attack/bf/status", HTTP_GET, handleBFStatus);
    apiServer.on("/api/settings/brightness", HTTP_POST, handleSetBrightness);
    apiServer.on("/api/menu/navigate", HTTP_POST, handleMenuNavigate);
    apiServer.on("/api/btn", HTTP_POST, handleButton);

    // CORS
    apiServer.on("/api/", HTTP_OPTIONS, handleCORS);

    apiServer.begin();
    apiRunning = true;
    Serial.println("[API] HTTP Server started on :8080");
}

void stopAPIServer() {
    if (!apiRunning) return;
    apiServer.stop();
    apiRunning = false;
    Serial.println("[API] HTTP Server stopped");
}

void apiLoop() {
    if (apiRunning) apiServer.handleClient();
}

bool isAPIServerRunning() { return apiRunning; }
