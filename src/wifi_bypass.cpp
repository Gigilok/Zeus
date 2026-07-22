// ============================================================
// wifi_bypass.cpp - BYPASS do ieee80211_raw_frame_sanity_check
//
// Usa DUAS abordagens para maxima compatibilidade:
// 1. --wrap (mais confiavel para libs pre-compiladas)
// 2. -zmuldefs (fallback, usado pelo ESP32-Marauder)
//
// No platformio.ini adicionar:
//   -Wl,--wrap=ieee80211_raw_frame_sanity_check
//   -Wl,-z,muldefs
// ============================================================
#include <Arduino.h>
#include <stdbool.h>

// Abordagem 1: --wrap (linker substitui TODAS as chamadas)
extern "C" int __wrap_ieee80211_raw_frame_sanity_check(int32_t arg1, int32_t arg2, int32_t arg3) {
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0;
}

// Abordagem 2: -zmuldefs (fallback, sobrescreve definicao da lib)
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg1, int32_t arg2, int32_t arg3) {
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0;
}
