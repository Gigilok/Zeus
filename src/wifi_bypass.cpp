// ============================================================
// wifi_bypass.cpp
// BYPASS do ieee80211_raw_frame_sanity_check
// Assinatura correta: 4 argumentos
// ============================================================
#include <Arduino.h>
#include <stdbool.h>

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

// Referência para a função original (não usada, mas necessária para link)
extern "C" int __real_ieee80211_raw_frame_sanity_check(
    int ifx,
    const void *buffer,
    int len,
    bool auto_seq
);
