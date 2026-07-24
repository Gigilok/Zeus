#include "config.h"
#include <WiFi.h>

extern bool displayInit();
extern bool inputInit();
extern bool nrf24Init();
extern bool cc1101Init();
extern void menuInit();
extern void menuLoop();
extern void showLoading(const char* text, int percent);
extern void startAPIServer();
extern void apiLoop();

extern bool deauthActive;
extern bool deauthLoop();

// Novos loops não-bloqueantes
extern void bfLoop();
extern void cc1101CaptureLoop();
extern void btJammerLoop();
extern bool cc1101CopyActive;
extern bool btJammerActive;
extern bool bfRunning;

bool nrf24OK = false;
bool cc1101OK = false;

void setup() {
    Serial.begin(115200);
    delay(2000); 

    WiFi.persistent(false);

    if (!displayInit()) {
        Serial.println("OLED init failed!");
    }

    showLoading("Iniciando...", 10);
    inputInit();
    showLoading("Botoes OK", 25);

    nrf24OK = nrf24Init();
    showLoading(nrf24OK ? "NRF24 OK" : "NRF24 FAIL", 40);

    cc1101OK = cc1101Init();
    showLoading(cc1101OK ? "CC1101 OK" : "CC1101 FAIL", 60);

    WiFi.mode(WIFI_AP_STA);
    delay(200);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    delay(100);
    WiFi.softAP("CrazyCat", "crazycat123", 6, 0, 4);
    delay(200);

    startAPIServer();
    Serial.println("[WiFi] AP CrazyCat em 192.168.4.1:8080");

    showLoading("Pronto!", 100);
    delay(500);

    menuInit();
}

void loop() {
    // 1. Processa requisições HTTP do Termux (CRÍTICO)
    apiLoop();

    // 2. Loops de ferramentas em background (Não bloqueantes)
    if (deauthActive) deauthLoop();
    if (bfRunning) bfLoop();
    if (cc1101CopyActive) cc1101CaptureLoop();
    if (btJammerActive) btJammerLoop();

    // 3. Atualiza display e lê botões
    menuLoop();
}
