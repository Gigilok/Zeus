#include <SPI.h>
#include <RF24.h>
#include "config.h"

RF24 radio(NRF_CE, NRF_CSN);

struct NRFDevice {
    uint8_t address[5];
    uint8_t channel;
    int8_t rssi;
};

NRFDevice nrfDevices[20];
uint8_t nrfDeviceCount = 0;

static int jamChannel = 0;
static unsigned long jamLastSwitch = 0;

static void hardResetNRF24() {
    digitalWrite(NRF_CE, LOW);
    delay(150);
    digitalWrite(NRF_CE, HIGH);
    delay(150);
    digitalWrite(NRF_CSN, HIGH);
    delay(10);
    digitalWrite(NRF_CSN, LOW);
    delay(10);
    digitalWrite(NRF_CSN, HIGH);
    delay(50);
}

bool nrf24Init() {
    pinMode(NRF_CE, OUTPUT);
    pinMode(NRF_CSN, OUTPUT);
    digitalWrite(NRF_CSN, HIGH);
    hardResetNRF24();
    if (!radio.begin()) return false;
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setAutoAck(false);
    radio.disableCRC();
    radio.stopListening();
    return true;
}

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
// CARRIER WAVE JAMMER - AGRESSIVO
// NAO desliga carrier entre canais, so muda canal rapidamente
// ============================================================
void nrf24StartJammer() {
    if (nrf24JammerActive) return;
    nrf24JammerActive = true;
    jamChannel = 0;
    jamLastSwitch = 0;

    radio.setAutoAck(false);
    radio.stopListening();
    radio.setRetries(0, 0);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED);

    // Inicia carrier no canal 0
    radio.setChannel(0);
    radio.startConstCarrier(RF24_PA_MAX, 0);
}

void nrf24StopJammer() {
    if (!nrf24JammerActive) return;
    radio.stopConstCarrier();
    radio.stopListening();
    radio.flush_tx();
    nrf24JammerActive = false;
}

int nrf24JammerLoop() {
    if (!nrf24JammerActive) return -1;

    // Muda canal a cada 500us (igual ao codigo funcional)
    // Mas sem parar o carrier! So muda o canal.
    if (micros() - jamLastSwitch >= 500) {
        jamLastSwitch = micros();
        jamChannel++;
        if (jamChannel > 125) jamChannel = 0;
        radio.setChannel(jamChannel);
    }

    return jamChannel;
}
