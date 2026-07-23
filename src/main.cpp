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
    delay(300);
    Serial.println("\n[BOOT] Crazy Cat v3.1 starting...");

    // ============================================================
    // WiFi AP (modo seguro no boot — evita conflitos RF no startup)
    // O modo AP_STA sera ativado apenas durante o scan, em runtime.
    // ============================================================
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );
    delay(100);
    bool apOk = WiFi.softAP("CrazyCat", "crazycat123", 6, 0, 4);
    delay(200);
    Serial.printf("[BOOT] AP CrazyCat: %s\n", apOk ? "OK" : "FAIL");
    Serial.printf("[BOOT] IP: %s\n", WiFi.softAPIP().toString().c_str());
    yield();

    startAPIServer();
    Serial.println("[BOOT] HTTP Server started");
    yield();

    // ============================================================
    // Display
    // ============================================================
    if (!displayInit()) {
        Serial.println("[BOOT] WARNING: OLED init failed!");
    } else {
        Serial.println("[BOOT] OLED OK");
    }
    yield();

    showLoading("Iniciando...", 10);
    yield();

    inputInit();
    showLoading("Botoes OK", 25);
    yield();

    nrf24OK = nrf24Init();
    showLoading(nrf24OK ? "NRF24 OK" : "NRF24 FAIL", 40);
    yield();

    cc1101OK = cc1101Init();
    showLoading(cc1101OK ? "CC1101 OK" : "CC1101 FAIL", 60);
    yield();

    showLoading("WiFi AP OK", 80);
    showLoading("Pronto!", 100);
    delay(500);
    yield();

    menuInit();
    Serial.println("[BOOT] Ready!\n");
}

void loop() {
    menuLoop();
    yield();
}
