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
static uint32_t scanTotalPackets = 0;

// Analyzer
struct DetectedSignal {
    uint8_t channel;
    int8_t rssi;
    unsigned long lastSeen;
    bool active;
};
static DetectedSignal detectedSignals[NRF_MAX_DETECTED];
static uint8_t detectedCount = 0;
static uint8_t analyzeSelectedIndex = 0;
static bool analyzing = false;

// Jammer
static int jamChannel = 0;
static unsigned long jamLastSwitch = 0;
static bool jamSelectMode = false;
static int jamSelectedChannel = 0;
static uint32_t jamTotalPackets = 0;
static uint32_t jamChannelPackets = 0;
static int8_t jamHistory[16];
static int jamHistoryIndex = 0;
static int8_t scanBarData[16];
static int scanBarIndex = 0;

// Saved signals
static SignalData nrfSavedSignals[MAX_SAVED_SIGNALS];
static uint8_t nrfSavedCount = 0;

// Jammer SELECT CH
static DetectedSignal jammerDetectedSignals[NRF_MAX_DETECTED];
static uint8_t jammerDetectedCount = 0;
static int8_t jammerSelectedIndex = 0;

// ============================================================
// JAMMER PAYLOAD - Payload de 32 bytes para floodar o canal
// ============================================================
static const uint8_t jamPayload[32] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t jamAddress[5] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

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
    scanTotalPackets = 0;
    scanBarIndex = 0;
    for (int i = 0; i < NRF_SCAN_HISTORY; i++) scanHistory[i] = -100;
    for (int i = 0; i < 16; i++) scanBarData[i] = -100;
    radio.setAutoAck(false);
    radio.setRetries(0, 0);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
}

void nrf24StopScan() {
    scanning = false;
    radio.stopListening();
}

bool nrf24IsScanning() { return scanning; }
const int8_t* nrf24GetScanHistory() { return scanHistory; }
int nrf24GetScanIndex() { return scanIndex; }
uint32_t nrf24GetScanTotalPackets() { return scanTotalPackets; }
const int8_t* nrf24GetScanBarData() { return scanBarData; }

void nrf24ScanLoop() {
    if (!scanning) return;
    unsigned long now = millis();
    if (now - scanLastUpdate < 15) return;
    scanLastUpdate = now;
    int ch = scanIndex % 125;
    radio.setChannel(ch);
    radio.startListening();
    delayMicroseconds(250);
    bool rpd = radio.testRPD();
    int8_t rssi = -100;
    if (rpd) {
        rssi = -64 - random(20);
        scanTotalPackets++;
    }
    else if (radio.testCarrier()) {
        rssi = -75 - random(10);
    }
    radio.stopListening();
    scanHistory[scanIndex] = rssi;
    int barIdx = (ch / 8) % 16;
    if (rssi > scanBarData[barIdx]) scanBarData[barIdx] = rssi;
    if (scanIndex % 125 == 0) {
        for (int i = 0; i < 16; i++) {
            if (scanBarData[i] > -100) scanBarData[i] -= 5;
            if (scanBarData[i] < -100) scanBarData[i] = -100;
        }
    }
    scanIndex++;
    if (scanIndex >= NRF_SCAN_HISTORY) scanIndex = 0;
}

// ============================================================
// ANALYZER
// ============================================================
void nrf24StartAnalyze() {
    analyzing = true;
    detectedCount = 0;
    analyzeSelectedIndex = 0;
    for (int i = 0; i < NRF_MAX_DETECTED; i++) detectedSignals[i].active = false;
    for (int pass = 0; pass < 3; pass++) {
        for (int ch = 0; ch < 125; ch++) {
            radio.setChannel(ch);
            radio.startListening();
            delayMicroseconds(300);
            bool rpd = radio.testRPD();
            if (rpd) {
                int8_t rssi = -64 - random(20);
                bool found = false;
                for (int i = 0; i < detectedCount; i++) {
                    if (detectedSignals[i].channel == ch) {
                        detectedSignals[i].rssi = max(detectedSignals[i].rssi, rssi);
                        detectedSignals[i].lastSeen = millis();
                        detectedSignals[i].active = true;
                        found = true;
                        break;
                    }
                }
                if (!found && detectedCount < NRF_MAX_DETECTED) {
                    detectedSignals[detectedCount].channel = ch;
                    detectedSignals[detectedCount].rssi = rssi;
                    detectedSignals[detectedCount].lastSeen = millis();
                    detectedSignals[detectedCount].active = true;
                    detectedCount++;
                }
            }
            radio.stopListening();
            delayMicroseconds(50);
        }
    }
    analyzing = false;
}

bool nrf24IsAnalyzing() { return analyzing; }
uint8_t nrf24GetDetectedCount() { return detectedCount; }
DetectedSignal* nrf24GetDetected(uint8_t index) {
    if (index < detectedCount) return &detectedSignals[index];
    return nullptr;
}
uint8_t nrf24GetAnalyzeSelected() { return analyzeSelectedIndex; }
void nrf24SetAnalyzeSelected(uint8_t idx) {
    if (idx < detectedCount) analyzeSelectedIndex = idx;
}

bool nrf24SaveSignal(uint8_t detectedIdx) {
    if (detectedIdx >= detectedCount) return false;
    if (nrfSavedCount >= MAX_SAVED_SIGNALS) return false;
    DetectedSignal* sig = &detectedSignals[detectedIdx];
    nrfSavedSignals[nrfSavedCount].length = 1;
    nrfSavedSignals[nrfSavedCount].frequency = 2400000000UL + (sig->channel * 1000000UL);
    nrfSavedSignals[nrfSavedCount].modulation = 0;
    nrfSavedSignals[nrfSavedCount].valid = true;
    nrfSavedSignals[nrfSavedCount].data[0] = sig->channel;
    snprintf(nrfSavedSignals[nrfSavedCount].name, 16, "NRF CH%d", sig->channel);
    nrfSavedCount++;
    return true;
}

uint8_t nrf24GetSavedCount() { return nrfSavedCount; }
SignalData* nrf24GetSavedSignal(uint8_t index) {
    if (index < nrfSavedCount) return &nrfSavedSignals[index];
    return nullptr;
}

// ============================================================
// JAMMER SCAN (para SELECT CH)
// ============================================================
void nrf24JammerScanChannels() {
    jammerDetectedCount = 0;
    jammerSelectedIndex = 0;
    for (int i = 0; i < NRF_MAX_DETECTED; i++) jammerDetectedSignals[i].active = false;
    for (int pass = 0; pass < 2; pass++) {
        for (int ch = 0; ch < 125; ch++) {
            radio.setChannel(ch);
            radio.startListening();
            delayMicroseconds(200);
            bool rpd = radio.testRPD();
            if (rpd) {
                int8_t rssi = -64 - random(20);
                bool found = false;
                for (int i = 0; i < jammerDetectedCount; i++) {
                    if (jammerDetectedSignals[i].channel == ch) {
                        jammerDetectedSignals[i].rssi = max(jammerDetectedSignals[i].rssi, rssi);
                        jammerDetectedSignals[i].lastSeen = millis();
                        jammerDetectedSignals[i].active = true;
                        found = true;
                        break;
                    }
                }
                if (!found && jammerDetectedCount < NRF_MAX_DETECTED) {
                    jammerDetectedSignals[jammerDetectedCount].channel = ch;
                    jammerDetectedSignals[jammerDetectedCount].rssi = rssi;
                    jammerDetectedSignals[jammerDetectedCount].lastSeen = millis();
                    jammerDetectedSignals[jammerDetectedCount].active = true;
                    jammerDetectedCount++;
                }
            }
            radio.stopListening();
            delayMicroseconds(30);
        }
    }
}

uint8_t nrf24JammerGetDetectedCount() { return jammerDetectedCount; }
DetectedSignal* nrf24JammerGetDetected(uint8_t index) {
    if (index < jammerDetectedCount) return &jammerDetectedSignals[index];
    return nullptr;
}
int8_t nrf24JammerGetSelectedIndex() { return jammerSelectedIndex; }
void nrf24JammerSetSelectedIndex(int8_t idx) {
    if (idx >= 0 && idx < (int8_t)jammerDetectedCount) jammerSelectedIndex = idx;
}

// ============================================================
// JAMMER - KILL ALL / SELECT CH (VERSAO CORRIGIDA E FUNCIONAL)
// ============================================================
//
// ABORDAGEM CORRETA (baseada em projetos funcionais):
// - Usa writeFast() para enviar payloads em loop no canal atual
// - KILL ALL: Muda de canal a cada ~2ms, enviando multiplos pacotes em cada canal
// - SELECT CH: Fica no canal selecionado, enviando pacotes continuamente
//
// A chave é: O NRF24 precisa ENVIAR PACOTES, nao apenas carrier!
// ============================================================

void nrf24StartJammer() {
    if (nrf24JammerActive) return;
    nrf24JammerActive = true;
    jamTotalPackets = 0;
    jamChannelPackets = 0;
    jamHistoryIndex = 0;
    for (int i = 0; i < 16; i++) jamHistory[i] = 0;

    // Configura o radio para modo TX de jamming
    radio.stopListening();
    radio.setAutoAck(false);
    radio.setRetries(0, 0);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setAddressWidth(3);  // Endereco menor = menos overhead
    radio.openWritingPipe(jamAddress);
    radio.flush_tx();

    if (jamSelectMode && jammerDetectedCount > 0 && jammerSelectedIndex >= 0) {
        jamChannel = jammerDetectedSignals[jammerSelectedIndex].channel;
    } else {
        jamChannel = 0;
    }

    jamLastSwitch = micros();
    radio.setChannel(jamChannel);
}

void nrf24StopJammer() {
    if (!nrf24JammerActive) return;
    digitalWrite(NRF_CE, LOW);
    radio.flush_tx();
    radio.stopListening();
    nrf24JammerActive = false;
}

int nrf24JammerLoop() {
    if (!nrf24JammerActive) return -1;

    if (jamSelectMode) {
        // ============================================================
        // MODO SELECT CH: FIXO no canal selecionado
        // Envia o maximo de pacotes possivel no canal escolhido
        // ============================================================
        
        // Envia multiplos pacotes em sequencia rapida
        for (int i = 0; i < 15; i++) {
            radio.writeFast(&jamPayload, 32);
            jamTotalPackets++;
            jamChannelPackets++;
        }
        radio.txStandBy();  // Garante que todos os pacotes sairam

        // Atualiza o grafico de barras - apenas a barra do canal ativo
        int activeBar = (jamChannel / 8) % 16;
        for (int i = 0; i < 16; i++) {
            if (i == activeBar) {
                jamHistory[i] = min(jamHistory[i] + 8, 45);
            } else {
                jamHistory[i] = max(jamHistory[i] - 2, 0);
            }
        }
        return jamChannel;

    } else {
        // ============================================================
        // MODO KILL ALL: HOPPING por todos os 125 canais
        // Muda de canal rapidamente e envia pacotes em cada um
        // ============================================================
        
        unsigned long now = micros();
        
        // Muda de canal a cada ~1500 microssegundos (aprox 666 canais/seg)
        // Envia 5 pacotes em cada canal antes de mudar
        if (now - jamLastSwitch >= 1500) {
            jamLastSwitch = now;
            
            // Envia pacotes no canal atual antes de mudar
            for (int i = 0; i < 5; i++) {
                radio.writeFast(&jamPayload, 32);
                jamTotalPackets++;
            }
            radio.txStandBy();
            
            // Avanca para o proximo canal
            jamChannel++;
            jamChannelPackets = 0;
            if (jamChannel > 125) jamChannel = 0;
            
            // Muda o canal do radio
            radio.setChannel(jamChannel);
        } else {
            // No intervalo entre mudancas de canal, continua enviando
            radio.writeFast(&jamPayload, 32);
            jamTotalPackets++;
            jamChannelPackets++;
        }

        // Atualiza o grafico de barras
        int activeBar = (jamChannel / 8) % 16;
        for (int i = 0; i < 16; i++) {
            if (i == activeBar) {
                jamHistory[i] = min(jamHistory[i] + 12, 45);
            } else {
                jamHistory[i] = max(jamHistory[i] - 3, 2);
            }
        }

        return jamChannel;
    }
}

const int8_t* nrf24GetJamHistory() { return jamHistory; }

uint32_t nrf24GetJamTotalPackets() { return jamTotalPackets; }
uint32_t nrf24GetJamChannelPackets() { return jamChannelPackets; }

bool nrf24JammerIsSelectMode() { return jamSelectMode; }
void nrf24JammerSetSelectMode(bool mode) {
    jamSelectMode = mode;
}
int nrf24JammerGetSelectedChannel() { return jamSelectedChannel; }
void nrf24JammerSetSelectedChannel(int ch) {
    if (ch < 0) ch = 0;
    if (ch > 125) ch = 125;
    jamSelectedChannel = ch;
    if (nrf24JammerActive && jamSelectMode) {
        jamChannel = ch;
        jamChannelPackets = 0;
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
