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

// Scanner - ondas em tempo real
static int8_t scanHistory[NRF_SCAN_HISTORY];
static int scanIndex = 0;
static bool scanning = false;
static unsigned long scanLastUpdate = 0;
static uint32_t scanTotalPackets = 0;

// Analyzer - sinais detectados
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
static uint32_t jamTotalPackets = 0;
static uint32_t jamChannelPackets = 0;
static int8_t jamHistory[16];
static int jamHistoryIndex = 0;

// Scanner bars
static int8_t scanBarData[16];
static int scanBarIndex = 0;

// Saved signals
static SignalData nrfSavedSignals[MAX_SAVED_SIGNALS];
static uint8_t nrfSavedCount = 0;

// ============================================================
// SCANNER SPECTRUM - SCROLLING (64 barras deslizantes)
// ============================================================
#define SPEC_BARS         64
#define SPEC_BAR_WIDTH    3
#define SPEC_BAR_GAP      1
#define SPEC_CHANNELS     125
#define SPEC_CH_PER_BAR   2
#define SPEC_MAX_HEIGHT   80

// Ring buffer: cada posicao = altura de uma barra no tempo
// displayIdx 0 = esquerda (mais antigo), 63 = direita (mais recente)
static int8_t specBarValues[SPEC_BARS];
static int8_t specSelectedBar = 32;
static int8_t specAnalysisChannel = 64;
static bool specRunning = false;
static uint32_t specFrames = 0;
static int specWriteIndex = 0;

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
// SCANNER ANTIGO (mantido para compatibilidade)
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
    } else if (radio.testCarrier()) {
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
// SCANNER SPECTRUM - NOVO: le REAL do RF24
// ============================================================

void nrf24SpecInit() {
    specRunning = false;
    specFrames = 0;
    specWriteIndex = 0;
    specSelectedBar = 32;
    specAnalysisChannel = 64;
    for (int i = 0; i < SPEC_BARS; i++) {
        specBarValues[i] = 2;
    }
}

// Le UM canal do RF24 e retorna RSSI mapeado para altura (2-80)
// Retorna tambem se houve atividade (para deteccao de jamming)
static int8_t readOneChannel(int ch, bool& outActive) {
    radio.setChannel(ch);
    radio.startListening();
    delayMicroseconds(200);

    bool rpd = radio.testRPD();
    bool carrier = radio.testCarrier();
    radio.stopListening();

    outActive = (rpd || carrier);

    int rssi = -100;
    if (rpd) {
        rssi = -45 - random(15);   // Sinal FORTE: -60 a -45 dBm
    } else if (carrier) {
        rssi = -80 - random(10);   // Sinal FRACO: -90 a -80 dBm
    }
    // else: sem sinal = -100

    // Mapeia RSSI (-100 a -45) -> altura (2 a 80)
    int h = map(rssi, -100, -45, 2, SPEC_MAX_HEIGHT);
    if (h < 2) h = 2;
    if (h > SPEC_MAX_HEIGHT) h = SPEC_MAX_HEIGHT;
    return (int8_t)h;
}

// Suavizacao VU-meter: sobe rapido, desce lento
static int8_t smoothValue(int8_t current, int8_t target) {
    int diff = target - current;
    if (diff > 0) {
        current += max(diff * 2 / 3, 3);
        if (current > target) current = target;
    } else if (diff < 0) {
        current += min(diff / 6, -1);
        if (current < target) current = target;
    }
    if (current < 2) current = 2;
    if (current > SPEC_MAX_HEIGHT) current = SPEC_MAX_HEIGHT;
    return current;
}

// Varre todos os 125 canais e retorna o MELHOR valor de cada grupo de 2
// Se detectar JAMMING EXTERNO (atividade em >80% dos canais), retorna tudo zerado
static void scanAllChannels(int8_t* outBars) {
    for (int i = 0; i < SPEC_BARS; i++) outBars[i] = 2;

    int activeCount = 0;
    int8_t rawValues[SPEC_CHANNELS];
    bool activeFlags[SPEC_CHANNELS];

    // Primeira passada: le todos os canais
    for (int ch = 0; ch < SPEC_CHANNELS; ch++) {
        rawValues[ch] = readOneChannel(ch, activeFlags[ch]);
        if (activeFlags[ch]) activeCount++;
    }

    // DETECCAO DE JAMMING EXTERNO:
    // Se >80% dos canais estao ativos simultaneamente, e jamming (ruido)
    // Nesse caso, nao ha sinal valido — barras caem
    bool jammingDetected = (activeCount > (SPEC_CHANNELS * 8 / 10));

    if (jammingDetected) {
        // Jamming detectado: todas as barras caem para 2 (sem sinal)
        for (int i = 0; i < SPEC_BARS; i++) {
            outBars[i] = 2;
        }
        return;
    }

    // Normal: agrupa os melhores valores por barra
    for (int ch = 0; ch < SPEC_CHANNELS; ch++) {
        int barIdx = ch / SPEC_CH_PER_BAR;
        if (barIdx >= SPEC_BARS) barIdx = SPEC_BARS - 1;
        if (rawValues[ch] > outBars[barIdx]) outBars[barIdx] = rawValues[ch];
    }
}

void nrf24SpecScan() {
    if (!specRunning) return;

    // Varre todos os canais e pega 64 valores
    int8_t newReadings[SPEC_BARS];
    scanAllChannels(newReadings);

    // Suaviza e coloca no ring buffer
    // A barra mais a DIREITA (displayIdx = 63) = leitura mais recente
    // A barra mais a ESQUERDA (displayIdx = 0) = leitura mais antiga
    // 
    // Ring buffer: specWriteIndex aponta para a posicao MAIS ANTIGA
    // (proxima a ser sobrescrita). A mais recente esta em (writeIndex - 1).
    //
    // Para o display: displayIdx 0 = mais antigo = writeIndex
    //                 displayIdx 63 = mais recente = (writeIndex - 1)

    // Empurra TODOS os 64 valores no buffer (cada posicao = uma barra no tempo)
    // Na verdade, cada posicao do ring buffer deve guardar o snapshot de TODAS as barras
    // Mas isso gasta 64*64 = 4096 bytes...

    // SIMPLIFICACAO: O ring buffer guarda 64 valores. Cada valor = uma barra.
    // A cada ciclo, empurramos os 64 valores novos, um por um.
    // Isso faz o grafico "andar" da esquerda para direita.

    for (int i = 0; i < SPEC_BARS; i++) {
        specBarValues[specWriteIndex] = smoothValue(specBarValues[specWriteIndex], newReadings[i]);
        specWriteIndex = (specWriteIndex + 1) % SPEC_BARS;
    }

    specFrames++;
}

void nrf24SpecStart() {
    specRunning = true;
    nrf24SpecInit();
    radio.setAutoAck(false);
    radio.setRetries(0, 0);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
}

void nrf24SpecStop() {
    specRunning = false;
    radio.stopListening();
}

bool nrf24SpecIsRunning() { return specRunning; }
uint32_t nrf24SpecGetFrames() { return specFrames; }

// Retorna o valor de uma barra para desenho
// displayIdx 0 = esquerda (mais antigo), 63 = direita (mais recente)
int8_t nrf24SpecGetBarValue(int displayIdx) {
    if (displayIdx < 0 || displayIdx >= SPEC_BARS) return 2;
    // writeIndex aponta para a posicao mais antiga
    // displayIdx 0 = mais antigo = writeIndex
    // displayIdx 63 = mais recente = (writeIndex - 1 + 64) % 64
    int ringIdx = (specWriteIndex + displayIdx) % SPEC_BARS;
    return specBarValues[ringIdx];
}

int8_t nrf24SpecGetSelectedBar() { return specSelectedBar; }
void nrf24SpecSetSelectedBar(int8_t bar) {
    if (bar >= 0 && bar < SPEC_BARS) {
        specSelectedBar = bar;
        specAnalysisChannel = bar * SPEC_CH_PER_BAR;
    }
}

int8_t nrf24SpecGetAnalysisChannel() { return specAnalysisChannel; }
void nrf24SpecSetAnalysisChannel(int8_t ch) {
    if (ch >= 0 && ch < 125) {
        specAnalysisChannel = ch;
        specSelectedBar = ch / SPEC_CH_PER_BAR;
    }
}

int8_t nrf24SpecGetBarChannel(int8_t bar) {
    if (bar < 0 || bar >= SPEC_BARS) return -1;
    return bar * SPEC_CH_PER_BAR;
}

// ============================================================
// WATERFALL
// ============================================================
#define WATERFALL_WIDTH   128
#define WATERFALL_HEIGHT  50
#define WATERFALL_CHANNELS 125

static uint8_t waterfallBuffer[WATERFALL_HEIGHT][16];
static int waterfallHead = 0;
static bool waterfallRunning = false;
static uint32_t waterfallTotalFrames = 0;

void nrf24WaterfallInit() {
    waterfallHead = 0;
    waterfallTotalFrames = 0;
    waterfallRunning = false;
    for (int y = 0; y < WATERFALL_HEIGHT; y++)
        for (int b = 0; b < 16; b++)
            waterfallBuffer[y][b] = 0;
}

void nrf24WaterfallScan() {
    if (!waterfallRunning) return;
    uint8_t newLine[16] = {0};
    for (int ch = 0; ch < WATERFALL_CHANNELS; ch++) {
        radio.setChannel(ch);
        radio.startListening();
        delayMicroseconds(150);
        bool active = radio.testRPD() || radio.testCarrier();
        radio.stopListening();
        if (active) {
            int byteIdx = ch / 8;
            int bitIdx = ch % 8;
            newLine[byteIdx] |= (1 << bitIdx);
        }
    }
    waterfallHead = (waterfallHead + 1) % WATERFALL_HEIGHT;
    memcpy(waterfallBuffer[waterfallHead], newLine, 16);
    waterfallTotalFrames++;
}

void nrf24WaterfallStart() {
    waterfallRunning = true;
    radio.setAutoAck(false);
    radio.setRetries(0, 0);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
}

void nrf24WaterfallStop() {
    waterfallRunning = false;
    radio.stopListening();
}

bool nrf24WaterfallIsRunning() { return waterfallRunning; }
uint32_t nrf24WaterfallGetFrames() { return waterfallTotalFrames; }

void nrf24WaterfallGetLine(int lineIndex, uint8_t* outLine) {
    if (lineIndex < 0 || lineIndex >= WATERFALL_HEIGHT) return;
    int realIdx = (waterfallHead - lineIndex + WATERFALL_HEIGHT) % WATERFALL_HEIGHT;
    memcpy(outLine, waterfallBuffer[realIdx], 16);
}

// ============================================================
// JAMMER
// ============================================================
#define JAM_SWITCH_INTERVAL_US  50

void nrf24StartJammer() {
    if (nrf24JammerActive) return;
    nrf24JammerActive = true;
    jamTotalPackets = 0;
    jamChannelPackets = 0;
    jamHistoryIndex = 0;
    jamChannel = 0;
    jamLastSwitch = 0;
    for (int i = 0; i < 16; i++) jamHistory[i] = 0;
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
    jamTotalPackets++;
    jamChannelPackets++;
    int activeBar = (jamChannel / 8) % 16;
    for (int i = 0; i < 16; i++) {
        if (i == activeBar) jamHistory[i] = min(jamHistory[i] + 20, 50);
        else jamHistory[i] = max(jamHistory[i] - 2, 2);
    }
    unsigned long now = micros();
    if (now - jamLastSwitch >= JAM_SWITCH_INTERVAL_US) {
        jamLastSwitch = now;
        jamChannel++;
        jamChannelPackets = 0;
        if (jamChannel > 125) jamChannel = 0;
        radio.setChannel(jamChannel);
    }
    return jamChannel;
}

const int8_t* nrf24GetJamHistory() { return jamHistory; }
uint32_t nrf24GetJamTotalPackets() { return jamTotalPackets; }
uint32_t nrf24GetJamChannelPackets() { return jamChannelPackets; }

uint8_t nrf24GetDeviceCount() { return nrfDeviceCount; }
NRFDevice* nrf24GetDevice(uint8_t index) {
    if (index < nrfDeviceCount) return &nrfDevices[index];
    return nullptr;
}
bool nrf24IsJammerActive() { return nrf24JammerActive; }
bool nrf24IsAvailable() { return radio.isChipConnected(); }
