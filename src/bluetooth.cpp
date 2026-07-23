#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BluetoothSerial.h>
#include "config.h"

BluetoothSerial SerialBT;
BLEScan* bleScan = nullptr;
bool bleScanning = false;

class MyCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice adv) {
        if (btDeviceCount < MAX_BT_DEVICES) {
            strncpy(btDevices[btDeviceCount].name, adv.getName().c_str(), 31);
            btDevices[btDeviceCount].name[31] = '\0';
            std::string addr = adv.getAddress().toString();
            sscanf(addr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
                   &btDevices[btDeviceCount].address[0],
                   &btDevices[btDeviceCount].address[1],
                   &btDevices[btDeviceCount].address[2],
                   &btDevices[btDeviceCount].address[3],
                   &btDevices[btDeviceCount].address[4],
                   &btDevices[btDeviceCount].address[5]);
            btDevices[btDeviceCount].rssi = adv.getRSSI();
            btDeviceCount++;
        }
    }
};

bool bluetoothInit() {
    BLEDevice::init("CrazyCat");
    bleScan = BLEDevice::getScan();
    bleScan->setAdvertisedDeviceCallbacks(new MyCallbacks());
    bleScan->setActiveScan(true);
    bleScan->setInterval(100);
    bleScan->setWindow(99);

    // Inicia Bluetooth Serial para transferencia de handshake
    SerialBT.begin("CrazyCat-BT");
    Serial.println("[BT] Serial ready: CrazyCat-BT");
    return true;
}

void startBTScan() {
    btDeviceCount = 0;
    bleScanning = true;
    bleScan->start(5, nullptr, false);
    bleScanning = false;
}

uint8_t getBTDeviceCount() { return btDeviceCount; }

BTDevice* getBTDevice(uint8_t index) {
    if (index < btDeviceCount) return &btDevices[index];
    return nullptr;
}

void startBTJammer(uint8_t deviceIndex) {
    if (deviceIndex >= btDeviceCount) return;
    btJammerActive = true;
    while (btJammerActive) {
        for (int ch = 0; ch < 40; ch++) {
            delay(5);
        }
        yield();
    }
}

void stopBTJammer() { btJammerActive = false; }

void disconnectBTDevice(uint8_t deviceIndex) {
    // Spoof disconnect
}
