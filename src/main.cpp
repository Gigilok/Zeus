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

bool nrf24OK = false;
bool cc1101OK = false;

void setup() {
    Serial.begin(115200);
    delay(1000); // Delay para estabilizar energia

    // Evita travamentos por escrita na flash e reduz consumo
    WiFi.persistent(false);
    WiFi.setTxPower(WIFI_POWER_18_5dBm); // Reduzido para evitar Brownout no USB do celular

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

    // AP CrazyCat para controle remoto
    WiFi.mode(WIFI_AP_STA);
    delay(200);
    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );
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
    // 1. Processa as requisições HTTP do Termux
    apiLoop();

    // 2. Processa o ataque Deauth em background (se ativo)
    if (deauthActive) {
        deauthLoop();
    }

    // 3. Atualiza display, lê botões e navega no menu
    menuLoop();
}
