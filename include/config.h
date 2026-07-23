#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================
// PINOS - Crazy Cat v3.1 (ORIGINAIS - NAO ALTERAR)
// ============================================================

// TELA OLED (I2C)
#define OLED_SCK    22
#define OLED_SDA    21

// BOTOES
#define BTN_UP      17  // Cinza
#define BTN_DOWN    15  // Vermelho
#define BTN_SELECT  32  // Marrom
#define BTN_BACK    33  // Preto

// NRF24L01
#define NRF_CE      26  // Amarelo
#define NRF_CSN     25  // Azul
#define NRF_SCK     18  // Verde
#define NRF_MOSI    23  // Roxo
#define NRF_MISO    19  // Cinza

// CC1101 - PINOS ORIGINAIS
#define CC1101_GDO0  4  // Azul
#define CC1101_CSN  27  // Roxo  <- MANTIDO NO 27
#define CC1101_SCK  14  // Amarelo
#define CC1101_MOSI 13  // Verde
#define CC1101_MISO 12  // Vermelho
#define CC1101_GDO2 16  // Laranja

// ============================================================
// CONFIGURACOES
// ============================================================
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT   64
#define OLED_ADDRESS    0x3C

#define MAX_SAVED_SIGNALS   5
#define CAPTURE_DURATION    10000
#define MAX_NETWORKS        20
#define MAX_BT_DEVICES      15

// ============================================================
// ESTRUTURAS
// ============================================================
struct SignalData {
    uint8_t data[64];
    uint8_t length;
    uint32_t frequency;
    uint8_t modulation;
    char name[16];
    bool valid;
};

struct NetworkInfo {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    bool encrypted;
};

struct BTDevice {
    char name[32];
    uint8_t address[6];
    int8_t rssi;
};

struct RemoteDevice {
    char ip[16];
    char name[32];
    uint8_t mac[6];
    uint16_t port;
};

// ============================================================
// ESTADOS
// ============================================================
enum MenuState {
    MENU_MAIN,
    MENU_NRF24, MENU_NRF24_JAMMER, MENU_NRF24_SCANNER, MENU_NRF24_ANALYZE, MENU_NRF24_ANALYZE_DETAIL,
    MENU_CC1101, MENU_CC1101_COPY, MENU_CC1101_REPLAY,
    MENU_ATTACKS,
    MENU_ATTACK_DRONE, MENU_ATTACK_DRONE_JAMMER, MENU_ATTACK_DRONE_REMOTE, MENU_ATTACK_DRONE_LOCATE,
    MENU_ATTACK_DEAUTH,
    MENU_ATTACK_CAMERA, MENU_ATTACK_CAMERA_FREEZE,
    MENU_ATTACK_BLUETOOTH,
    MENU_ATTACK_BRUTEFORCE, MENU_ATTACK_BF_GATE, MENU_ATTACK_BF_CAR,
    MENU_NETWORKS, MENU_NET_PASSWORD, MENU_NET_DEAUTH, MENU_NET_REMOTE,
    MENU_SETTINGS, MENU_SETTINGS_PINS, MENU_SETTINGS_MODULES, MENU_SETTINGS_BRIGHTNESS, MENU_SETTINGS_CONNECTION
};

enum ButtonState { BTN_NONE, BTN_PRESSED_UP, BTN_PRESSED_DOWN, BTN_PRESSED_SELECT, BTN_PRESSED_BACK };

// ============================================================
// VARIAVEIS EXTERNAS (definidas em globals.cpp)
// ============================================================
extern SignalData savedSignals[MAX_SAVED_SIGNALS];
extern NetworkInfo scannedNetworks[MAX_NETWORKS];
extern BTDevice btDevices[MAX_BT_DEVICES];
extern RemoteDevice remoteDevices[10];

extern uint8_t savedSignalCount;
extern uint8_t networkCount;
extern uint8_t btDeviceCount;
extern uint8_t remoteDeviceCount;

struct MenuItem {
    const char* label;
    MenuState state;
};

extern MenuState currentMenu;
extern MenuState previousMenu;
extern int8_t menuIndex;
extern int8_t menuMaxIndex;
extern bool menuRunning;
extern uint8_t screenBrightness;

extern bool nrf24JammerActive;
extern bool cc1101CopyActive;
extern bool deauthActive;
extern bool droneJammerActive;
extern bool cameraFreezeActive;
extern bool btJammerActive;
extern bool bfRunning;

extern char capturedPassword[64];
extern bool passwordCaptured;
extern bool fakeAPEnabled;
extern bool evilTwinActive;

// NRF24 Scanner & Jammer
#define NRF_SCAN_HISTORY    64
#define NRF_SCAN_BARS       16
#define NRF_MAX_DETECTED    10


// Handshake Capture
extern void startHandshakeCapture();
extern void stopHandshakeCapture();
extern bool isHandshakeCapturing();
extern const char* getHandshakeStatus();
extern uint8_t getHandshakeMessageCount();
extern bool isHandshakeComplete();
extern bool saveHandshakeToFile(const char* filename);
extern bool sendHandshakeViaBluetooth(const char* filename);
extern size_t getHandshakeFileSize(const char* filename);
extern void serveHandshakeHTTP();


// API REST
extern void startAPIServer();
extern void stopAPIServer();
extern void apiLoop();
extern bool isAPIServerRunning();

#endif
