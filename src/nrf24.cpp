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

// ============================================================
// FUNCOES DE DEBUG DOS REGISTRADORES
// ============================================================
#define REG_CONFIG      0x00
#define REG_EN_AA       0x01
#define REG_EN_RXADDR   0x02
#define REG_SETUP_AW    0x03
#define REG_SETUP_RETR  0x04
#define REG_RF_CH       0x05
#define REG_RF_SETUP    0x06
#define REG_STATUS      0x07
#define REG_OBSERVE_TX  0x08
#define REG_CD          0x09
#define REG_RX_ADDR_P0  0x0A
#define REG_TX_ADDR     0x10
#define REG_FIFO_STATUS 0x17
#define REG_DYNPD       0x1C
#define REG_FEATURE     0x1D

static uint8_t readReg(uint8_t reg) {
    uint8_t result;
    digitalWrite(NRF_CSN, LOW);
    SPI.transfer(0x00 | reg);
    result = SPI.transfer(0xFF);
    digitalWrite(NRF_CSN, HIGH);
    return result;
}

static void printAllRegisters() {
    Serial.println("  === REGISTRADORES NRF24 ===");
    Serial.print("  CONFIG     (0x00): 0x"); Serial.println(readReg(REG_CONFIG), HEX);
    Serial.print("  EN_AA      (0x01): 0x"); Serial.println(readReg(REG_EN_AA), HEX);
    Serial.print("  EN_RXADDR  (0x02): 0x"); Serial.println(readReg(REG_EN_RXADDR), HEX);
    Serial.print("  SETUP_AW   (0x03): 0x"); Serial.println(readReg(REG_SETUP_AW), HEX);
    Serial.print("  SETUP_RETR (0x04): 0x"); Serial.println(readReg(REG_SETUP_RETR), HEX);
    Serial.print("  RF_CH      (0x05): 0x"); Serial.println(readReg(REG_RF_CH), HEX);
    Serial.print("  RF_SETUP   (0x06): 0x"); Serial.println(readReg(REG_RF_SETUP), HEX);
    Serial.print("  STATUS     (0x07): 0x"); Serial.println(readReg(REG_STATUS), HEX);
    Serial.print("  OBSERVE_TX (0x08): 0x"); Serial.println(readReg(REG_OBSERVE_TX), HEX);
    Serial.print("  CD         (0x09): 0x"); Serial.println(readReg(REG_CD), HEX);
    Serial.print("  FIFO_STATUS(0x17): 0x"); Serial.println(readReg(REG_FIFO_STATUS), HEX);
    Serial.print("  DYNPD      (0x1C): 0x"); Serial.println(readReg(REG_DYNPD), HEX);
    Serial.print("  FEATURE    (0x1D): 0x"); Serial.println(readReg(REG_FEATURE), HEX);
    Serial.println("  ============================");
}

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
    Serial.println("[NRF24] nrf24Init() chamado");
    pinMode(NRF_CE, OUTPUT);
    pinMode(NRF_CSN, OUTPUT);
    digitalWrite(NRF_CSN, HIGH);
    hardResetNRF24();
    if (!radio.begin()) {
        Serial.println("[NRF24] ERRO: radio.begin() falhou!");
        return false;
    }
    Serial.println("[NRF24] radio.begin() OK");
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setAutoAck(false);
    radio.disableCRC();
    radio.stopListening();
    Serial.print("[NRF24] isChipConnected=");
    Serial.println(radio.isChipConnected() ? "SIM" : "NAO");
    Serial.print("[NRF24] isPVariant=");
    Serial.println(radio.isPVariant() ? "SIM" : "NAO");
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
// ANALYZER - LISTA CANAIS + DETALHES + GRAVAR
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
// JAMMER SELECT CH - Scan rapido de canais
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
// JAMMER - KILL ALL / SELECT CH  (CORRIGIDO - CE HIGH MANUAL)
// ============================================================
//
// PROBLEMA ENCONTRADO NOS LOGS:
// - startConstCarrier() para chips P variant faz:
//   1. CE HIGH por 1ms
//   2. CE LOW
//   3. reUseTX() -> CE HIGH por 10us, depois CE LOW
// - Mas TX FIFO está VAZIO (TX_EMPTY=1), então reUseTX() não funciona
// - Resultado: CE fica LOW, chip não transmite!
//
// SOLUÇÃO:
// - Após startConstCarrier(), forçar CE HIGH manualmente
// - Manter CE HIGH durante todo o jamming
// - CONT_WAVE bit no RF_SETUP faz o chip emitir portadora contínua em modo TX
// - setChannel() muda o canal mantendo a transmissão ativa
//
// ============================================================

void nrf24StartJammer() {
    Serial.println("");
    Serial.println("========================================");
    Serial.println("[NRF24] nrf24StartJammer() CHAMADO");
    Serial.println("========================================");

    if (nrf24JammerActive) {
        Serial.println("[NRF24] JA ESTAVA ATIVO! Retornando.");
        return;
    }

    nrf24JammerActive = true;
    jamTotalPackets = 0;
    jamChannelPackets = 0;
    jamHistoryIndex = 0;
    for (int i = 0; i < 16; i++) jamHistory[i] = 0;

    if (jamSelectMode && jammerDetectedCount > 0 && jammerSelectedIndex >= 0) {
        jamChannel = jammerDetectedSignals[jammerSelectedIndex].channel;
        Serial.print("[NRF24] Modo: SELECT CH | Canal=");
        Serial.println(jamChannel);
    } else {
        jamChannel = 0;
        Serial.println("[NRF24] Modo: KILL ALL | Canal inicial=0");
    }

    jamLastSwitch = 0;

    Serial.println("[NRF24] Registradores ANTES de startConstCarrier:");
    printAllRegisters();

    Serial.print("[NRF24] >>> Chamando radio.startConstCarrier(RF24_PA_MAX, ");
    Serial.print(jamChannel);
    Serial.println(")...");

    // Inicia a portadora contínua
    radio.startConstCarrier(RF24_PA_MAX, jamChannel);

    Serial.println("[NRF24] <<< startConstCarrier() retornou!");

    // === CORREÇÃO CRÍTICA ===
    // startConstCarrier() para chips P variant deixa CE LOW após reUseTX().
    // Mas TX FIFO está vazio, então reUseTX() não funciona.
    // Precisamos forçar CE HIGH manualmente para manter a transmissão ativa.
    // CONT_WAVE bit no RF_SETUP faz o chip emitir portadora contínua quando CE=HIGH.
    Serial.println("[NRF24] >>> FORCANDO CE HIGH manualmente!");
    digitalWrite(NRF_CE, HIGH);
    Serial.println("[NRF24] <<< CE agora está HIGH");

    Serial.println("[NRF24] Registradores DEPOIS de startConstCarrier + CE HIGH:");
    printAllRegisters();

    Serial.println("[NRF24] nrf24StartJammer() CONCLUIDO");
    Serial.println("========================================");
    Serial.println("");
}

void nrf24StopJammer() {
    Serial.println("");
    Serial.println("========================================");
    Serial.println("[NRF24] nrf24StopJammer() CHAMADO");
    Serial.println("========================================");

    if (!nrf24JammerActive) {
        Serial.println("[NRF24] NAO ESTAVA ATIVO! Retornando.");
        return;
    }

    Serial.println("[NRF24] Registradores ANTES de stopConstCarrier:");
    printAllRegisters();

    Serial.println("[NRF24] >>> Chamando radio.stopConstCarrier()...");
    radio.stopConstCarrier();
    Serial.println("[NRF24] <<< stopConstCarrier() retornou!");

    // === CORREÇÃO: Garantir que CE vai para LOW ===
    Serial.println("[NRF24] >>> FORCANDO CE LOW manualmente!");
    digitalWrite(NRF_CE, LOW);
    Serial.println("[NRF24] <<< CE agora está LOW");

    radio.stopListening();
    radio.flush_tx();
    nrf24JammerActive = false;

    Serial.println("[NRF24] Registradores DEPOIS de stopConstCarrier + CE LOW:");
    printAllRegisters();

    Serial.println("[NRF24] nrf24StopJammer() CONCLUIDO");
    Serial.println("========================================");
    Serial.println("");
}

static unsigned long debugLastPrint = 0;
static int debugPrintCounter = 0;

int nrf24JammerLoop() {
    if (!nrf24JammerActive) return -1;

    jamTotalPackets++;
    jamChannelPackets++;
    jamHistoryIndex = (jamHistoryIndex + 1) % 16;

    if (jamSelectMode) {
        // === MODO SELECT CH: FIXO ===
        // NUNCA chama setChannel() aqui!
        // CE permanece HIGH (configurado no start)
        // A portadora contínua é emitida no canal fixo

        if (millis() - debugLastPrint >= 2000) {
            debugLastPrint = millis();
            debugPrintCounter++;
            uint8_t rf_ch = readReg(REG_RF_CH);
            uint8_t rf_setup = readReg(REG_RF_SETUP);
            uint8_t config = readReg(REG_CONFIG);
            uint8_t fifo_status = readReg(REG_FIFO_STATUS);

            Serial.println("");
            Serial.println("--- JAMMER SELECT CH (DEBUG) ---");
            Serial.print("  Iteracao: "); Serial.println(debugPrintCounter);
            Serial.print("  Canal configurado (variavel): "); Serial.println(jamChannel);
            Serial.print("  Canal no registrador RF_CH: "); Serial.println(rf_ch);
            Serial.print("  RF_SETUP: 0x"); Serial.println(rf_setup, HEX);
            Serial.print("    CONT_WAVE="); Serial.print((rf_setup & 0x80) ? "1" : "0");
            Serial.print(" PLL_LOCK="); Serial.print((rf_setup & 0x10) ? "1" : "0");
            Serial.print(" RF_DR="); Serial.print((rf_setup & 0x08) ? "1(2Mbps)" : "0(1Mbps)");
            Serial.print(" RF_PWR="); Serial.println((rf_setup >> 1) & 0x03);
            Serial.print("  CONFIG: 0x"); Serial.println(config, HEX);
            Serial.print("    PRIM_RX="); Serial.print((config & 0x01) ? "RX" : "TX");
            Serial.print(" PWR_UP="); Serial.print((config & 0x02) ? "UP" : "DOWN");
            Serial.print(" EN_CRC="); Serial.println((config & 0x08) ? "1" : "0");
            Serial.print("  FIFO_STATUS: 0x"); Serial.println(fifo_status, HEX);
            Serial.print("    TX_EMPTY="); Serial.print((fifo_status & 0x10) ? "1" : "0");
            Serial.print(" TX_FULL="); Serial.print((fifo_status & 0x20) ? "1" : "0");
            Serial.print(" REUSE_TX_PL="); Serial.println((fifo_status & 0x40) ? "1" : "0");
            Serial.print("  Total packets: "); Serial.println(jamTotalPackets);
            Serial.print("  isChipConnected: "); Serial.println(radio.isChipConnected() ? "SIM" : "NAO");
            Serial.println("--------------------------------");
            Serial.println("");
        }

        for (int i = 0; i < 16; i++) {
            if (i == (jamChannel / 8)) {
                jamHistory[i] = min(jamHistory[i] + 10, 40);
            } else {
                jamHistory[i] = max(jammerDetectedCount > 0 ? jamHistory[i] - 5 : 0, 2);
            }
        }
        return jamChannel;
    }

    // === MODO KILL ALL: HOPPING ===
    // CE permanece HIGH (configurado no start)
    // setChannel() muda o canal mantendo a transmissão ativa
    // A portadora contínua é emitida em cada canal

    if (millis() - debugLastPrint >= 2000) {
        debugLastPrint = millis();
        debugPrintCounter++;
        uint8_t rf_ch = readReg(REG_RF_CH);
        uint8_t rf_setup = readReg(REG_RF_SETUP);
        uint8_t config = readReg(REG_CONFIG);
        uint8_t fifo_status = readReg(REG_FIFO_STATUS);

        Serial.println("");
        Serial.println("--- JAMMER KILL ALL (DEBUG) ---");
        Serial.print("  Iteracao: "); Serial.println(debugPrintCounter);
        Serial.print("  Canal configurado (variavel): "); Serial.println(jamChannel);
        Serial.print("  Canal no registrador RF_CH: "); Serial.println(rf_ch);
        Serial.print("  RF_SETUP: 0x"); Serial.println(rf_setup, HEX);
        Serial.print("    CONT_WAVE="); Serial.print((rf_setup & 0x80) ? "1" : "0");
        Serial.print(" PLL_LOCK="); Serial.print((rf_setup & 0x10) ? "1" : "0");
        Serial.print(" RF_DR="); Serial.print((rf_setup & 0x08) ? "1(2Mbps)" : "0(1Mbps)");
        Serial.print(" RF_PWR="); Serial.println((rf_setup >> 1) & 0x03);
        Serial.print("  CONFIG: 0x"); Serial.println(config, HEX);
        Serial.print("    PRIM_RX="); Serial.print((config & 0x01) ? "RX" : "TX");
        Serial.print(" PWR_UP="); Serial.print((config & 0x02) ? "UP" : "DOWN");
        Serial.print(" EN_CRC="); Serial.println((config & 0x08) ? "1" : "0");
        Serial.print("  FIFO_STATUS: 0x"); Serial.println(fifo_status, HEX);
        Serial.print("    TX_EMPTY="); Serial.print((fifo_status & 0x10) ? "1" : "0");
        Serial.print(" TX_FULL="); Serial.print((fifo_status & 0x20) ? "1" : "0");
        Serial.print(" REUSE_TX_PL="); Serial.println((fifo_status & 0x40) ? "1" : "0");
        Serial.print("  Total packets: "); Serial.println(jamTotalPackets);
        Serial.print("  isChipConnected: "); Serial.println(radio.isChipConnected() ? "SIM" : "NAO");
        Serial.println("-------------------------------");
        Serial.println("");
    }

    int activeBar = (jamChannel / 8) % 16;
    for (int i = 0; i < 16; i++) {
        if (i == activeBar) {
            jamHistory[i] = min(jamHistory[i] + 15, 45);
        } else {
            jamHistory[i] = max(jamHistory[i] - 3, 2);
        }
    }

    // Muda de canal a cada 500 microssegundos
    if (micros() - jamLastSwitch >= 500) {
        jamLastSwitch = micros();
        int oldChannel = jamChannel;
        jamChannel++;
        jamChannelPackets = 0;
        if (jamChannel > 125) jamChannel = 0;

        radio.setChannel(jamChannel);

        // Debug: print quando muda de canal
        static int channelChangeCounter = 0;
        channelChangeCounter++;
        if (channelChangeCounter % 500 == 0) {
            uint8_t actual_ch = readReg(REG_RF_CH);
            Serial.print("[NRF24] HOP: ");
            Serial.print(oldChannel);
            Serial.print(" -> ");
            Serial.print(jamChannel);
            Serial.print(" | RF_CH reg=");
            Serial.println(actual_ch);
        }
    }

    return jamChannel;
}

const int8_t* nrf24GetJamHistory() { return jamHistory; }

uint32_t nrf24GetJamTotalPackets() { return jamTotalPackets; }
uint32_t nrf24GetJamChannelPackets() { return jamChannelPackets; }

bool nrf24JammerIsSelectMode() { return jamSelectMode; }
void nrf24JammerSetSelectMode(bool mode) { 
    Serial.print("[NRF24] SetSelectMode: ");
    Serial.println(mode ? "SELECT CH" : "KILL ALL");
    jamSelectMode = mode; 
}
int nrf24JammerGetSelectedChannel() { return jamSelectedChannel; }
void nrf24JammerSetSelectedChannel(int ch) {
    if (ch < 0) ch = 0;
    if (ch > 125) ch = 125;
    jamSelectedChannel = ch;

    Serial.print("[NRF24] SetSelectedChannel: ");
    Serial.println(ch);

    if (nrf24JammerActive && jamSelectMode) {
        Serial.println("[NRF24] Reiniciando jammer no novo canal...");
        jamChannel = ch;
        jamChannelPackets = 0;
        radio.stopConstCarrier();
        delayMicroseconds(100);
        radio.startConstCarrier(RF24_PA_MAX, ch);
        digitalWrite(NRF_CE, HIGH);  // FORÇAR CE HIGH novamente!
        Serial.println("[NRF24] Jammer reiniciado no novo canal.");
    }
}

uint8_t nrf24GetDeviceCount() { return nrfDeviceCount; }
NRFDevice* nrf24GetDevice(uint8_t index) {
    if (index < nrfDeviceCount) return &nrfDevices[index];
    return nullptr;
}
bool nrf24IsJammerActive() { return nrf24JammerActive; }
bool nrf24IsAvailable() { return radio.isChipConnected(); }
