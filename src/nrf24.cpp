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

// Scanner
static int8_t scanHistory[NRF_SCAN_HISTORY];
static int scanIndex = 0;
static bool scanning = false;
static unsigned long scanLastUpdate = 0;

// Sinais detectados
struct DetectedSignal {
    uint8_t channel;
    int8_t rssi;
    unsigned long firstSeen;
    unsigned long lastSeen;
    bool active;
};
static DetectedSignal detectedSignals[NRF_MAX_DETECTED];
static uint8_t detectedCount = 0;

// Jammer
static int jamChannel = 0;
static unsigned long jamLastSwitch = 0;
static bool jamSelectMode = false;  // false = kill all, true = select channel
static int jamSelectedChannel = 0;

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
// SCANNER
// ============================================================
void nrf24StartScan() {
    scanning = true;
    scanIndex = 0;
    scanLastUpdate = 0;
    detectedCount = 0;
    for (int i = 0; i < NRF_SCAN_HISTORY; i++) scanHistory[i] = -100;
    for (int i = 0; i < NRF_MAX_DETECTED; i++) detectedSignals[i].active = false;
    radio.stopListening();
    radio.setAutoAck(false);
    radio.setRetries(0, 0);
    radio.setCRCLength(RF24_CRC_DISABLED);
}

void nrf24StopScan() {
    scanning = false;
}

bool nrf24IsScanning() { return scanning; }
const int8_t* nrf24GetScanHistory() { return scanHistory; }
int nrf24GetScanIndex() { return scanIndex; }

uint8_t nrf24GetDetectedCount() { return detectedCount; }

DetectedSignal* nrf24GetDetected(uint8_t index) {
    if (index < detectedCount) return &detectedSignals[index];
    return nullptr;
}

void nrf24ScanLoop() {
    if (!scanning) return;

    if (millis() - scanLastUpdate >= 15) {
        scanLastUpdate = millis();

        int ch = scanIndex % 125;
        radio.setChannel(ch);
        delayMicroseconds(200);

        bool carrier = radio.testCarrier();
        bool rpd = radio.testRPD();
        int8_t rssi = -100;
        if (carrier || rpd) {
            rssi = -50 - random(15);
            // Adiciona/atualiza sinal detectado
            bool found = false;
            for (int i = 0; i < detectedCount; i++) {
                if (detectedSignals[i].channel == ch) {
                    detectedSignals[i].rssi = rssi;
                    detectedSignals[i].lastSeen = millis();
                    detectedSignals[i].active = true;
                    found = true;
                    break;
                }
            }
            if (!found && detectedCount < NRF_MAX_DETECTED) {
                detectedSignals[detectedCount].channel = ch;
                detectedSignals[detectedCount].rssi = rssi;
                detectedSignals[detectedCount].firstSeen = millis();
                detectedSignals[detectedCount].lastSeen = millis();
                detectedSignals[detectedCount].active = true;
                detectedCount++;
            }
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
    jamChannel = jamSelectMode ? jamSelectedChannel : 0;
    jamLastSwitch = 0;
    radio.setAutoAck(false);
    radio.stopListening();
    radio.setRetries(0, 0);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.setChannel(jamChannel);
    radio.startConstCarrier(RF24_PA_MAX, jamChannel);
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

    if (jamSelectMode) {
        // Jammer em canal fixo
        return jamChannel;
    } else {
        // Jammer varre todos os canais
        if (micros() - jamLastSwitch >= 500) {
            jamLastSwitch = micros();
            jamChannel++;
            if (jamChannel > 125) jamChannel = 0;
            radio.setChannel(jamChannel);
        }
        return jamChannel;
    }
}

// Getters/Setters para modo de jammer
bool nrf24JammerIsSelectMode() { return jamSelectMode; }
void nrf24JammerSetSelectMode(bool mode) { jamSelectMode = mode; }
int nrf24JammerGetSelectedChannel() { return jamSelectedChannel; }
void nrf24JammerSetSelectedChannel(int ch) {
    if (ch < 0) ch = 0;
    if (ch > 125) ch = 125;
    jamSelectedChannel = ch;
    if (nrf24JammerActive && jamSelectMode) {
        jamChannel = ch;
        radio.setChannel(ch);
    }
}

uint8_t nrf24GetDeviceCount() { return nrfDeviceCount; }
NRFDevice* nrf24GetDevice(uint8_t index) {
    if (index < nrfDeviceCount) return &nrfDevices[index];
    return nullptr;
}
bool nrf24IsJammerActive() { return nrf24JammerActive; }
bool nrf24IsAvailable() { return radio.isChipConnected(); }
