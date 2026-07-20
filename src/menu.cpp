#include "config.h"
#include <Adafruit_SSD1306.h>

struct NRFDevice {
    uint8_t address[5];
    uint8_t channel;
    int8_t rssi;
};

// Forward declarations
extern void clearDisplay();
extern void updateDisplay();
extern void drawMenuHeader(const char* title);
extern void drawMenuItem(int y, const char* text, bool selected);
extern void drawStatusBar(const char* status);
extern void drawProgressBar(int x, int y, int width, int height, int percent);
extern void drawText(int x, int y, const char* text, uint8_t size);
extern void drawCenteredText(int y, const char* text, uint8_t size);
extern void showMessage(const char* title, const char* message);
extern void showLoading(const char* text, int percent);
extern void showSplashScreen();
extern void setBrightness(uint8_t brightness);
extern Adafruit_SSD1306& getDisplay();
extern NRFDevice* nrf24GetDevice(uint8_t);

extern ButtonState readButtons();

extern bool nrf24IsAvailable();
extern void nrf24StartJammer();
extern void nrf24StopJammer();
extern void nrf24Scan();
extern uint8_t nrf24GetDeviceCount();
extern bool nrf24IsJammerActive();
extern int nrf24JammerLoop();

extern bool cc1101IsAvailable();
extern void cc1101StartCapture();
extern void cc1101StopCapture();
extern void cc1101ReplaySignal(uint8_t index);
extern bool cc1101IsCapturing();
extern uint8_t cc1101GetSavedCount();
extern SignalData* cc1101GetSignal(uint8_t);

extern void scanNetworks();
extern uint8_t getNetworkCount();
extern NetworkInfo* getNetwork(uint8_t);
extern void startDeauth(uint8_t);
extern void stopDeauth();
extern void startFakeAP(const char*);
extern void stopFakeAP();
extern void startEvilTwin(uint8_t);
extern void startCameraFreeze();
extern void stopCameraFreeze();
extern void startDroneJammer();
extern void stopDroneJammer();
extern void startDroneLocate();
struct DroneLocation { float distance; int8_t rssi; };
extern DroneLocation* getDroneLocation();
extern void scanRemoteDevices();
extern uint8_t getRemoteDeviceCount();
extern RemoteDevice* getRemoteDevice(uint8_t);

extern void startBTScan();
extern uint8_t getBTDeviceCount();
extern BTDevice* getBTDevice(uint8_t);
extern void startBTJammer(uint8_t);
extern void stopBTJammer();

extern void startGateBruteForce();
extern void startCarBruteForce(uint8_t);
extern void stopBruteForce();
extern bool isBruteForceRunning();
extern uint16_t getCurrentBFIndex();
extern uint16_t getTotalBFCount(uint8_t type, uint8_t brand);
extern const char* getCarBrandName(uint8_t);
extern uint8_t getCarBrandCount();

extern void testAllPins();
extern uint8_t getPinTestCount();
struct PinTest { const char* name; bool working; };
extern PinTest* getPinTest(uint8_t);
extern void testModules(bool, bool, bool);
extern uint8_t getModuleCount();
struct ModuleStatus { const char* name; bool connected; bool working; };
extern ModuleStatus* getModule(uint8_t);
extern void initConnection(int);
extern void disconnectConnection();
extern bool isConnectionActive();
extern const char* getConnectionTypeName();
extern const char* getPairingCode();

// ============================================================
// MENU STRUCTURES
// ============================================================
struct MenuItem {
    const char* label;
    MenuState state;
};

MenuItem mainMenu[] = {
    {"NRF24", MENU_NRF24},
    {"CC1101", MENU_CC1101},
    {"Ataques", MENU_ATTACKS},
    {"Redes", MENU_NETWORKS},
    {"Config", MENU_SETTINGS}
};
const uint8_t mainMenuCount = 5;

MenuItem nrf24Menu[] = {{"Jammer", MENU_NRF24_JAMMER}, {"Scanner", MENU_NRF24_SCANNER}};
MenuItem cc1101Menu[] = {{"Copiar", MENU_CC1101_COPY}, {"Reproduzir", MENU_CC1101_REPLAY}};
MenuItem attacksMenu[] = {
    {"Drone", MENU_ATTACK_DRONE}, {"Deauth", MENU_ATTACK_DEAUTH},
    {"Camera", MENU_ATTACK_CAMERA}, {"Bluetooth", MENU_ATTACK_BLUETOOTH},
    {"BruteForce", MENU_ATTACK_BRUTEFORCE}
};
MenuItem droneMenu[] = {{"Jammer", MENU_ATTACK_DRONE_JAMMER}, {"Remoto", MENU_ATTACK_DRONE_REMOTE}, {"Localizar", MENU_ATTACK_DRONE_LOCATE}};
MenuItem cameraMenu[] = {{"Congelar", MENU_ATTACK_CAMERA_FREEZE}, {"Deauth", MENU_ATTACK_DEAUTH}};
MenuItem bfMenu[] = {{"Portao", MENU_ATTACK_BF_GATE}, {"Carro", MENU_ATTACK_BF_CAR}};
MenuItem networksMenu[] = {{"Senha", MENU_NET_PASSWORD}, {"Deauth", MENU_NET_DEAUTH}, {"Remoto", MENU_NET_REMOTE}};
MenuItem settingsMenu[] = {{"Pinos", MENU_SETTINGS_PINS}, {"Modulos", MENU_SETTINGS_MODULES}, {"Brilho", MENU_SETTINGS_BRIGHTNESS}, {"Conexao", MENU_SETTINGS_CONNECTION}};

// ============================================================
// MENU STATE
// ============================================================
MenuItem* currentMenuItems = nullptr;
uint8_t currentMenuItemCount = 0;
const char* currentMenuTitle = "";
int8_t listIndex = 0;
int8_t listMaxIndex = 0;
bool inListView = false;
bool scanning = false;
bool capturing = false;
unsigned long captureStartTime = 0;

// ============================================================
// MENU NAVIGATION
// ============================================================
void setMenu(MenuItem* items, uint8_t count, const char* title) {
    currentMenuItems = items;
    currentMenuItemCount = count;
    currentMenuTitle = title;
    menuIndex = 0;
    menuMaxIndex = count - 1;
    inListView = false;
}

void enterMenu(MenuState state) {
    previousMenu = currentMenu;
    currentMenu = state;
    switch (state) {
        case MENU_MAIN: setMenu(mainMenu, mainMenuCount, "MENU"); break;
        case MENU_NRF24: setMenu(nrf24Menu, 2, "NRF24"); break;
        case MENU_CC1101: setMenu(cc1101Menu, 2, "CC1101"); break;
        case MENU_ATTACKS: setMenu(attacksMenu, 5, "ATAQUES"); break;
        case MENU_ATTACK_DRONE: setMenu(droneMenu, 3, "DRONE"); break;
        case MENU_ATTACK_CAMERA: setMenu(cameraMenu, 2, "CAMERA"); break;
        case MENU_ATTACK_BRUTEFORCE: setMenu(bfMenu, 2, "BRUTEFORCE"); break;
        case MENU_NETWORKS: setMenu(networksMenu, 3, "REDES"); break;
        case MENU_SETTINGS: setMenu(settingsMenu, 4, "CONFIG"); break;
        default: break;
    }
}

void goBack() {
    switch (currentMenu) {
        case MENU_NRF24: case MENU_CC1101: case MENU_ATTACKS: case MENU_NETWORKS: case MENU_SETTINGS:
            enterMenu(MENU_MAIN); break;
        case MENU_NRF24_JAMMER: case MENU_NRF24_SCANNER: enterMenu(MENU_NRF24); break;
        case MENU_CC1101_COPY: case MENU_CC1101_REPLAY: enterMenu(MENU_CC1101); break;
        case MENU_ATTACK_DRONE: case MENU_ATTACK_DEAUTH: case MENU_ATTACK_CAMERA: case MENU_ATTACK_BLUETOOTH: case MENU_ATTACK_BRUTEFORCE:
            enterMenu(MENU_ATTACKS); break;
        case MENU_ATTACK_DRONE_JAMMER: case MENU_ATTACK_DRONE_REMOTE: case MENU_ATTACK_DRONE_LOCATE: enterMenu(MENU_ATTACK_DRONE); break;
        case MENU_ATTACK_CAMERA_FREEZE: enterMenu(MENU_ATTACK_CAMERA); break;
        case MENU_ATTACK_BF_GATE: case MENU_ATTACK_BF_CAR: enterMenu(MENU_ATTACK_BRUTEFORCE); break;
        case MENU_NET_PASSWORD: case MENU_NET_DEAUTH: case MENU_NET_REMOTE: enterMenu(MENU_NETWORKS); break;
        case MENU_SETTINGS_PINS: case MENU_SETTINGS_MODULES: case MENU_SETTINGS_BRIGHTNESS: case MENU_SETTINGS_CONNECTION:
            enterMenu(MENU_SETTINGS); break;
        default: enterMenu(MENU_MAIN); break;
    }
    if (nrf24JammerActive) { nrf24StopJammer(); }
    if (nrf24JammerActive) nrf24StopJammer();
    if (deauthActive) stopDeauth();
    if (cameraFreezeActive) stopCameraFreeze();
    if (droneJammerActive) stopDroneJammer();
    if (btJammerActive) stopBTJammer();
    if (bfRunning) stopBruteForce();
    if (capturing) { cc1101StopCapture(); capturing = false; }
    if (inListView) inListView = false;
}

// ============================================================
// RENDER HELPERS
// ============================================================
void renderMenu() {
    clearDisplay();
    drawMenuHeader(currentMenuTitle);
    int startY = 12;
    int visibleItems = 5;
    int startIndex = (menuIndex >= visibleItems) ? menuIndex - visibleItems + 1 : 0;
    for (int i = 0; i < visibleItems && (startIndex + i) < (int)currentMenuItemCount; i++) {
        drawMenuItem(startY + i * 10, currentMenuItems[startIndex + i].label, (startIndex + i) == menuIndex);
    }
    updateDisplay();
}

void renderList(const char* title, int count, void (*drawItem)(int, int, bool)) {
    clearDisplay();
    drawMenuHeader(title);
    int visibleItems = 5;
    int startIndex = (listIndex >= visibleItems) ? listIndex - visibleItems + 1 : 0;
    for (int i = 0; i < visibleItems && (startIndex + i) < count; i++) {
        drawItem(startIndex + i, 12 + i * 10, (startIndex + i) == listIndex);
    }
    updateDisplay();
}

// ============================================================
// SCREEN RENDERERS
// ============================================================
void renderNRF24Jammer() {
    clearDisplay();
    drawMenuHeader("NRF24");
    if (nrf24IsJammerActive()) {
        int ch = nrf24JammerLoop();
        int pct = (ch * 100) / 125;
        drawCenteredText(18, "JAM", 2);
        drawProgressBar(14, 38, 100, 8, pct);
    } else {
        drawCenteredText(32, "JAM", 2);
    }
    updateDisplay();
}

void renderNRF24Scanner() {
    clearDisplay();
    drawMenuHeader("NRF24 SCAN");
    if (scanning) {
        drawCenteredText(30, "Escaneando...", 1);
        int pct = ((millis() - captureStartTime) * 100) / 5000;
        if (pct > 100) pct = 100;
        drawProgressBar(14, 45, 100, 8, pct);
    } else {
        if (nrf24GetDeviceCount() == 0) {
            drawCenteredText(25, "Nenhum", 1);
            drawCenteredText(38, "dispositivo", 1);
        } else {
            drawText(0, 14, "Encontrados:", 1);
            for (int i = 0; i < (int)nrf24GetDeviceCount() && i < 4; i++) {
                char buf[32];
                snprintf(buf, 32, "CH:%d", nrf24GetDevice(i)->channel);
                drawText(0, 24 + i * 10, buf, 1);
            }
        }
        drawCenteredText(55, "SEL: Scan", 1);
    }
    updateDisplay();
}

void renderCC1101Copy() {
    clearDisplay();
    drawMenuHeader("COPIAR SINAL");
    if (capturing) {
        unsigned long elapsed = millis() - captureStartTime;
        int pct = (elapsed * 100) / CAPTURE_DURATION;
        if (pct > 100) pct = 100;
        char buf[32];
        snprintf(buf, 32, "%lu ms", elapsed);
        drawCenteredText(20, "Capturando...", 1);
        drawCenteredText(32, buf, 1);
        drawProgressBar(14, 45, 100, 8, pct);
    } else {
        char buf[32];
        snprintf(buf, 32, "Sinais: %d", cc1101GetSavedCount());
        drawCenteredText(25, buf, 1);
        drawCenteredText(40, "SEL: Iniciar", 1);
        drawCenteredText(52, "BACK: Voltar", 1);
    }
    updateDisplay();
}

void drawSignalItem(int index, int y, bool selected) {
    if (selected) {
        getDisplay().fillRect(0, y, 128, 10, 1);
        getDisplay().setTextColor(0);
    } else {
        getDisplay().setTextColor(1);
    }
    SignalData* sig = cc1101GetSignal(index);
    if (sig && sig->valid) {
        char buf[32];
        snprintf(buf, 32, "%s %luMHz", sig->name, sig->frequency / 1000000);
        drawText(4, y + 1, buf, 1);
    }
    if (selected) getDisplay().setTextColor(1);
}

void renderCC1101Replay() {
    if (!inListView) {
        inListView = true;
        listIndex = 0;
        listMaxIndex = cc1101GetSavedCount() - 1;
        if (listMaxIndex < 0) listMaxIndex = 0;
    }
    if (cc1101GetSavedCount() == 0) {
        clearDisplay();
        drawMenuHeader("REPRODUZIR");
        drawCenteredText(30, "Nenhum sinal", 1);
        updateDisplay();
        return;
    }
    renderList("REPRODUZIR", cc1101GetSavedCount(), drawSignalItem);
}

void drawNetworkItem(int index, int y, bool selected) {
    if (selected) {
        getDisplay().fillRect(0, y, 128, 10, 1);
        getDisplay().setTextColor(0);
    } else {
        getDisplay().setTextColor(1);
    }
    NetworkInfo* net = getNetwork(index);
    if (net) {
        char buf[32];
        snprintf(buf, 32, "%s [%d]", net->ssid, net->rssi);
        drawText(4, y + 1, buf, 1);
    }
    if (selected) getDisplay().setTextColor(1);
}

void renderDeauth() {
    if (!inListView) {
        inListView = true;
        listIndex = 0;
        listMaxIndex = getNetworkCount() - 1;
        if (listMaxIndex < 0) listMaxIndex = 0;
        if (getNetworkCount() == 0) {
            showLoading("Escaneando...", 0);
            scanNetworks();
            listMaxIndex = getNetworkCount() - 1;
            if (listMaxIndex < 0) listMaxIndex = 0;
        }
    }
    if (getNetworkCount() == 0) {
        clearDisplay();
        drawMenuHeader("DEAUTH");
        drawCenteredText(30, "Sem redes", 1);
        updateDisplay();
        return;
    }
    if (deauthActive) {
        clearDisplay();
        drawMenuHeader("DEAUTH");
        NetworkInfo* net = getNetwork(listIndex);
        if (net) {
            char buf[64];
            snprintf(buf, 64, "Desautenticando:\n%s", net->ssid);
            drawText(0, 14, buf, 1);
        }
        drawCenteredText(50, "SEL: Parar", 1);
        updateDisplay();
    } else {
        renderList("DEAUTH", getNetworkCount(), drawNetworkItem);
    }
}

void renderPassword() {
    if (!inListView) {
        inListView = true;
        listIndex = 0;
        listMaxIndex = getNetworkCount() - 1;
        if (listMaxIndex < 0) listMaxIndex = 0;
        if (getNetworkCount() == 0) {
            showLoading("Escaneando...", 0);
            scanNetworks();
            listMaxIndex = getNetworkCount() - 1;
            if (listMaxIndex < 0) listMaxIndex = 0;
        }
    }
    if (getNetworkCount() == 0) {
        clearDisplay();
        drawMenuHeader("SENHA");
        drawCenteredText(30, "Sem redes", 1);
        updateDisplay();
        return;
    }
    if (passwordCaptured) {
        clearDisplay();
        drawMenuHeader("SENHA!");
        NetworkInfo* net = getNetwork(listIndex);
        if (net) {
            char buf[64];
            snprintf(buf, 64, "Rede: %s", net->ssid);
            drawText(0, 14, buf, 1);
        }
        char buf[64];
        snprintf(buf, 64, "Pass: %s", capturedPassword);
        drawText(0, 28, buf, 1);
        drawCenteredText(50, "SEL: Nova", 1);
        updateDisplay();
    } else if (fakeAPEnabled) {
        clearDisplay();
        drawMenuHeader("AGUARDANDO...");
        NetworkInfo* net = getNetwork(listIndex);
        if (net) {
            char buf[64];
            snprintf(buf, 64, "Clone: %s", net->ssid);
            drawText(0, 14, buf, 1);
        }
        drawCenteredText(35, "Aguardando vitima", 1);
        drawCenteredText(48, "conectar...", 1);
        updateDisplay();
    } else {
        renderList("CAPTURAR SENHA", getNetworkCount(), drawNetworkItem);
    }
}

void drawRemoteItem(int index, int y, bool selected) {
    if (selected) {
        getDisplay().fillRect(0, y, 128, 10, 1);
        getDisplay().setTextColor(0);
    } else {
        getDisplay().setTextColor(1);
    }
    RemoteDevice* dev = getRemoteDevice(index);
    if (dev) {
        char buf[32];
        snprintf(buf, 32, "%s %s", dev->name, dev->ip);
        drawText(4, y + 1, buf, 1);
    }
    if (selected) getDisplay().setTextColor(1);
}

void renderRemote() {
    if (!inListView) {
        inListView = true;
        listIndex = 0;
        listMaxIndex = getRemoteDeviceCount() - 1;
        if (listMaxIndex < 0) listMaxIndex = 0;
        if (getRemoteDeviceCount() == 0) {
            showLoading("Buscando...", 0);
            scanRemoteDevices();
            listMaxIndex = getRemoteDeviceCount() - 1;
            if (listMaxIndex < 0) listMaxIndex = 0;
        }
    }
    if (getRemoteDeviceCount() == 0) {
        clearDisplay();
        drawMenuHeader("REMOTO");
        drawCenteredText(25, "Nenhum", 1);
        drawCenteredText(38, "dispositivo", 1);
        drawCenteredText(52, "SEL: Atualizar", 1);
        updateDisplay();
        return;
    }
    renderList("DISPOSITIVOS", getRemoteDeviceCount(), drawRemoteItem);
}

void renderDroneJammer() {
    clearDisplay();
    drawMenuHeader("DRONE JAMMER");
    if (droneJammerActive) {
        drawCenteredText(25, "JAMMING", 1);
        drawCenteredText(40, "ATIVO", 2);
        drawCenteredText(58, "SEL: Parar", 1);
    } else {
        drawCenteredText(25, "Pronto", 1);
        drawCenteredText(40, "JAMMING", 2);
        drawCenteredText(58, "SEL: Iniciar", 1);
    }
    updateDisplay();
}

void renderDroneRemote() {
    clearDisplay();
    drawMenuHeader("DRONE REMOTO");
    drawCenteredText(20, "Controle", 1);
    drawCenteredText(35, "UP/DOWN: Throttle", 1);
    drawCenteredText(48, "SEL: Arm", 1);
    drawCenteredText(58, "BACK: Sair", 1);
    updateDisplay();
}

void renderDroneLocate() {
    clearDisplay();
    drawMenuHeader("LOCALIZAR");
    startDroneLocate();
    DroneLocation* loc = getDroneLocation();
    if (loc) {
        char buf[32];
        snprintf(buf, 32, "Dist: %.1fm", loc->distance);
        drawCenteredText(25, buf, 1);
        snprintf(buf, 32, "RSSI: %d dBm", loc->rssi);
        drawCenteredText(38, buf, 1);
    }
    drawCenteredText(55, "Varrendo...", 1);
    updateDisplay();
}

void renderCameraFreeze() {
    clearDisplay();
    drawMenuHeader("CAMERA FREEZE");
    if (cameraFreezeActive) {
        drawCenteredText(25, "CONGELANDO", 1);
        drawCenteredText(40, "CAMERA", 2);
        drawCenteredText(58, "SEL: Parar", 1);
    } else {
        drawCenteredText(25, "Pronto", 1);
        drawCenteredText(40, "CONGELAR", 2);
        drawCenteredText(58, "SEL: Iniciar", 1);
    }
    updateDisplay();
}

void drawBTItem(int index, int y, bool selected) {
    if (selected) {
        getDisplay().fillRect(0, y, 128, 10, 1);
        getDisplay().setTextColor(0);
    } else {
        getDisplay().setTextColor(1);
    }
    BTDevice* dev = getBTDevice(index);
    if (dev) {
        char buf[32];
        snprintf(buf, 32, "%s [%d]", dev->name, dev->rssi);
        drawText(4, y + 1, buf, 1);
    }
    if (selected) getDisplay().setTextColor(1);
}

void renderBluetooth() {
    if (!inListView) {
        inListView = true;
        listIndex = 0;
        listMaxIndex = getBTDeviceCount() - 1;
        if (listMaxIndex < 0) listMaxIndex = 0;
        if (getBTDeviceCount() == 0) {
            showLoading("Scan BT...", 0);
            startBTScan();
            listMaxIndex = getBTDeviceCount() - 1;
            if (listMaxIndex < 0) listMaxIndex = 0;
        }
    }
    if (getBTDeviceCount() == 0) {
        clearDisplay();
        drawMenuHeader("BLUETOOTH");
        drawCenteredText(30, "Nenhum", 1);
        updateDisplay();
        return;
    }
    if (btJammerActive) {
        clearDisplay();
        drawMenuHeader("BT JAMMER");
        BTDevice* dev = getBTDevice(listIndex);
        if (dev) {
            char buf[64];
            snprintf(buf, 64, "Jamming:\n%s", dev->name);
            drawText(0, 14, buf, 1);
        }
        drawCenteredText(50, "SEL: Parar", 1);
        updateDisplay();
    } else {
        renderList("BLUETOOTH", getBTDeviceCount(), drawBTItem);
    }
}

void renderBFGate() {
    clearDisplay();
    drawMenuHeader("BF PORTAO");
    if (isBruteForceRunning()) {
        char buf[32];
        snprintf(buf, 32, "%d/%d", getCurrentBFIndex(), getTotalBFCount(0, 0));
        drawCenteredText(25, buf, 1);
        int pct = (getCurrentBFIndex() * 100) / getTotalBFCount(0, 0);
        drawProgressBar(14, 40, 100, 8, pct);
        drawCenteredText(55, "SEL: Parar", 1);
    } else {
        drawCenteredText(25, "Brute Force", 1);
        drawCenteredText(40, "PORTAO", 2);
        drawCenteredText(55, "SEL: Iniciar", 1);
    }
    updateDisplay();
}

void drawCarBrandItem(int index, int y, bool selected) {
    if (selected) {
        getDisplay().fillRect(0, y, 128, 10, 1);
        getDisplay().setTextColor(0);
    } else {
        getDisplay().setTextColor(1);
    }
    const char* name = getCarBrandName(index);
    drawText(4, y + 1, name, 1);
    if (selected) getDisplay().setTextColor(1);
}

void renderBFCar() {
    if (!inListView) {
        inListView = true;
        listIndex = 0;
        listMaxIndex = getCarBrandCount() - 1;
    }
    if (isBruteForceRunning()) {
        clearDisplay();
        drawMenuHeader("BF CARRO");
        char buf[32];
        snprintf(buf, 32, "%d/%d", getCurrentBFIndex(), getTotalBFCount(1, listIndex));
        drawCenteredText(25, buf, 1);
        int pct = (getCurrentBFIndex() * 100) / getTotalBFCount(1, listIndex);
        drawProgressBar(14, 40, 100, 8, pct);
        drawCenteredText(55, "SEL: Parar", 1);
        updateDisplay();
    } else {
        renderList("ESCOLHA MARCA", getCarBrandCount(), drawCarBrandItem);
    }
}

void renderSettingsPins() {
    clearDisplay();
    drawMenuHeader("TESTE PINOS");
    testAllPins();
    int y = 14;
    for (int i = 0; i < (int)getPinTestCount() && i < 4; i++) {
        PinTest* pt = getPinTest(i);
        if (pt) {
            char buf[32];
            snprintf(buf, 32, "%s: %s", pt->name, pt->working ? "OK" : "FALHA");
            drawText(0, y + i * 10, buf, 1);
        }
    }
    drawCenteredText(55, "BACK: Voltar", 1);
    updateDisplay();
}

void renderSettingsModules() {
    clearDisplay();
    drawMenuHeader("TESTE MODULOS");
    testModules(nrf24IsAvailable(), cc1101IsAvailable(), false);
    int y = 14;
    for (int i = 0; i < (int)getModuleCount() && i < 5; i++) {
        ModuleStatus* mod = getModule(i);
        if (mod) {
            char buf[32];
            snprintf(buf, 32, "%s: %s", mod->name, mod->working ? "OK" : "FALHA");
            drawText(0, y + i * 9, buf, 1);
        }
    }
    drawCenteredText(55, "BACK: Voltar", 1);
    updateDisplay();
}

void renderSettingsBrightness() {
    clearDisplay();
    drawMenuHeader("BRILHO");
    char buf[32];
    snprintf(buf, 32, "Brilho: %d%%", (screenBrightness * 100) / 255);
    drawCenteredText(25, buf, 1);
    drawProgressBar(14, 40, 100, 10, (screenBrightness * 100) / 255);
    drawCenteredText(55, "UP/DOWN: Ajustar", 1);
    updateDisplay();
}

void renderSettingsConnection() {
    clearDisplay();
    drawMenuHeader("CONEXAO");
    char buf[32];
    snprintf(buf, 32, "Tipo: %s", getConnectionTypeName());
    drawText(0, 14, buf, 1);
    snprintf(buf, 32, "Status: %s", isConnectionActive() ? "Ativo" : "Inativo");
    drawText(0, 26, buf, 1);
    if (isConnectionActive()) {
        snprintf(buf, 32, "Codigo: %s", getPairingCode());
        drawText(0, 38, buf, 1);
    }
    drawCenteredText(55, "SEL: Alternar", 1);
    updateDisplay();
}

// ============================================================
// INPUT HANDLERS
// ============================================================
void handleNRF24Jammer(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (!nrf24IsJammerActive()) {
            nrf24StartJammer();
        } else {
            nrf24StopJammer();
        }
    }
}

void handleNRF24Scanner(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT && !scanning) {
        scanning = true;
        captureStartTime = millis();
        nrf24Scan();
        scanning = false;
    }
}

void handleCC1101Copy(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT && !capturing) {
        capturing = true;
        captureStartTime = millis();
        cc1101StartCapture();
        capturing = false;
    }
}

void handleCC1101Replay(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT && listIndex < (int)cc1101GetSavedCount()) {
        cc1101ReplaySignal(listIndex);
        showMessage("REPRODUZIR", "Sinal enviado!");
        delay(800);
    }
    if (btn == BTN_PRESSED_UP && listIndex > 0) listIndex--;
    if (btn == BTN_PRESSED_DOWN && listIndex < listMaxIndex) listIndex++;
}

void handleDeauth(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (!deauthActive && listIndex < (int)getNetworkCount()) startDeauth(listIndex);
        else stopDeauth();
    }
    if (btn == BTN_PRESSED_UP && listIndex > 0) listIndex--;
    if (btn == BTN_PRESSED_DOWN && listIndex < listMaxIndex) listIndex++;
}

void handlePassword(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (!fakeAPEnabled && !passwordCaptured && listIndex < (int)getNetworkCount()) {
            startEvilTwin(listIndex);
        } else if (passwordCaptured) {
            passwordCaptured = false;
            stopFakeAP();
        } else if (fakeAPEnabled) {
            stopFakeAP();
        }
    }
    if (btn == BTN_PRESSED_UP && listIndex > 0) listIndex--;
    if (btn == BTN_PRESSED_DOWN && listIndex < listMaxIndex) listIndex++;
}

void handleRemote(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (getRemoteDeviceCount() == 0) {
            showLoading("Buscando...", 0);
            scanRemoteDevices();
            listMaxIndex = getRemoteDeviceCount() - 1;
            if (listMaxIndex < 0) listMaxIndex = 0;
        } else if (listIndex < (int)getRemoteDeviceCount()) {
            RemoteDevice* dev = getRemoteDevice(listIndex);
            if (dev) {
                showMessage("REMOTO", dev->name);
                delay(800);
            }
        }
    }
    if (btn == BTN_PRESSED_UP && listIndex > 0) listIndex--;
    if (btn == BTN_PRESSED_DOWN && listIndex < listMaxIndex) listIndex++;
}

void handleDroneJammer(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (!droneJammerActive) startDroneJammer();
        else stopDroneJammer();
    }
}

void handleDroneRemote(ButtonState btn) {
    // Throttle control
}

void handleDroneLocate(ButtonState btn) {
    // Auto-updates
}

void handleCameraFreeze(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (!cameraFreezeActive) startCameraFreeze();
        else stopCameraFreeze();
    }
}

void handleBluetooth(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (!btJammerActive && listIndex < (int)getBTDeviceCount()) startBTJammer(listIndex);
        else stopBTJammer();
    }
    if (btn == BTN_PRESSED_UP && listIndex > 0) listIndex--;
    if (btn == BTN_PRESSED_DOWN && listIndex < listMaxIndex) listIndex++;
}

void handleBFGate(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (!isBruteForceRunning()) startGateBruteForce();
        else stopBruteForce();
    }
}

void handleBFCar(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (!isBruteForceRunning()) startCarBruteForce(listIndex);
        else stopBruteForce();
    }
    if (btn == BTN_PRESSED_UP && listIndex > 0) listIndex--;
    if (btn == BTN_PRESSED_DOWN && listIndex < listMaxIndex) listIndex++;
}

void handleSettingsPins(ButtonState btn) {
    // Auto-updates
}

void handleSettingsModules(ButtonState btn) {
    // Auto-updates
}

void handleSettingsBrightness(ButtonState btn) {
    if (btn == BTN_PRESSED_UP) {
        if (screenBrightness < 255) screenBrightness += 25;
        if (screenBrightness > 255) screenBrightness = 255;
        setBrightness(screenBrightness);
    }
    if (btn == BTN_PRESSED_DOWN) {
        if (screenBrightness > 0) screenBrightness -= 25;
        setBrightness(screenBrightness);
    }
}

void handleSettingsConnection(ButtonState btn) {
    if (btn == BTN_PRESSED_SELECT) {
        if (isConnectionActive()) disconnectConnection();
        else {
            static int connType = 0;
            connType = (connType + 1) % 4;
            initConnection(connType);
        }
    }
}

// ============================================================
// MENU LOOP
// ============================================================
void menuInit() {
    showSplashScreen();
    enterMenu(MENU_MAIN);
}

void menuLoop() {
    ButtonState btn = readButtons();

    switch (currentMenu) {
        case MENU_MAIN: case MENU_NRF24: case MENU_CC1101: case MENU_ATTACKS:
        case MENU_ATTACK_DRONE: case MENU_ATTACK_CAMERA: case MENU_ATTACK_BRUTEFORCE:
        case MENU_NETWORKS: case MENU_SETTINGS:
            renderMenu(); break;
        case MENU_NRF24_JAMMER: renderNRF24Jammer(); break;
        case MENU_NRF24_SCANNER: renderNRF24Scanner(); break;
        case MENU_CC1101_COPY: renderCC1101Copy(); break;
        case MENU_CC1101_REPLAY: renderCC1101Replay(); break;
        case MENU_ATTACK_DRONE_JAMMER: renderDroneJammer(); break;
        case MENU_ATTACK_DRONE_REMOTE: renderDroneRemote(); break;
        case MENU_ATTACK_DRONE_LOCATE: renderDroneLocate(); break;
        case MENU_ATTACK_DEAUTH: case MENU_NET_DEAUTH: renderDeauth(); break;
        case MENU_ATTACK_CAMERA_FREEZE: renderCameraFreeze(); break;
        case MENU_ATTACK_BLUETOOTH: renderBluetooth(); break;
        case MENU_ATTACK_BF_GATE: renderBFGate(); break;
        case MENU_ATTACK_BF_CAR: renderBFCar(); break;
        case MENU_NET_PASSWORD: renderPassword(); break;
        case MENU_NET_REMOTE: renderRemote(); break;
        case MENU_SETTINGS_PINS: renderSettingsPins(); break;
        case MENU_SETTINGS_MODULES: renderSettingsModules(); break;
        case MENU_SETTINGS_BRIGHTNESS: renderSettingsBrightness(); break;
        case MENU_SETTINGS_CONNECTION: renderSettingsConnection(); break;
    }

    switch (currentMenu) {
        case MENU_MAIN: case MENU_NRF24: case MENU_CC1101: case MENU_ATTACKS:
        case MENU_ATTACK_DRONE: case MENU_ATTACK_CAMERA: case MENU_ATTACK_BRUTEFORCE:
        case MENU_NETWORKS: case MENU_SETTINGS:
            if (btn == BTN_PRESSED_UP && menuIndex > 0) menuIndex--;
            if (btn == BTN_PRESSED_DOWN && menuIndex < menuMaxIndex) menuIndex++;
            if (btn == BTN_PRESSED_SELECT) enterMenu(currentMenuItems[menuIndex].state);
            if (btn == BTN_PRESSED_BACK) goBack();
            break;
        case MENU_NRF24_JAMMER: handleNRF24Jammer(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_NRF24_SCANNER: handleNRF24Scanner(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_CC1101_COPY: handleCC1101Copy(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_CC1101_REPLAY: handleCC1101Replay(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_ATTACK_DRONE_JAMMER: handleDroneJammer(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_ATTACK_DRONE_REMOTE: handleDroneRemote(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_ATTACK_DRONE_LOCATE: handleDroneLocate(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_ATTACK_DEAUTH: case MENU_NET_DEAUTH: handleDeauth(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_ATTACK_CAMERA_FREEZE: handleCameraFreeze(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_ATTACK_BLUETOOTH: handleBluetooth(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_ATTACK_BF_GATE: handleBFGate(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_ATTACK_BF_CAR: handleBFCar(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_NET_PASSWORD: handlePassword(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_NET_REMOTE: handleRemote(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_SETTINGS_PINS: handleSettingsPins(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_SETTINGS_MODULES: handleSettingsModules(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_SETTINGS_BRIGHTNESS: handleSettingsBrightness(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
        case MENU_SETTINGS_CONNECTION: handleSettingsConnection(btn); if (btn == BTN_PRESSED_BACK) goBack(); break;
    }

    delay(50);
}
