// ============================================================
// wifi_api.cpp - Servidor HTTP REST (Safe Action & PCAP Edition)
// ============================================================
#include "wifi_api.h"
#include "config.h"
#include "wifi_handshake.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
extern MenuItem* currentMenuItems;
extern uint8_t currentMenuItemCount;
extern const char* currentMenuTitle;
extern int8_t menuIndex;
extern int8_t menuMaxIndex;
extern bool scannerRunning;
extern bool capturing;
extern unsigned long captureStartTime;
extern void enterMenu(MenuState state);
extern void goBack();

extern void scanNetworks();
extern uint8_t getNetworkCount();
extern NetworkInfo* getNetwork(uint8_t);
extern void startDeauth(uint8_t);
extern void stopDeauth();
extern void startEvilTwin(uint8_t);
extern void stopEvilTwin();
extern void startFakeAP(const char*);
extern void stopFakeAP();

extern bool nrf24JammerActive;
extern void nrf24StartJammer();
extern void nrf24StopJammer();
extern bool nrf24IsScanning();
extern void nrf24StartScan();
extern void nrf24StopScan();
extern void nrf24SpecStart();
extern void nrf24SpecStop();
extern const int8_t* nrf24GetScanBarData();
extern uint32_t nrf24GetScanTotalPackets();
extern uint8_t nrf24GetSavedCount();
extern uint8_t nrf24GetDetectedCount();
extern uint8_t nrf24GetAnalyzeSelected();

extern bool cc1101CopyActive;
extern void cc1101StartCapture();
extern void cc1101StopCapture();
extern void cc1101ReplaySignal(uint8_t);
extern uint8_t cc1101GetSavedCount();
extern SignalData* cc1101GetSignal(uint8_t);

extern uint8_t btDeviceCount;
extern void startBTScan();
extern void startBTJammer(uint8_t);
extern void stopBTJammer();
extern uint8_t getBTDeviceCount();
extern BTDevice* getBTDevice(uint8_t);

extern bool bfRunning;
extern void startGateBruteForce();
extern void stopBruteForce();
extern void startCarBruteForce(uint8_t);
extern uint16_t getCurrentBFIndex();
extern uint16_t getTotalBFCount(uint8_t, uint8_t);
extern uint8_t getCarBrandCount();

extern bool droneJammerActive;
extern void startDroneJammer();
extern void stopDroneJammer();
extern bool cameraFreezeActive;
extern void startCameraFreeze();
extern void stopCameraFreeze();

extern void initConnection(int);
extern void setBrightness(uint8_t brightness);

extern String getPcapData(); // Função criada no wifi_handshake.cpp

// ============================================================
// SERVER
// ============================================================
static WebServer apiServer(8080);
static bool apiRunning = false;

// Variáveis para execução segura de ações que mudam o estado do WiFi
static volatile bool pendingDeauthStart = false;
static volatile bool pendingEvilTwinStart = false;
static volatile bool pendingDeauthStop = false;
static volatile bool pendingEvilTwinStop = false;
static volatile int pendingNetIdx = -1;

static void sendJSON(int code, const String& json) {
    apiServer.sendHeader("Access-Control-Allow-Origin", "*");
    apiServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    apiServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    apiServer.sendHeader("Connection", "close");
    apiServer.send(code, "application/json", json);
}

static void sendOK(const String& msg) {
    DynamicJsonDocument doc(256);
    doc["status"] = "ok";
    doc["message"] = msg;
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

static void sendERR(const String& msg) {
    DynamicJsonDocument doc(256);
    doc["status"] = "error";
    doc["message"] = msg;
    String out;
    serializeJson(doc, out);
    sendJSON(400, out);
}

// ============================================================
// ENDPOINTS
// ============================================================

// GET /api/status
static void handleStatus() {
    DynamicJsonDocument doc(512);
    doc["menu"] = (int)currentMenu;
    doc["menu_name"] = currentMenuTitle ? currentMenuTitle : "";
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
    doc["network_count"] = networkCount;
    doc["btdevice_count"] = btDeviceCount;
    doc["cc1101_signals"] = cc1101GetSavedCount();
    doc["nrf24_signals"] = nrf24GetSavedCount();
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

// GET /api/networks
static void handleNetworks() {
    DynamicJsonDocument doc(2048);
    JsonArray nets = doc.createNestedArray("networks");
    for (int i = 0; i < (int)networkCount; i++) {
        NetworkInfo* net = &scannedNetworks[i];
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

// POST /api/networks/scan
static void handleScanNetworks() {
    yield();
    scanNetworks(); 
    yield();
    
    DynamicJsonDocument doc(2048);
    doc["status"] = "ok";
    doc["message"] = "Scan complete";
    doc["count"] = networkCount;

    JsonArray nets = doc.createNestedArray("networks");
    for (int i = 0; i < (int)networkCount; i++) {
        NetworkInfo* net = &scannedNetworks[i];
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

// POST /api/deauth/start?id=N
static void handleDeauthStart() {
    if (!apiServer.hasArg("id")) { sendERR("Missing id"); return; }
    int netIdx = apiServer.arg("id").toInt();
    if (netIdx < 0 || netIdx >= (int)networkCount) { sendERR("Invalid network id"); return; }
    
    pendingNetIdx = netIdx;
    pendingDeauthStart = true; 
    sendOK("Deauth started");
}

// POST /api/deauth/stop
static void handleDeauthStop() {
    pendingDeauthStop = true;
    sendOK("Deauth stopped");
}

// POST /api/eviltwin/start?id=N
static void handleEvilTwinStart() {
    if (!apiServer.hasArg("id")) { sendERR("Missing id"); return; }
    int netIdx = apiServer.arg("id").toInt();
    if (netIdx < 0 || netIdx >= (int)networkCount) { sendERR("Invalid network id"); return; }
    
    pendingNetIdx = netIdx;
    pendingEvilTwinStart = true;
    sendOK("Evil Twin started");
}

// POST /api/eviltwin/stop
static void handleEvilTwinStop() {
    pendingEvilTwinStop = true;
    sendOK("Evil Twin stopped");
}

// GET /api/handshake
static void handleHandshakeStatus() {
    DynamicJsonDocument doc(256);
    doc["capturing"] = isHandshakeCapturing();
    doc["complete"] = isHandshakeComplete();
    doc["frames"] = getHandshakeMessageCount();
    doc["status"] = getHandshakeStatus();
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

// GET /api/handshake/download
static void handleHandshakeDownload() {
    if (getHandshakeMessageCount() == 0) {
        sendERR("No handshake captured. Use Evil Twin first");
        return;
    }
    
    String pcapData = getPcapData();
    if (pcapData.length() == 0) {
        sendERR("Failed to build PCAP file.");
        return;
    }
    
    apiServer.sendHeader("Content-Disposition", "attachment; filename=handshake.pcap");
    apiServer.sendHeader("Connection", "close");
    apiServer.send(200, "application/vnd.tcpdump.pcap", pcapData);
}

// POST /api/nrf24/jammer/start
static void handleNRF24JammerStart() { if (!nrf24JammerActive) nrf24StartJammer(); sendOK("NRF24 Jammer started"); }
static void handleNRF24JammerStop() { nrf24StopJammer(); sendOK("NRF24 Jammer stopped"); }
static void handleNRF24ScannerStart() { scannerRunning = true; nrf24SpecStart(); sendOK("NRF24 Scanner started"); }
static void handleNRF24ScannerStop() { scannerRunning = false; nrf24SpecStop(); sendOK("NRF24 Scanner stopped"); }

static void handleNRF24ScanData() {
    DynamicJsonDocument doc(512);
    const int8_t* bars = nrf24GetScanBarData();
    JsonArray arr = doc.createNestedArray("bars");
    for (int i = 0; i < 16; i++) arr.add(bars[i]);
    doc["packets"] = nrf24GetScanTotalPackets();
    String out;
    serializeJson(doc, out);
    sendJSON(200, out);
}

static void handleCC1101Copy() {
    yield();
    if (!capturing) {
        capturing = true; captureStartTime = millis(); cc1101StartCapture(); capturing = false;
    }
    yield();
    sendOK("CC1101 copy started");
}

static void handleCC1101Replay() {
    if (!apiServer.hasArg("id")) { sendERR("Missing id"); return; }
    int idx = apiServer.arg("id").toInt();
    if (idx < (int)cc1101GetSavedCount()) { cc1101ReplaySignal(idx); sendOK("CC1101 replay"); } 
    else { sendERR("Invalid signal id"); }
}

static void handleCC1101Signals() {
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("signals");
    for (int i = 0; i < (int)cc1101GetSavedCount(); i++) {
        SignalData* sig = cc1101GetSignal(i);
        if (!sig || !sig->valid) continue;
        JsonObject obj = arr.createNestedObject();
        obj["id"] = i; obj["name"] = sig->name; obj["frequency"] = sig->frequency;
    }
    String out; serializeJson(doc, out); sendJSON(200, out);
}

static void handleDroneJammerStart() { if (!droneJammerActive) startDroneJammer(); sendOK("Drone jammer started"); }
static void handleDroneJammerStop() { stopDroneJammer(); sendOK("Drone jammer stopped"); }
static void handleCameraFreezeStart() { if (!cameraFreezeActive) startCameraFreeze(); sendOK("Camera freeze started"); }
static void handleCameraFreezeStop() { stopCameraFreeze(); sendOK("Camera freeze stopped"); }

static void handleBTScan() { btDeviceCount = 0; startBTScan(); sendOK("BT scan started"); }

static void handleBTDevices() {
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("devices");
    for (int i = 0; i < (int)btDeviceCount; i++) {
        BTDevice* dev = getBTDevice(i);
        if (!dev) continue;
        JsonObject obj = arr.createNestedObject();
        obj["id"] = i; obj["name"] = dev->name; obj["rssi"] = dev->rssi;
    }
    String out; serializeJson(doc, out); sendJSON(200, out);
}

static void handleBTJammerStart() {
    if (!apiServer.hasArg("id")) { sendERR("Missing id"); return; }
    int idx = apiServer.arg("id").toInt();
    if (idx < (int)btDeviceCount) { startBTJammer(idx); sendOK("BT jammer started"); } 
    else { sendERR("Invalid device id"); }
}
static void handleBTJammerStop() { stopBTJammer(); sendOK("BT jammer stopped"); }

static void handleBFGateStart() { if (!bfRunning) startGateBruteForce(); sendOK("BF Gate started"); }
static void handleBFGateStop() { stopBruteForce(); sendOK("BF Gate stopped"); }
static void handleBFCarStart() {
    if (!apiServer.hasArg("brand")) { sendERR("Missing brand"); return; }
    int brand = apiServer.arg("brand").toInt();
    if (brand < getCarBrandCount()) { if (!bfRunning) startCarBruteForce(brand); sendOK("BF Car started"); } 
    else { sendERR("Invalid brand"); }
}
static void handleBFCarStop() { stopBruteForce(); sendOK("BF Car stopped"); }

static void handleBFStatus() {
    DynamicJsonDocument doc(512);
    doc["running"] = bfRunning;
    doc["current_index"] = getCurrentBFIndex();
    doc["gate_total"] = getTotalBFCount(0, 0);
    doc["brand_count"] = getCarBrandCount();
    String out; serializeJson(doc, out); sendJSON(200, out);
}

static void handleSetBrightness() {
    if (!apiServer.hasArg("value")) { sendERR("Missing value"); return; }
    int val = apiServer.arg("value").toInt();
    if (val >= 0 && val <= 255) { screenBrightness = val; setBrightness(screenBrightness); sendOK("Brightness set"); } 
    else { sendERR("Invalid value (0-255)"); }
}

static void handleMenuNavigate() {
    if (!apiServer.hasArg("to")) { sendERR("Missing to"); return; }
    String target = apiServer.arg("to");
    if (target == "MAIN") enterMenu(MENU_MAIN);
    else if (target == "NRF24") enterMenu(MENU_NRF24);
    else if (target == "CC1101") enterMenu(MENU_CC1101);
    else if (target == "ATTACKS") enterMenu(MENU_ATTACKS);
    else if (target == "NETWORKS") enterMenu(MENU_NETWORKS);
    else if (target == "SETTINGS") enterMenu(MENU_SETTINGS);
    else { sendERR("Unknown menu"); return; }
    sendOK("Menu changed");
}

static void handleButton() {
    if (!apiServer.hasArg("action")) { sendERR("Missing action"); return; }
    String action = apiServer.arg("action");
    if (action == "UP") { if (menuIndex > 0) menuIndex--; } 
    else if (action == "DOWN") { if (menuIndex < menuMaxIndex) menuIndex++; } 
    else if (action == "SELECT") { if (currentMenuItems && menuIndex < (int)currentMenuItemCount) enterMenu(currentMenuItems[menuIndex].state); } 
    else if (action == "BACK") { goBack(); } 
    else { sendERR("Unknown action"); return; }
    sendOK("Button " + action + " pressed");
}

// ============================================================
// SETUP
// ============================================================
void startAPIServer() {
    if (apiRunning) return;

    apiServer.onNotFound([]() {
        if (apiServer.method() == HTTP_OPTIONS) {
            apiServer.sendHeader("Access-Control-Allow-Origin", "*");
            apiServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            apiServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
            apiServer.send(204);
            return;
        }
        sendERR("Endpoint not found: " + apiServer.uri());
    });

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

    // Processa ações pendentes com segurança (fora do handler HTTP)
    if (pendingDeauthStart) {
        pendingDeauthStart = false;
        startDeauth(pendingNetIdx);
    }
    if (pendingEvilTwinStart) {
        pendingEvilTwinStart = false;
        startEvilTwin(pendingNetIdx);
    }
    if (pendingDeauthStop) {
        pendingDeauthStop = false;
        stopDeauth();
    }
    if (pendingEvilTwinStop) {
        pendingEvilTwinStop = false;
        stopEvilTwin();
    }
}

bool isAPIServerRunning() { return apiRunning; }
