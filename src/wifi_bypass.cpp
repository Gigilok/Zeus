// ============================================================
// wifi_bypass.cpp
// BYPASS do ieee80211_raw_frame_sanity_check
// Assinatura correta para Arduino-ESP32 v2.0+ (ESP-IDF v4.4+)
// 4 argumentos: ifx, buffer, len, auto_seq
// ============================================================
#include <Arduino.h>
#include <stdbool.h>

// Assinatura REAL da funcao na libnet80211.a do Arduino-ESP32
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
