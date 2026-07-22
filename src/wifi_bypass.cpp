// ============================================================
// wifi_bypass.cpp - BYPASS do ieee80211_raw_frame_sanity_check
// Funciona no Arduino-ESP32 v2.x (ESP-IDF v4.4)
// ============================================================
#include <Arduino.h>

extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg1, int32_t arg2, int32_t arg3) {
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0;
}
