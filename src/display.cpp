#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

bool displayInit() {
    Wire.begin(OLED_SDA, OLED_SCK);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        return false;
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.display();
    return true;
}

void setBrightness(uint8_t brightness) {
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(brightness);
}

void clearDisplay() {
    display.clearDisplay();
}

void updateDisplay() {
    display.display();
}

void drawText(int x, int y, const char* text, uint8_t size) {
    display.setTextSize(size);
    display.setCursor(x, y);
    display.print(text);
}

void drawCenteredText(int y, const char* text, uint8_t size) {
    display.setTextSize(size);
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, y);
    display.print(text);
}

void drawMenuHeader(const char* title) {
    display.fillRect(0, 0, SCREEN_WIDTH, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(2, 1);
    display.print(title);
    display.setTextColor(SSD1306_WHITE);
}

void drawMenuItem(int y, const char* text, bool selected) {
    if (selected) {
        display.fillRect(0, y, SCREEN_WIDTH, 10, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
    } else {
        display.setTextColor(SSD1306_WHITE);
    }
    display.setTextSize(1);
    display.setCursor(4, y + 1);
    display.print(text);
    if (selected) {
        display.setTextColor(SSD1306_WHITE);
    }
}

void drawProgressBar(int x, int y, int width, int height, int percent) {
    display.drawRect(x, y, width, height, SSD1306_WHITE);
    int fillWidth = (width - 2) * percent / 100;
    if (fillWidth > 0) {
        display.fillRect(x + 1, y + 1, fillWidth, height - 2, SSD1306_WHITE);
    }
}

void drawStatusBar(const char* status) {
    display.fillRect(0, SCREEN_HEIGHT - 8, SCREEN_WIDTH, 8, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(1);
    display.setCursor(2, SCREEN_HEIGHT - 7);
    display.print(status);
    display.setTextColor(SSD1306_WHITE);
}

void showSplashScreen() {
    display.clearDisplay();
    drawCenteredText(10, "CRAZY", 2);
    drawCenteredText(28, "CAT", 2);
    display.setTextSize(1);
    drawCenteredText(48, "v3.1", 1);
    drawCenteredText(56, "by ESP32", 1);
    display.display();
    delay(2000);
}

void showMessage(const char* title, const char* message) {
    display.clearDisplay();
    drawMenuHeader(title);
    display.setTextSize(1);
    display.setCursor(0, 14);
    display.print(message);
    display.display();
}

void showLoading(const char* text, int percent) {
    display.clearDisplay();
    drawCenteredText(20, text, 1);
    drawProgressBar(14, 40, 100, 10, percent);
    display.display();
}

Adafruit_SSD1306& getDisplay() {
    return display;
}
