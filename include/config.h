#pragma once
#include <Arduino.h>

// Разрешаем прерывания во время вывода на LED. Это критично для стабильности
// энкодера/Холла, чтобы FastLED не блокировал ISR (SK9822 это поддерживает)
#define FASTLED_ALLOW_INTERRUPTS 1
#include <FastLED.h>
#include <Preferences.h>

// --- ПАРАМЕТРЫ ДИСПЛЕЯ ---
#define NUM_LEDS 304              // 4 луча по 76 диодов
#define FRAME_SIZE (120 * 38 * 3) // 13680 байт

// --- ПИНЫ ESP32-S3 ---
#define PIN_CS             41
#define PIN_LED_DATA       11
#define PIN_LED_CLK        12
#define PIN_WAKEUP         5
#define PIN_BUTTON         0

#define PIN_COLOR_INT      4
#define PIN_EN_DCDC        10
#define PIN_EN_LEVEL_SHIFT 9
#define PIN_I2C_SDA        39
#define PIN_I2C_SCL        40
#define BQ25792_ADDR       0x6B

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ (Объявления для всех файлов) ---
extern volatile uint8_t global_brightness;
extern uint8_t min_brightness;
extern uint8_t max_brightness;
extern volatile int global_angle_offset;
extern uint8_t* frameBuffer;
extern volatile bool newFrameReady;
extern CRGB leds[];
extern String hostName;
extern bool isSlaveMode;
extern bool force_stop_display;

// --- ПЕРЕМЕННЫЕ АНИМАЦИИ (GIF) ---
extern uint32_t currentFrameIndex;
extern uint32_t totalFrames;
extern uint16_t frameDelay;
extern uint32_t lastFrameSwitchTime;

// Отслеживание активности Web UI
extern volatile uint32_t last_web_activity_time;

// Настройки альбома
extern bool slideshowActive;
extern uint16_t slideInterval;

extern Preferences prefs;