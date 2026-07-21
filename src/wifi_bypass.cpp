// ============================================================
// wifi_bypass.cpp
// BYPASS do ieee80211_raw_frame_sanity_check
// 
// Método 1: --wrap do linker (tenta interceptar chamadas)
// Método 2: weak symbol (sobrescreve se a lib não exportar)
// Método 3: Ambos juntos para máxima compatibilidade
// ============================================================
#include <Arduino.h>
#include <stdbool.h>

// === MÉTODO 1: --wrap ===
extern "C" int __wrap_ieee80211_raw_frame_sanity_check(
    int ifx,
    const void *buffer,
    int len,
    bool auto_seq
) {
    (void)ifx;
    (void)buffer;
    (void)len;
    (void)auto_seq;
    return 0;
}

extern "C" int __real_ieee80211_raw_frame_sanity_check(
    int ifx,
    const void *buffer,
    int len,
    bool auto_seq
);

// === MÉTODO 2: weak symbol ===
// Se a função for exportada como weak pela lib, esta prevalece
extern "C" int ieee80211_raw_frame_sanity_check(
    int ifx,
    const void *buffer,
    int len,
    bool auto_seq
) __attribute__((weak));

extern "C" int ieee80211_raw_frame_sanity_check(
    int ifx,
    const void *buffer,
    int len,
    bool auto_seq
) {
    (void)ifx;
    (void)buffer;
    (void)len;
    (void)auto_seq;
    return 0;
}
