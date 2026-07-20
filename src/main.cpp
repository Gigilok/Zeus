#include "config.h"
#include <WiFi.h>

extern bool displayInit();
extern bool inputInit();
extern bool nrf24Init();
extern bool cc1101Init();
extern void menuInit();
extern void menuLoop();
extern void showLoading(const char* text, int percent);

bool nrf24OK = false;
bool cc1101OK = false;

void setup() {
    Serial.begin(115200);

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

    WiFi.mode(WIFI_STA);
    showLoading("WiFi OK", 80);

    showLoading("Pronto!", 100);
    delay(500);

    menuInit();
}

void loop() {
    menuLoop();
}
