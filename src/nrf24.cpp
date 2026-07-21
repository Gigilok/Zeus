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
static bool jamSelectMode = false;
static int jamSelectedChannel = 0;
static uint32_t jamTotalPackets = 0;
static uint32_t jamChannelPackets = 0;

// Jammer SELECT CH - dados do grafico em tempo real
static int8_t jamHistory[16];
static int jamHistoryIndex = 0;

// Scanner - dados do grafico em tempo real
static int8_t scanBarData[16];
static int scanBarIndex = 0;

// Saved signals para replay
static SignalData nrfSavedSignals[MAX_SAVED_SIGNALS];
static uint8_t nrfSavedCount = 0;

// ============================================================
// JAMMER SELECT CH - Canais detectados para escolha
// ============================================================
static DetectedSignal jammerDetectedSignals[NRF_MAX_DETECTED];
static uint8_t jammerDetectedCount = 0;
static int8_t jammerSelectedIndex = 0;

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
// JAMMER SELECT CH - Scan rapido de canais
// ============================================================
void nrf24JammerScanChannels() {
    jammerDetectedCount = 0;
    jammerSelectedIndex = 0;
    for (int i = 0; i < NRF_MAX_DETECTED; i++) jammerDetectedSignals[i].active = false;

    // Varredura rapida de 2 passadas
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
// JAMMER - KILL ALL / SELECT CH
// ============================================================
//
// ABORDAGEM CORRETA (confirmada por smoochiee, Bruce, RF-Clown):
//
// KILL ALL (todos os canais):
//   1. Chama startConstCarrier() UMA VEZ para iniciar a portadora
//   2. No loop, chama setChannel() para fazer hopping rapido
//   3. O CONT_WAVE bit permanece ativo, entao o sinal continua emitido
//   4. setChannel() apenas muda o RF_CH registrador
//
// SELECT CH (um canal):
//   1. Chama startConstCarrier() UMA VEZ para iniciar a portadora
//   2. NUNCA chama setChannel() no loop
//   3. O sinal permanece fixo no canal configurado
//
// ============================================================

void nrf24StartJammer() {
    if (nrf24JammerActive) return;
    nrf24JammerActive = true;
    jamTotalPackets = 0;
    jamChannelPackets = 0;
    jamHistoryIndex = 0;
    for (int i = 0; i < 16; i++) jamHistory[i] = 0;

    // Configura o canal inicial
    if (jamSelectMode && jammerDetectedCount > 0 && jammerSelectedIndex >= 0) {
        jamChannel = jammerDetectedSignals[jammerSelectedIndex].channel;
    } else {
        jamChannel = 0;
    }

    jamLastSwitch = 0;

    // === INICIA A PORTADORA CONTINUA ===
    // startConstCarrier() faz:
    //   - stopListening()
    //   - Seta CONT_WAVE + PLL_LOCK no RF_SETUP
    //   - Configura TX para P variant (autoAck off, retries 0, etc)
    //   - setPALevel()
    //   - setChannel()  <-- ja configura o canal aqui!
    //   - ce(HIGH) para iniciar TX
    //   - Para P variant: ce(LOW) + reUseTX()
    //
    // O bit CONT_WAVE permanece ativo no RF_SETUP, entao o chip
    // continua emitindo portadora quando em modo TX.
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
    jamTotalPackets++;
    jamChannelPackets++;

    // Atualiza dados reais do grafico baseado na atividade do jammer
    jamHistoryIndex = (jamHistoryIndex + 1) % 16;

    if (jamSelectMode) {
        // ============================================================
        // MODO SELECT CH: FIXO no canal selecionado
        // ============================================================
        // NUNCA chamar setChannel() aqui!
        // startConstCarrier() ja configurou o canal no RF_CH.
        // O bit CONT_WAVE no RF_SETUP mantem a portadora ativa.
        // O canal permanece fixo.

        for (int i = 0; i < 16; i++) {
            if (i == (jamChannel / 8)) {
                jamHistory[i] = min(jamHistory[i] + 10, 40);
            } else {
                jamHistory[i] = max(jammerDetectedCount > 0 ? jamHistory[i] - 5 : 0, 2);
            }
        }
        return jamChannel;
    }

    // ============================================================
    // MODO KILL ALL: hopping de TODOS os canais
    // ============================================================
    // setChannel() muda o registrador RF_CH, mantendo CONT_WAVE ativo.
    // Isso faz o chip emitir portadora no novo canal.
    // O hopping rapido cobre toda a banda 2.4GHz.

    int activeBar = (jamChannel / 8) % 16;
    for (int i = 0; i < 16; i++) {
        if (i == activeBar) {
            jamHistory[i] = min(jamHistory[i] + 15, 45);
        } else {
            jamHistory[i] = max(jamHistory[i] - 3, 2);
        }
    }

    // Muda de canal a cada 500 microssegundos para cobrir toda a banda
    if (micros() - jamLastSwitch >= 500) {
        jamLastSwitch = micros();
        jamChannel++;
        jamChannelPackets = 0;
        if (jamChannel > 125) jamChannel = 0;

        // === MUDA O CANAL ===
        // setChannel() escreve no registrador RF_CH (0x05).
        // O bit CONT_WAVE no RF_SETUP permanece ativo.
        // O chip continua emitindo portadora, agora no novo canal.
        radio.setChannel(jamChannel);
    }

    return jamChannel;
}

const int8_t* nrf24GetJamHistory() { return jamHistory; }

uint32_t nrf24GetJamTotalPackets() { return jamTotalPackets; }
uint32_t nrf24GetJamChannelPackets() { return jamChannelPackets; }

bool nrf24JammerIsSelectMode() { return jamSelectMode; }
void nrf24JammerSetSelectMode(bool mode) { jamSelectMode = mode; }
int nrf24JammerGetSelectedChannel() { return jamSelectedChannel; }
void nrf24JammerSetSelectedChannel(int ch) {
    if (ch < 0) ch = 0;
    if (ch > 125) ch = 125;
    jamSelectedChannel = ch;

    // Se estiver ativo no modo SELECT e quiser mudar de canal,
    // precisamos parar e reiniciar o startConstCarrier no novo canal
    if (nrf24JammerActive && jamSelectMode) {
        jamChannel = ch;
        jamChannelPackets = 0;
        radio.stopConstCarrier();
        delayMicroseconds(100);
        radio.startConstCarrier(RF24_PA_MAX, ch);
    }
}

uint8_t nrf24GetDeviceCount() { return nrfDeviceCount; }
NRFDevice* nrf24GetDevice(uint8_t index) {
    if (index < nrfDeviceCount) return &nrfDevices[index];
    return nullptr;
}
bool nrf24IsJammerActive() { return nrf24JammerActive; }
bool nrf24IsAvailable() { return radio.isChipConnected(); }
