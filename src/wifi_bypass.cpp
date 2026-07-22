// ============================================================
// wifi_bypass.cpp
// BYPASS do ieee80211_raw_frame_sanity_check
// Assinatura compativel com Hydra-ESP / risinek: 3 argumentos int32_t
// No Arduino-ESP32 o --wrap pode nao funcionar, mas deixamos como fallback.
// O metodo principal de deauth agora eh BSSID Clone (wifi_attacks.cpp).
// ============================================================
#include <Arduino.h>
#include <stdbool.h>

// Assinatura usada pelo ESP-IDF / libwifi (3 argumentos int32_t)
// Se o linker wrap funcionar, esta funcao substitui a original.
// Se nao funcionar, o BSSID Clone em wifi_attacks.cpp faz o trabalho.
extern "C" int __wrap_ieee80211_raw_frame_sanity_check(
    int32_t arg1,
    int32_t arg2,
    int32_t arg3
) {
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return 0;
}
