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

// Importa as funções e variáveis do wifi_attacks.cpp
extern bool deauthActive;
extern bool deauthLoop();
extern bool droneJammerActive;
extern bool cameraFreezeActive;

// Importa o loop do NRF24 (verifique se o nome da função no seu nrf24.cpp é esse)
extern bool scannerRunning;
extern void nrf24Loop(); 

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

    // AP CrazyCat para controle remoto
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );
    delay(50);
    WiFi.softAP("CrazyCat", "crazycat123", 6, 0, 4);
    delay(100);

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

    // 3. Processa o Scanner e Jammer do NRF24 (CRÍTICO PARA PEGAR PACOTES)
    if (scannerRunning) {
        nrf24Loop(); // Se a sua função se chamar diferente, ajuste aqui
    }

    // 4. Atualiza display, lê botões e navega no menu
    menuLoop();
}
