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

// Jammer - KILL ALL otimizado
static int jamChannel = 0;
static unsigned long jamLastSwitch = 0;
static uint32_t jamTotalPackets = 0;
static uint32_t jamChannelPackets = 0;

// Jammer - dados do grafico em tempo real
static int8_t jamHistory[16];
static int jamHistoryIndex = 0;

// Scanner - dados do grafico em tempo real
static int8_t scanBarData[16];
static int scanBarIndex = 0;

// Saved signals para replay
static SignalData nrfSavedSignals[MAX_SAVED_SIGNALS];
static uint8_t nrfSavedCount = 0;

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
// SCANNER - ONDAS EM TEMPO REAL + PACOTES
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

    // Atualiza dados das barras do grafico (agrupado por canal/8)
    int barIdx = (ch / 8) % 16;
    if (rssi > scanBarData[barIdx]) {
        scanBarData[barIdx] = rssi;
    }

    // Decaimento gradual das barras a cada ciclo completo (125 canais)
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
// ANALYZER - LISTA CANAIS + DETALHES + GRAVAR
// ============================================================
void nrf24StartAnalyze() {
    analyzing = true;
    detectedCount = 0;
    analyzeSelectedIndex = 0;
    for (int i = 0; i < NRF_MAX_DETECTED; i++) detectedSignals[i].active = false;

    // Varre todos os canais rapidamente - MULTIPLAS PASSADAS para garantir deteccao
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

// Grava sinal no slot de saved signals
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
// SCANNER SPECTRUM BARS (estilo imagem - barras finas em tempo real)
// ============================================================
#define SPEC_BARS         64    // 64 barras na tela
#define SPEC_BAR_WIDTH    1     // 1px por barra
#define SPEC_BAR_GAP      1     // 1px de gap
#define SPEC_CHANNELS     125   // 125 canais NRF24
#define SPEC_CH_PER_BAR   2     // 2 canais por barra

static int8_t specBarValues[SPEC_BARS];    // Altura atual (suavizada)
static int8_t specBarTargets[SPEC_BARS];   // Target (leitura real)
static int8_t specSelectedBar = 0;         // Barra selecionada (cursor)
static bool specRunning = false;
static uint32_t specFrames = 0;

void nrf24SpecInit() {
    specRunning = false;
    specFrames = 0;
    specSelectedBar = 0;
    for (int i = 0; i < SPEC_BARS; i++) {
        specBarValues[i] = -100;
        specBarTargets[i] = -100;
    }
}

void nrf24SpecScan() {
    if (!specRunning) return;

    // Varre todos os canais e agrupa em 64 barras
    for (int bar = 0; bar < SPEC_BARS; bar++) {
        int bestRssi = -100;
        int startCh = bar * SPEC_CH_PER_BAR;
        int endCh = startCh + SPEC_CH_PER_BAR;
        if (endCh > SPEC_CHANNELS) endCh = SPEC_CHANNELS;

        for (int ch = startCh; ch < endCh; ch++) {
            radio.setChannel(ch);
            radio.startListening();
            delayMicroseconds(120);

            bool rpd = radio.testRPD();
            bool carrier = radio.testCarrier();
            radio.stopListening();

            int rssi = -100;
            if (rpd) rssi = -55 - random(15);
            else if (carrier) rssi = -72 - random(10);

            if (rssi > bestRssi) bestRssi = rssi;
        }

        specBarTargets[bar] = bestRssi;
    }

    // Suavização: sobe rápido, desce lento (vu-meter)
    for (int i = 0; i < SPEC_BARS; i++) {
        int diff = specBarTargets[i] - specBarValues[i];
        if (diff > 0) {
            // Sobe rápido
            specBarValues[i] += max(diff * 2 / 3, 3);
            if (specBarValues[i] > specBarTargets[i]) 
                specBarValues[i] = specBarTargets[i];
        } else if (diff < 0) {
            // Desce devagar
            specBarValues[i] += min(diff / 6, -1);
            if (specBarValues[i] < specBarTargets[i])
                specBarValues[i] = specBarTargets[i];
        }
        if (specBarValues[i] < -100) specBarValues[i] = -100;
        if (specBarValues[i] > 0) specBarValues[i] = 0;
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
const int8_t* nrf24SpecGetBars() { return specBarValues; }
int8_t nrf24SpecGetSelectedBar() { return specSelectedBar; }
void nrf24SpecSetSelectedBar(int8_t bar) {
    if (bar >= 0 && bar < SPEC_BARS) specSelectedBar = bar;
}
int8_t nrf24SpecGetBarChannel(int8_t bar) {
    if (bar < 0 || bar >= SPEC_BARS) return -1;
    return bar * SPEC_CH_PER_BAR;
}

// ============================================================
// ANALYZER - LISTA CANAIS + DETALHES + GRAVAR
// ============================================================
void nrf24StartAnalyze() {
    analyzing = true;
    detectedCount = 0;
    analyzeSelectedIndex = 0;
    for (int i = 0; i < NRF_MAX_DETECTED; i++) detectedSignals[i].active = false;

    // Varre todos os canais rapidamente - MULTIPLAS PASSADAS para garantir deteccao
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

// Grava sinal no slot de saved signals
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
// SCANNER SPECTRUM BARS (estilo vídeo - barras verticais)
// ============================================================
#define SPECTRUM_BARS     16    // 16 barras na tela
#define SPECTRUM_CHANNELS 125   // 125 canais NRF24
#define BAR_WIDTH         7     // 7px por barra (16*7=112, sobra espaço)
#define BAR_GAP           1     // 1px de gap

static int8_t spectrumBarValues[SPECTRUM_BARS];   // Altura atual de cada barra
static int8_t spectrumBarTargets[SPECTRUM_BARS];  // Target (RSSI real)
static bool spectrumRunning = false;
static uint32_t spectrumFrames = 0;

void nrf24SpectrumInit() {
    spectrumRunning = false;
    spectrumFrames = 0;
    for (int i = 0; i < SPECTRUM_BARS; i++) {
        spectrumBarValues[i] = 0;
        spectrumBarTargets[i] = 0;
    }
}

void nrf24SpectrumScan() {
    if (!spectrumRunning) return;

    // Varre todos os canais e agrupa em 16 barras
    // Cada barra = ~8 canais (125/16 ≈ 8)
    int chPerBar = SPECTRUM_CHANNELS / SPECTRUM_BARS;  // 7 ou 8

    for (int bar = 0; bar < SPECTRUM_BARS; bar++) {
        int bestRssi = -100;
        int startCh = bar * chPerBar;
        int endCh = startCh + chPerBar;
        if (endCh > SPECTRUM_CHANNELS) endCh = SPECTRUM_CHANNELS;

        for (int ch = startCh; ch < endCh; ch++) {
            radio.setChannel(ch);
            radio.startListening();
            delayMicroseconds(200);

            bool rpd = radio.testRPD();
            bool carrier = radio.testCarrier();
            radio.stopListening();

            int rssi = -100;
            if (rpd) rssi = -60 - random(15);
            else if (carrier) rssi = -75 - random(10);

            if (rssi > bestRssi) bestRssi = rssi;
        }

        spectrumBarTargets[bar] = bestRssi;
    }

    // Suavização: as barras sobem/descem gradualmente
    for (int i = 0; i < SPECTRUM_BARS; i++) {
        int diff = spectrumBarTargets[i] - spectrumBarValues[i];
        if (diff > 0) {
            spectrumBarValues[i] += max(diff / 3, 2);  // Sobe rápido
        } else if (diff < 0) {
            spectrumBarValues[i] += min(diff / 4, -1); // Desce lento
        }
        if (spectrumBarValues[i] < -100) spectrumBarValues[i] = -100;
        if (spectrumBarValues[i] > 0) spectrumBarValues[i] = 0;
    }

    spectrumFrames++;
}

void nrf24SpectrumStart() {
    spectrumRunning = true;
    nrf24SpectrumInit();
    radio.setAutoAck(false);
    radio.setRetries(0, 0);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
}

void nrf24SpectrumStop() {
    spectrumRunning = false;
    radio.stopListening();
}

bool nrf24SpectrumIsRunning() { return spectrumRunning; }
uint32_t nrf24SpectrumGetFrames() { return spectrumFrames; }
const int8_t* nrf24SpectrumGetBars() { return spectrumBarValues; }

// ============================================================
// ANALYZER - LISTA CANAIS + DETALHES + GRAVAR
// ============================================================
void nrf24StartAnalyze() {
    analyzing = true;
    detectedCount = 0;
    analyzeSelectedIndex = 0;
    for (int i = 0; i < NRF_MAX_DETECTED; i++) detectedSignals[i].active = false;

    // Varre todos os canais rapidamente - MULTIPLAS PASSADAS para garantir deteccao
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

// Grava sinal no slot de saved signals
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
// SCANNER WATERFALL (estilo vídeo - spectrogram)
// ============================================================
#define WATERFALL_WIDTH   128   // Largura da tela
#define WATERFALL_HEIGHT  50    // Altura do gráfico (linhas de histórico)
#define WATERFALL_CHANNELS 125  // Canais NRF24

// Buffer do waterfall: cada linha = snapshot de 125 canais
// Usamos apenas 1 bit por canal (ativo/inativo) para economizar RAM
// 125 canais / 8 = 16 bytes por linha
static uint8_t waterfallBuffer[WATERFALL_HEIGHT][16];  // 50 * 16 = 800 bytes
static int waterfallHead = 0;   // Índice da linha mais recente
static bool waterfallRunning = false;
static uint32_t waterfallTotalFrames = 0;

void nrf24WaterfallInit() {
    waterfallHead = 0;
    waterfallTotalFrames = 0;
    waterfallRunning = false;
    for (int y = 0; y < WATERFALL_HEIGHT; y++) {
        for (int b = 0; b < 16; b++) {
            waterfallBuffer[y][b] = 0;
        }
    }
}

void nrf24WaterfallScan() {
    if (!waterfallRunning) return;

    // Cria nova linha de snapshot
    uint8_t newLine[16] = {0};

    // Varre todos os 125 canais rapidamente
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

    // Adiciona nova linha no topo (empurra histórico para baixo)
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

// Retorna ponteiro para o buffer (para o menu desenhar)
// A linha 0 = mais antiga, linha WATERFALL_HEIGHT-1 = mais recente
// Mas como é circular, precisamos reorganizar
void nrf24WaterfallGetLine(int lineIndex, uint8_t* outLine) {
    if (lineIndex < 0 || lineIndex >= WATERFALL_HEIGHT) return;
    // Calcula índice real no buffer circular
    // waterfallHead = linha mais recente (topo do gráfico)
    // Queremos: lineIndex 0 = topo (mais recente), lineIndex 49 = fundo (mais antigo)
    int realIdx = (waterfallHead - lineIndex + WATERFALL_HEIGHT) % WATERFALL_HEIGHT;
    memcpy(outLine, waterfallBuffer[realIdx], 16);
}

// ============================================================
// JAMMER - KILL ALL OTIMIZADO (SEM MODO SELECT)
// ============================================================
// Switch de canal a cada 50us (mais rapido que 500us anterior)
// Usa const carrier para saturacao maxima

#define JAM_SWITCH_INTERVAL_US  50   // 50 microssegundos por canal (antes era 500)
#define JAM_BURST_PACKETS       2    // 2 pacotes por canal por switch

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

    // Atualiza o histórico de barras com base na atividade real do jammer
    // Cada barra representa um grupo de ~8 canais (125/16 ≈ 8)
    int activeBar = (jamChannel / 8) % 16;

    for (int i = 0; i < 16; i++) {
        if (i == activeBar) {
            // Barra ativa: sobe rapidamente (saturação)
            jamHistory[i] = min(jamHistory[i] + 20, 50);
        } else {
            // Barras inativas: decaem lentamente
            jamHistory[i] = max(jamHistory[i] - 2, 2);
        }
    }

    // Switch ultra-rápido de canal a cada 50us
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
