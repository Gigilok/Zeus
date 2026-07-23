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
    Serial.println("\n================================");
    Serial.println("[BOOT] Crazy Cat v3.1 starting...");
    Serial.println("================================");

    if (!displayInit()) {
        Serial.println("[BOOT] WARNING: OLED init failed!");
    } else {
        Serial.println("[BOOT] OLED OK");
    }
    yield();

    showLoading("Iniciando...", 10);

    inputInit();
    Serial.println("[BOOT] Input OK");
    showLoading("Botoes OK", 25);

    nrf24OK = nrf24Init();
    Serial.printf("[BOOT] NRF24: %s\n", nrf24OK ? "OK" : "FAIL");
    showLoading(nrf24OK ? "NRF24 OK" : "NRF24 FAIL", 40);
    yield();

    cc1101OK = cc1101Init();
    Serial.printf("[BOOT] CC1101: %s\n", cc1101OK ? "OK" : "FAIL");
    showLoading(cc1101OK ? "CC1101 OK" : "CC1101 FAIL", 60);
    yield();

    // ============================================================
    // WiFi AP Setup
    // ============================================================
    Serial.println("[BOOT] Configuring WiFi AP...");

    WiFi.mode(WIFI_OFF);
    delay(100);

    WiFi.mode(WIFI_AP);
    delay(100);
    Serial.println("[BOOT] WiFi mode set to AP");

    bool configOk = WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),
        IPAddress(192, 168, 4, 1),
        IPAddress(255, 255, 255, 0)
    );
    Serial.printf("[BOOT] softAPConfig: %s\n", configOk ? "OK" : "FAIL");
    delay(100);

    bool apOk = WiFi.softAP("CrazyCat", "crazycat123", 6, 0, 4);
    Serial.printf("[BOOT] softAP('CrazyCat'): %s\n", apOk ? "OK" : "FAIL");

    if (apOk) {
        Serial.printf("[BOOT] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
        Serial.printf("[BOOT] AP MAC: %s\n", WiFi.softAPmacAddress().c_str());
        Serial.printf("[BOOT] Station count: %d\n", WiFi.softAPgetStationNum());
    } else {
        Serial.println("[BOOT] ERROR: Failed to start AP!");
    }

    delay(300);

    startAPIServer();
    Serial.println("[BOOT] HTTP API Server started on :8080");

    showLoading("WiFi AP OK", 80);
    showLoading("Pronto!", 100);
    delay(500);

    menuInit();
    Serial.println("[BOOT] Setup complete. Entering loop.");
    Serial.println("================================\n");
}

void loop() {
    menuLoop();
    yield();
}
