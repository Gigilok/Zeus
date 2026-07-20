#include <SPI.h>
#include "config.h"

// CC1101 usa HSPI compartilhado com NRF24
// Inicializa SPI aqui uma unica vez para ambos

#define CC1101_IOCFG2   0x00
#define CC1101_IOCFG0   0x02
#define CC1101_FIFOTHR  0x03
#define CC1101_PKTCTRL0 0x08
#define CC1101_FREQ2    0x0D
#define CC1101_FREQ1    0x0E
#define CC1101_FREQ0    0x0F
#define CC1101_MDMCFG4  0x10
#define CC1101_MDMCFG3  0x11
#define CC1101_MDMCFG2  0x12
#define CC1101_MDMCFG1  0x13
#define CC1101_MDMCFG0  0x14
#define CC1101_DEVIATN  0x15
#define CC1101_MCSM0    0x18
#define CC1101_FOCCFG   0x19
#define CC1101_BSCFG    0x1A
#define CC1101_AGCCTRL2 0x1B
#define CC1101_AGCCTRL1 0x1C
#define CC1101_AGCCTRL0 0x1D
#define CC1101_FREND0   0x22
#define CC1101_FSCAL3   0x23
#define CC1101_FSCAL2   0x24
#define CC1101_FSCAL1   0x25
#define CC1101_FSCAL0   0x26
#define CC1101_TEST2    0x2C
#define CC1101_TEST1    0x2D
#define CC1101_TEST0    0x2E
#define CC1101_SRES     0x30
#define CC1101_SRX      0x34
#define CC1101_STX      0x35
#define CC1101_SIDLE    0x36
#define CC1101_PATABLE  0x3E

#define CC1101_READ_SINGLE  0x80
#define CC1101_READ_BURST   0xC0
#define CC1101_WRITE_BURST  0x40

bool cc1101Initialized = false;
static bool spiInitialized = false;

const uint32_t commonFrequencies[] = {
    315000000, 433920000, 868350000, 915000000
};

void cc1101Select() { digitalWrite(CC1101_CSN, LOW); }
void cc1101Deselect() { digitalWrite(CC1101_CSN, HIGH); }

uint8_t cc1101ReadReg(uint8_t reg) {
    cc1101Select();
    SPI.transfer(reg | CC1101_READ_SINGLE);
    uint8_t val = SPI.transfer(0x00);
    cc1101Deselect();
    return val;
}

void cc1101WriteReg(uint8_t reg, uint8_t value) {
    cc1101Select();
    SPI.transfer(reg);
    SPI.transfer(value);
    cc1101Deselect();
}

void cc1101SendCommand(uint8_t cmd) {
    cc1101Select();
    SPI.transfer(cmd);
    cc1101Deselect();
}

void cc1101SetFrequency(uint32_t freqHz) {
    uint32_t freqWord = (uint32_t)((freqHz / 26000000.0) * 65536);
    cc1101WriteReg(CC1101_FREQ2, (freqWord >> 16) & 0xFF);
    cc1101WriteReg(CC1101_FREQ1, (freqWord >> 8) & 0xFF);
    cc1101WriteReg(CC1101_FREQ0, freqWord & 0xFF);
}

bool cc1101Init() {
    // Inicializa HSPI uma unica vez (compartilhado com NRF24)
    if (!spiInitialized) {
        SPI.begin(NRF_SCK, NRF_MISO, NRF_MOSI);  // SCK=18, MISO=19, MOSI=23
        SPI.setFrequency(4000000);
        SPI.setDataMode(SPI_MODE0);
        spiInitialized = true;
    }

    pinMode(CC1101_CSN, OUTPUT);
    digitalWrite(CC1101_CSN, HIGH);
    pinMode(CC1101_GDO0, INPUT);
    pinMode(CC1101_GDO2, INPUT);

    // Reset
    cc1101Select();
    SPI.transfer(CC1101_SRES);
    delay(1);
    cc1101Deselect();
    delay(10);

    // Verifica PARTNUM (reg 0x30) - CC1101 retorna 0x00
    uint8_t partnum = cc1101ReadReg(0x30);
    uint8_t version = cc1101ReadReg(0x31);

    // Se nao respondeu, tenta de novo
    if (partnum != 0x00) {
        delay(10);
        cc1101SendCommand(CC1101_SRES);
        delay(10);
        partnum = cc1101ReadReg(0x30);
        version = cc1101ReadReg(0x31);
    }

    if (partnum == 0x00) {
        cc1101WriteReg(CC1101_IOCFG0, 0x2E);
        cc1101WriteReg(CC1101_FIFOTHR, 0x47);
        cc1101WriteReg(CC1101_PKTCTRL0, 0x32);
        cc1101WriteReg(CC1101_MDMCFG4, 0xF5);
        cc1101WriteReg(CC1101_MDMCFG3, 0x83);
        cc1101WriteReg(CC1101_MDMCFG2, 0x30);
        cc1101WriteReg(CC1101_MDMCFG1, 0x22);
        cc1101WriteReg(CC1101_MDMCFG0, 0xF8);
        cc1101WriteReg(CC1101_DEVIATN, 0x15);
        cc1101WriteReg(CC1101_MCSM0, 0x18);
        cc1101WriteReg(CC1101_FOCCFG, 0x16);
        cc1101WriteReg(CC1101_BSCFG, 0x6C);
        cc1101WriteReg(CC1101_AGCCTRL2, 0x43);
        cc1101WriteReg(CC1101_AGCCTRL1, 0x40);
        cc1101WriteReg(CC1101_AGCCTRL0, 0x91);
        cc1101WriteReg(CC1101_FREND0, 0x11);
        cc1101WriteReg(CC1101_FSCAL3, 0xE9);
        cc1101WriteReg(CC1101_FSCAL2, 0x2A);
        cc1101WriteReg(CC1101_FSCAL1, 0x00);
        cc1101WriteReg(CC1101_FSCAL0, 0x1F);
        cc1101WriteReg(CC1101_TEST2, 0x81);
        cc1101WriteReg(CC1101_TEST1, 0x35);
        cc1101WriteReg(CC1101_TEST0, 0x09);

        cc1101Select();
        SPI.transfer(CC1101_PATABLE | CC1101_WRITE_BURST);
        for (int i = 0; i < 8; i++) SPI.transfer(0xC0);
        cc1101Deselect();

        cc1101Initialized = true;
        return true;
    }

    return false;
}

struct SignalCapture {
    uint32_t timestamps[512];
    uint8_t values[512];
    uint16_t count;
    uint32_t frequency;
    bool active;
};

SignalCapture currentCapture;

void cc1101StartCapture() {
    if (!cc1101Initialized) return;
    cc1101CopyActive = true;
    currentCapture.count = 0;
    currentCapture.active = true;

    int bestRssi = -128;
    uint32_t bestFreq = 433920000;

    for (int f = 0; f < 4; f++) {
        cc1101SetFrequency(commonFrequencies[f]);
        cc1101SendCommand(CC1101_SRX);
        delay(500);
        uint8_t rssi_raw = cc1101ReadReg(0x34);
        int rssi = (rssi_raw >= 128) ? (rssi_raw - 256) / 2 - 74 : rssi_raw / 2 - 74;
        if (rssi > bestRssi) {
            bestRssi = rssi;
            bestFreq = commonFrequencies[f];
        }
    }

    currentCapture.frequency = bestFreq;
    cc1101SetFrequency(bestFreq);
    cc1101SendCommand(CC1101_SRX);

    unsigned long startTime = millis();
    uint8_t lastValue = digitalRead(CC1101_GDO0);

    while (millis() - startTime < CAPTURE_DURATION && currentCapture.count < 512) {
        uint8_t val = digitalRead(CC1101_GDO0);
        if (val != lastValue) {
            currentCapture.timestamps[currentCapture.count] = micros();
            currentCapture.values[currentCapture.count] = val;
            currentCapture.count++;
            lastValue = val;
        }
        yield();
    }

    currentCapture.active = false;
    cc1101CopyActive = false;
    cc1101SendCommand(CC1101_SIDLE);

    if (currentCapture.count > 10 && savedSignalCount < MAX_SAVED_SIGNALS) {
        savedSignals[savedSignalCount].length = (currentCapture.count > 64) ? 64 : currentCapture.count;
        savedSignals[savedSignalCount].frequency = currentCapture.frequency;
        savedSignals[savedSignalCount].modulation = 0;
        savedSignals[savedSignalCount].valid = true;
        for (int i = 0; i < savedSignals[savedSignalCount].length; i++) {
            savedSignals[savedSignalCount].data[i] = currentCapture.values[i];
        }
        snprintf(savedSignals[savedSignalCount].name, 16, "Sinal %d", savedSignalCount + 1);
        savedSignalCount++;
    }
}

void cc1101StopCapture() {
    cc1101CopyActive = false;
    currentCapture.active = false;
    cc1101SendCommand(CC1101_SIDLE);
}

void cc1101ReplaySignal(uint8_t index) {
    if (index >= savedSignalCount || !savedSignals[index].valid) return;
    if (!cc1101Initialized) return;
    cc1101SetFrequency(savedSignals[index].frequency);
    cc1101SendCommand(CC1101_STX);
    for (int i = 0; i < savedSignals[index].length; i++) {
        digitalWrite(CC1101_GDO0, savedSignals[index].data[i]);
        delayMicroseconds(500);
    }
    cc1101SendCommand(CC1101_SIDLE);
}

bool cc1101IsAvailable() { return cc1101Initialized; }
bool cc1101IsCapturing() { return cc1101CopyActive; }
uint8_t cc1101GetSavedCount() { return savedSignalCount; }

SignalData* cc1101GetSignal(uint8_t index) {
    if (index < savedSignalCount) return &savedSignals[index];
    return nullptr;
}
