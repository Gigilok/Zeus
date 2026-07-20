#include <SPI.h>
#include <RF24.h>
#include "config.h"

// NRF24 usa HSPI compartilhado - NAO chama SPI.begin() aqui!
RF24* nrf24 = nullptr;

struct NRFDevice {
    uint8_t address[5];
    uint8_t channel;
    int8_t rssi;
};

NRFDevice nrfDevices[20];
uint8_t nrfDeviceCount = 0;

bool nrf24Init() {
    // NRF24 usa HSPI (pinos 18,19,23) - SPI ja inicializado pelo CC1101
    nrf24 = new RF24(NRF_CE, NRF_CSN);

    if (!nrf24->begin()) {
        delete nrf24;
        nrf24 = nullptr;
        return false;
    }

    nrf24->setPALevel(RF24_PA_MAX);
    nrf24->setDataRate(RF24_2MBPS);
    nrf24->setAutoAck(false);
    nrf24->disableCRC();
    nrf24->enableDynamicPayloads();

    return true;
}

void nrf24StartJammer() {
    if (!nrf24) return;
    nrf24JammerActive = true;
    nrf24->stopListening();
    uint8_t noise[32];
    for (int i = 0; i < 32; i++) noise[i] = random(256);

    while (nrf24JammerActive) {
        for (int ch = 0; ch < 125; ch++) {
            nrf24->setChannel(ch);
            nrf24->write(noise, 32);
            for (int i = 0; i < 32; i++) noise[i] = random(256);
        }
        yield();
    }
}

void nrf24StopJammer() {
    nrf24JammerActive = false;
}

void nrf24Scan() {
    if (!nrf24) return;
    nrfDeviceCount = 0;
    uint8_t buffer[32];

    for (int ch = 0; ch < 125 && nrfDeviceCount < 20; ch++) {
        nrf24->setChannel(ch);
        nrf24->startListening();
        delay(50);

        if (nrf24->available()) {
            uint8_t len = nrf24->getDynamicPayloadSize();
            if (len > 0 && len <= 32) {
                nrf24->read(buffer, len);
                nrfDevices[nrfDeviceCount].channel = ch;
                nrfDevices[nrfDeviceCount].rssi = -40;
                memcpy(nrfDevices[nrfDeviceCount].address, buffer, 5);
                nrfDeviceCount++;
            }
        }
        nrf24->stopListening();
    }
}

uint8_t nrf24GetDeviceCount() {
    return nrfDeviceCount;
}

NRFDevice* nrf24GetDevice(uint8_t index) {
    if (index < nrfDeviceCount) return &nrfDevices[index];
    return nullptr;
}

bool nrf24IsJammerActive() {
    return nrf24JammerActive;
}

bool nrf24IsAvailable() {
    if (!nrf24) return false;
    return nrf24->isChipConnected();
}
