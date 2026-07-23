#ifndef WIFI_HANDSHAKE_H
#define WIFI_HANDSHAKE_H

#include <Arduino.h>

void startHandshakeCapture();
void stopHandshakeCapture();
bool isHandshakeCapturing();

const char* getHandshakeStatus();
uint8_t getHandshakeMessageCount();
bool isHandshakeComplete();
void clearHandshakeBuffer();

#endif
