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

bool nrf24OK = false;
bool cc1101OK = false;

void setup() {
    Serial.begin(115200);
    delay(100);  // Estabiliza Serial

    if (!displayInit()) {
        Serial.println("OLED init failed!");
    }
    yield();  // Evita watchdog

    showLoading("Iniciando...", 10);

    inputInit();
    showLoading("Botoes OK", 25);

    nrf24OK = nrf24Init();
    showLoading(nrf24OK ? "NRF24 OK" : "NRF24 FAIL", 40);
    yield();

    cc1101OK = cc1101Init();
    showLoading(cc1101OK ? "CC1101 OK" : "CC1101 FAIL", 60);
    yield();

    // ============================================================
    // WiFi AP + HTTP API Server
    // ORDEM CORRETA: mode -> config -> softAP -> delay -> server
    // ============================================================
    WiFi.mode(WIFI_AP);
    delay(50);

    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );
    delay(50);

    WiFi.softAP("CrazyCat", "crazycat123", 6, 0, 4);
    delay(200);  // Aguarda AP estabilizar

    startAPIServer();
    delay(100);

    showLoading("WiFi AP OK", 80);
    Serial.println("[WiFi] AP CrazyCat iniciado em 192.168.4.1:8080");

    showLoading("Pronto!", 100);
    delay(500);

    menuInit();
}

void loop() {
    menuLoop();
}
