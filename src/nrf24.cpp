#include <SPI.h>
#include <RF24.h>
#include "config.h"

// ============================================================
// NRF24 - Crazy Cat v3.1
// Pinos: CE=26, CSN=25, SCK=18, MISO=19, MOSI=23
// Compartilha SPI com CC1101
// ============================================================

RF24 radio(NRF_CE, NRF_CSN);

struct NRFDevice {
    uint8_t address[5];
    uint8_t channel;
    int8_t rssi;
};

NRFDevice nrfDevices[20];
uint8_t nrfDeviceCount = 0;

// Estado do jammer (NAO-bloqueante)
static int jamChannel = 0;
static unsigned long jamLastSwitch = 0;
static bool carrierActive = false;

// ============================================================
// INIT
// ============================================================
bool nrf24Init() {
    if (!radio.begin()) {
        return false;
    }
    radio.setPALevel(RF24_PA_MAX);
    radio.setDataRate(RF24_2MBPS);
    radio.setAutoAck(false);
    radio.disableCRC();
    radio.stopListening();
    return true;
}

// ============================================================
// SCANNER
// ============================================================
void nrf24Scan() {
    nrfDeviceCount = 0;
    uint8_t buffer[32];

    for (int ch = 0; ch < 125 && nrfDeviceCount < 20; ch++) {
        radio.setChannel(ch);
        radio.startListening();
        delay(40);

        if (radio.available()) {
            uint8_t len = radio.getDynamicPayloadSize();
            if (len > 0 && len <= 32) {
                radio.read(buffer, len);
                nrfDevices[nrfDeviceCount].channel = ch;
                nrfDevices[nrfDeviceCount].rssi = -40;
                memcpy(nrfDevices[nrfDeviceCount].address, buffer, 5);
                nrfDeviceCount++;
            }
        }
        radio.stopListening();
    }
}

uint8_t nrf24GetDeviceCount() { return nrfDeviceCount; }

NRFDevice* nrf24GetDevice(uint8_t index) {
    if (index < nrfDeviceCount) return &nrfDevices[index];
    return nullptr;
}

bool nrf24IsJammerActive() { return nrf24JammerActive; }
bool nrf24IsAvailable() { return radio.isChipConnected(); }

// ============================================================
// CARRIER WAVE JAMMER - NAO BLOQUEANTE
// ============================================================
void nrf24StartJammer() {
    if (nrf24JammerActive) return;
    nrf24JammerActive = true;
    jamChannel = 0;
    jamLastSwitch = 0;
    carrierActive = false;

    radio.setAutoAck(false);
    radio.stopListening();
    radio.setRetries(0, 0);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED);
}

void nrf24StopJammer() {
    if (!nrf24JammerActive) return;
    if (carrierActive) {
        radio.stopConstCarrier();
        carrierActive = false;
    }
    radio.stopListening();
    radio.flush_tx();
    nrf24JammerActive = false;
}

// Chamado a cada loop() quando jammer ativo
// Retorna canal atual para display
int nrf24JammerLoop() {
    if (!nrf24JammerActive) return -1;

    // Troca de canal a cada 80ms
    if (millis() - jamLastSwitch >= 80) {
        jamLastSwitch = millis();

        if (carrierActive) {
            radio.stopConstCarrier();
        }

        jamChannel += 2;
        if (jamChannel > 125) jamChannel = 0;

        radio.setChannel(jamChannel);
        radio.startConstCarrier(RF24_PA_MAX, jamChannel);
        carrierActive = true;
    }

    return jamChannel;
}
