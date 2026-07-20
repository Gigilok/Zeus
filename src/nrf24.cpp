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

// Scanner - historico de RSSI por canal para ondas
static int8_t scanHistory[NRF_SCAN_HISTORY];
static int scanIndex = 0;
static bool scanning = false;
static unsigned long scanLastUpdate = 0;

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

// ============================================================
// SCANNER - ONDAS EM TEMPO REAL
// ============================================================
void nrf24StartScan() {
    scanning = true;
    scanIndex = 0;
    scanLastUpdate = 0;
    for (int i = 0; i < NRF_SCAN_HISTORY; i++) scanHistory[i] = -100;
    radio.stopListening();
    radio.setAutoAck(false);
    radio.setRetries(0, 0);
    radio.setCRCLength(RF24_CRC_DISABLED);
}

void nrf24StopScan() {
    scanning = false;
}

bool nrf24IsScanning() { return scanning; }

// Retorna ponteiro para o historico de 64 amostras
const int8_t* nrf24GetScanHistory() { return scanHistory; }

int nrf24GetScanIndex() { return scanIndex; }

void nrf24ScanLoop() {
    if (!scanning) return;

    // Atualiza a cada 15ms = ~66 amostras/segundo
    if (millis() - scanLastUpdate >= 15) {
        scanLastUpdate = millis();

        // Varre canal atual e mede RSSI
        radio.setChannel(scanIndex % 125);
        delayMicroseconds(200);

        bool carrier = radio.testCarrier();
        bool rpd = radio.testRPD();
        int8_t rssi = -100;
        if (carrier || rpd) {
            rssi = -50 - random(15);  // Simula RSSI quando detecta algo
        }

        scanHistory[scanIndex] = rssi;
        scanIndex++;
        if (scanIndex >= NRF_SCAN_HISTORY) scanIndex = 0;
    }
}

// ============================================================
// JAMMER
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
    if (micros() - jamLastSwitch >= 500) {
        jamLastSwitch = micros();
        jamChannel++;
        if (jamChannel > 125) jamChannel = 0;
        radio.setChannel(jamChannel);
    }
    return jamChannel;
}

uint8_t nrf24GetDeviceCount() { return nrfDeviceCount; }
NRFDevice* nrf24GetDevice(uint8_t index) {
    if (index < nrfDeviceCount) return &nrfDevices[index];
    return nullptr;
}
bool nrf24IsJammerActive() { return nrf24JammerActive; }
bool nrf24IsAvailable() { return radio.isChipConnected(); }
