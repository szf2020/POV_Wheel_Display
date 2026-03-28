#include "config.h"
#include "network.h"
#include "esp_now_sync.h"
#include <WiFi.h>

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <ICM45605.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <BH1750.h>
#include <vector>
#include <ESPAsyncWebServer.h>
#include "driver/gpio.h"

// Экспортируем сервер из network.cpp для добавления нового эндпоинта
extern AsyncWebServer server;

// --- ИНИЦИАЛИЗАЦИЯ ГЛОБАЛЬНЫХ ПЕРЕМЕННЫХ ---
volatile uint8_t global_brightness = 20; 

RTC_DATA_ATTR uint8_t min_brightness = 10;
RTC_DATA_ATTR uint8_t max_brightness = 50;
RTC_DATA_ATTR volatile int global_angle_offset = 0;

uint8_t* frameBuffer = nullptr; 

// Глобальные переменные для поддержки GIF анимаций
uint32_t currentFrameIndex = 0;
uint32_t totalFrames = 1;
uint16_t frameDelay = 100;
uint32_t lastFrameSwitchTime = 0;

volatile bool newFrameReady = false;
CRGB leds[NUM_LEDS];

bool slideshowActive = false;
uint16_t slideInterval = 5000;
uint32_t lastSlideTime = 0;
int currentSlideIndex = 0;
std::vector<String> savedFiles;

volatile uint32_t last_magnet_time = 0;   
volatile uint32_t revolution_time = 0;    
volatile bool magnet_triggered = false; 
volatile bool is_rendering_rotation = false; // Флаг строгого рендеринга 1 оборота
volatile uint32_t last_power_toggle_time = 0; // Защита от EMI глитчей

ICM456xx imu(SPI, PIN_CS);
BH1750 lightMeter;

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ (Замените старые переменные времени этими) ---
volatile bool hall_event = false;
volatile uint32_t last_hall_time = 0;
volatile uint32_t rotation_period = 0;
int last_drawn_sector = -1; 
RTC_DATA_ATTR bool force_stop_display = false;
volatile uint32_t last_web_activity_time = 0; // Добавлено: отслеживание активности в Web UI

// --- ПЕРЕМЕННЫЕ BQ25798 ---
volatile bool bq_interrupt_flag = false;

volatile uint32_t last_dcdc_off_time = 0;
static bool peripherals_active = true;

volatile bool wakeup_event = false;

// Хелперы для I2C чтения BQ25798
uint8_t readBQ8(uint8_t reg) {
    Wire.beginTransmission(BQ25792_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint16_t)BQ25792_ADDR, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    return 0;
}

int16_t readBQ16(uint8_t reg) {
    Wire.beginTransmission(BQ25792_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint16_t)BQ25792_ADDR, (uint8_t)2);
    if (Wire.available() >= 2) {
        return (Wire.read() << 8) | Wire.read();
    }
    return 0;
}

void IRAM_ATTR wakeupInterruptHandler() {
    // Реагируем только если периферия спит
    if (!peripherals_active) {
        wakeup_event = true;
    }
}

void IRAM_ATTR magnetInterruptHandler() {
    uint32_t now_ms = millis();
    uint32_t now = micros();
    
    // Игнорируем прерывания полсекунды после отключения питания (защита от глитча)
    if (!peripherals_active && (now_ms - last_dcdc_off_time < 500)) {
        return; 
    }

    // 50ms аппаратный антидребезг
    if (now - last_hall_time > 50000) { 
        rotation_period = now - last_hall_time;
        last_hall_time = now;
        hall_event = true;
    }
}

// Обработчик прерывания BQ25798 (INTn на GPIO 21)
void IRAM_ATTR bqInterruptHandler() {
    bq_interrupt_flag = true;
}

void initBQ25792() {
    // DIS_LDO
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x12); // REG12_Charger_Control_3 Register
    Wire.write(0x04); // Значение 0x04: DIS_LDO = 1
    Wire.endTransmission(); 

   // DIS REG08_Precharge_Control
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x08); // REG08_Precharge_Control Register
    Wire.write(0x19); // Precharge 1000mA
    Wire.endTransmission(); 

    // 1. Перезапуск цикла заряда: сначала отключаем зарядку (ИСПРАВЛЕНО: 0x82 вместо 0x1A, чтобы не включать HIZ)
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x0F); // Регистр Charger Control 0
    Wire.write(0x82); // Значение 0x82: EN_CHG = 0, EN_HIZ = 0
    Wire.endTransmission(); 
    delay(200);       // Пауза 200 мс для стабилизации

    // 2. Включаем зарядку обратно (ИСПРАВЛЕНО: 0xA2 вместо 0x3A, чтобы не включать HIZ)
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x0F); 
    Wire.write(0xA2); // Значение 0xA2: EN_CHG = 1, EN_HIZ = 0
    Wire.endTransmission();

    // 3. Отключаем сторожевой таймер (Watchdog)
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x10); // Регистр Charger Control 1
    Wire.write(0x00); // Значение 0x00: отключает Watchdog. Иначе чип сбросит настройки через ~40 сек.
    Wire.endTransmission();

    // 4. Настройка минимального системного напряжения (VSYSMIN) - ИСПРАВЛЕНО ДЛЯ 1S БАТАРЕИ
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x00); // Регистр Minimal System Voltage (VSYSMIN)
    Wire.write(0x04); // Значение 0x04: 2.5V + (4 * 0.25V) = 3.5V. (0x06 было 4.0V, что блокировало зарядку 1S)
    Wire.endTransmission();

    // Установка лимита напряжения заряда (VBATREG) на 4.2V - Перенесено сюда для надежности
    Wire.beginTransmission(BQ25792_ADDR);
    Wire.write(0x01); Wire.write(0x01); // 4.2V MSB
    Wire.endTransmission();
    Wire.beginTransmission(BQ25792_ADDR);
    Wire.write(0x02); Wire.write(0xA4); // 4.2V LSB (0x01A4 = 420 * 10mV = 4200mV)
    Wire.endTransmission();

    // 5. Установка лимита входного тока (IINDPM) на 1.5 А
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x06); // Регистр Input Current Limit (Старший байт - MSB)
    Wire.write(0x00); // Значение 0x00
    Wire.endTransmission();
    
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x07); // Регистр Input Current Limit (Младший байт - LSB)
    Wire.write(0x96); // Значение 0x96
    Wire.endTransmission();
    // Итоговое значение: 0x0096 (в десятичной системе = 150). Шаг 10 мА -> 1500 мА.

    // VINDPM = 4.3V (Input voltage regulation point)
    Wire.beginTransmission(BQ25792_ADDR);
    Wire.write(0x05);        // VINDPM register MSB
    Wire.write(0x24);
    Wire.endTransmission();
    
    // 6. Установка лимита тока зарядки (ICHG) на 3.0 А
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x03); // Регистр Charge Current Limit (Старший байт - MSB)
    Wire.write(0x01); // Значение 0x01
    Wire.endTransmission();
    
    Wire.beginTransmission(BQ25792_ADDR); 
    Wire.write(0x04); // Регистр Charge Current Limit (Младший байт - LSB)
    Wire.write(0x2C); // Значение 0x2C
    Wire.endTransmission();
    // Итоговое значение: 0x012C (в десятичной системе = 300). Шаг 10 мА -> 3000 мА.
}

// Upgraded architecture (4 LED arms, 90-degree spacing)
void drawSector(int current_sector) {
    if (frameBuffer == nullptr || !newFrameReady) return;

    // Смещение для текущего кадра анимации (если это статика, index = 0)
    uint32_t anim_offset = currentFrameIndex * FRAME_SIZE;

    for (int ray = 0; ray < 4; ray++) {
        // 120 sectors / 4 arms = 30 sectors offset
        int sector_to_draw = (current_sector + ray * 30) % 120; 
        int buffer_offset = anim_offset + sector_to_draw * 38 * 3; 

        for (int i = 0; i < 38; i++) {
            uint8_t r = frameBuffer[buffer_offset + i * 3];
            uint8_t g = frameBuffer[buffer_offset + i * 3 + 1];
            uint8_t b = frameBuffer[buffer_offset + i * 3 + 2];
            leds[ray * 76 + i] = CRGB(r, g, b);
            leds[ray * 76 + 75 - i] = CRGB(r, g, b); 
        }
    }
    FastLED.show();
}

void updateFileList() {
    savedFiles.clear();
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while(file) {
        String fn = file.name();
        if (fn.endsWith(".bin")) savedFiles.push_back(fn);
        file = root.openNextFile();
    }
}

void setup() {
    Serial.begin(115200);
    //delay(2000);

    // Снимаем глобальную блокировку пинов после сна
    gpio_deep_sleep_hold_dis();
    // Размораживаем конкретно наш пин 21
    gpio_hold_dis((gpio_num_t)21);
    
    // Возвращаем пин 21 в обычное состояние для активной работы
    gpio_pullup_dis((gpio_num_t)21);

    pinMode(PIN_EN_DCDC, OUTPUT); pinMode(PIN_EN_LEVEL_SHIFT, OUTPUT);
    pinMode(PIN_WAKEUP, INPUT_PULLUP); pinMode(PIN_BUTTON, INPUT_PULLUP); pinMode(PIN_COLOR_INT, INPUT_PULLUP);
    
    // Настраиваем прерывание BQ25798 на GPIO 21
    pinMode(21, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(21), bqInterruptHandler, FALLING);
    
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

    // Проверяем подключен ли адаптер сразу после пробуждения
    bool is_adapter_connected = false;
    uint8_t stat = readBQ8(0x1B);
    is_adapter_connected = ((stat >> 5) & 0x07) != 0;

    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    Serial.printf("\n--- BOOT --- Wakeup reason: %d\n", wakeup_reason);
    
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
        Serial.println("ПРОСНУЛИСЬ ОТ PIN_WAKEUP (IO5) - Глитч подтвержден!");
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
        Serial.println("ПРОСНУЛИСЬ ОТ BQ25798 (GPIO 21) - Залипло прерывание зарядки!");
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        Serial.println("ПОЛНАЯ ПЕРЕЗАГРУЗКА (Возможно краш, WDT или Brownout)");
    }
    
    // Если пробуждение не от кнопки (EXT0) и не от зарядки (EXT1) и адаптер не подключен
    if (wakeup_reason != ESP_SLEEP_WAKEUP_EXT0 && wakeup_reason != ESP_SLEEP_WAKEUP_EXT1 && !is_adapter_connected) {
        Serial.println("Not an EXT0/EXT1 wakeup and no charger. Going to Deep Sleep...");
        digitalWrite(PIN_EN_DCDC, LOW); 
        digitalWrite(PIN_EN_LEVEL_SHIFT, LOW);

        Wire.beginTransmission(0x23);
        Wire.write(0x00); 
        Wire.endTransmission();

        SPI.begin();
        pinMode(PIN_CS, OUTPUT);
        digitalWrite(PIN_CS, HIGH); delay(5);
        digitalWrite(PIN_CS, LOW);
        SPI.transfer(0x4E); 
        SPI.transfer(0x01); 
        digitalWrite(PIN_CS, HIGH);

        esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_WAKEUP, 0); 
        esp_sleep_enable_ext1_wakeup(1ULL << 21, ESP_EXT1_WAKEUP_ANY_LOW); // Просыпаться от зарядного устройства
        esp_deep_sleep_start();
    }

    Serial.println("Wakeup Detected! Initializing...");
    digitalWrite(PIN_EN_DCDC, HIGH); 
    digitalWrite(PIN_EN_LEVEL_SHIFT, HIGH);

    // Если проснулись от зарядки или она была подключена, инициируем настройку BQ
    if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1 || is_adapter_connected) {
        bq_interrupt_flag = true;
    }

    if (psramFound()) {
        frameBuffer = (uint8_t*)ps_malloc(FRAME_SIZE);
        if (frameBuffer) memset(frameBuffer, 0, FRAME_SIZE);
    }
    LittleFS.begin(true);

    initBQ25792();
    lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire);

    setupNetwork();

    // Эндпоинт для телеметрии батареи с добавленным полным дебагом конфигурации ЗУ
    server.on("/battery", HTTP_GET, [](AsyncWebServerRequest *request){
        // Включаем ADC (на случай если он был отключен)
        Wire.beginTransmission(BQ25792_ADDR);
        Wire.write(0x2E);
        Wire.write(0x80); // ADC_EN = 1
        Wire.endTransmission();
        
        int16_t vbus = readBQ16(0x35); // VBUS_ADC (0x35)
        int16_t ibus = readBQ16(0x31); // IBUS_ADC (0x31)
        int16_t vbat = readBQ16(0x3B); // VBAT_ADC (0x3B)
        int16_t ibat = readBQ16(0x33); // IBAT_ADC (0x33)
        
        // --- ДЕТАЛЬНЫЙ ДЕБАГ РЕГИСТРОВ ЗАРЯДА ---
        uint8_t reg00 = readBQ8(0x00); // VSYSMIN
        uint8_t reg0F = readBQ8(0x0F); // Charger Control 0
        uint8_t reg1B = readBQ8(0x1B); // Charger Status 0
        uint8_t reg1C = readBQ8(0x1C); // Charger Status 1
        uint8_t reg20 = readBQ8(0x20); // Fault Status 0
        uint8_t reg21 = readBQ8(0x21); // Fault Status 1

        uint16_t chg_v_limit = readBQ16(0x01); // Charge Voltage Limit
        uint16_t chg_i_limit = readBQ16(0x03); // Charge Current Limit
        uint16_t in_i_limit = readBQ16(0x06);  // Input Current Limit

        bool vbus_present = ((reg1B >> 5) & 0x07) != 0;
        bool charge_enabled = (reg0F >> 5) & 0x01;
        uint8_t chg_state = (reg1B >> 3) & 0x03;
        bool ilim_active = (reg1C >> 6) & 0x01;

        Serial.println("\n=== BQ25798 CHARGER DEBUG ===");
        Serial.printf("Charge current register (0x03-0x04): 0x%04X (%d mA)\n", chg_i_limit, chg_i_limit * 10);
        Serial.printf("Charge voltage register (0x01-0x02): 0x%04X (%d mV)\n", chg_v_limit, chg_v_limit * 10);
        Serial.printf("Input current limit (0x06-0x07): 0x%04X (%d mA)\n", in_i_limit, in_i_limit * 10);
        Serial.printf("VSYSMIN register (0x00): 0x%02X (%d mV)\n", reg00, 2500 + (reg00 & 0x3F) * 250);
        Serial.printf("Charger control register (0x0F): 0x%02X\n", reg0F);
        Serial.printf("Charger status register (0x1B): 0x%02X\n", reg1B);
        Serial.printf("Power status register (0x1C): 0x%02X\n", reg1C);
        Serial.printf("Fault status register (0x20-0x21): 0x%02X 0x%02X\n", reg20, reg21);

        Serial.println("\n--- DECODED MEANINGS ---");
        Serial.printf("Adapter present: %s\n", vbus_present ? "YES" : "NO");
        Serial.printf("Charging enabled: %s\n", charge_enabled ? "YES" : "NO");
        
        String state_str = "Unknown";
        switch(chg_state) {
            case 0: state_str = "Not Charging"; break;
            case 1: state_str = "Trickle / Pre-charge"; break;
            case 2: state_str = "Fast Charging"; break;
            case 3: state_str = "Taper Charge"; break;
        }
        Serial.printf("Charging state: %s\n", state_str.c_str());
        
        Serial.printf("Input limit active (IINDPM): %s\n", ilim_active ? "YES" : "NO");
        Serial.printf("Fault flags: ");
        if(reg20 == 0 && reg21 == 0) Serial.print("NONE\n");
        else Serial.printf("0x%02X 0x%02X\n", reg20, reg21);
        Serial.println("===============================\n");
        // ----------------------------------------

        bool connected = vbus_present;

        String json = "{";
        json += "\"vbat\":" + String(vbat) + ","; 
        json += "\"ibat\":" + String(ibat) + ","; 
        json += "\"vbus\":" + String(vbus) + ",";
        json += "\"ibus\":" + String(ibus) + ",";
        json += "\"connected\":" + String(connected ? "true" : "false");
        json += "}";
        request->send(200, "application/json", json);
    });

    initSync();
    updateFileList(); 

    String last_file = prefs.getString("last_file", "");
    if (last_file != "" && LittleFS.exists("/" + last_file)) {
        Serial.println("Автозагрузка последнего изображения: " + last_file);
        loadFrameFromFile("/" + last_file);
    }

    FastLED.addLeds<SK9822, PIN_LED_DATA, PIN_LED_CLK, BGR, DATA_RATE_MHZ(24)>(leds, NUM_LEDS);
    FastLED.clear(true); FastLED.setBrightness((global_brightness * 255) / 100);

    attachInterrupt(digitalPinToInterrupt(PIN_COLOR_INT), magnetInterruptHandler, FALLING);

    attachInterrupt(digitalPinToInterrupt(PIN_WAKEUP), wakeupInterruptHandler, FALLING);
    
    if (isSlaveMode) { fill_solid(leds, NUM_LEDS, CRGB::Blue); } 
    else { fill_solid(leds, NUM_LEDS, CRGB::Green); }
    FastLED.show(); delay(200); FastLED.clear(true); FastLED.show();
}

void loop() {
    loopNetwork();

    uint32_t now_ms = millis();
    uint32_t now_us = micros();

    static bool is_rendering_rotation = false; // Флаг для рендеринга строго одного оборота
    
    // --- БЕЗОПАСНОЕ ЧТЕНИЕ ПРЕРЫВАНИЙ ---
    // Защита от race conditions: читаем volatile переменные с отключенными прерываниями,
    // чтобы ISR не изменил их прямо во время математических расчетов.
    uint32_t safe_last_hall_time;
    uint32_t safe_rotation_period;
    noInterrupts();
    safe_last_hall_time = last_hall_time;
    safe_rotation_period = rotation_period;
    interrupts();

    // --- Обработка прерывания зарядного устройства BQ25798 ---
   if (bq_interrupt_flag) {
        bq_interrupt_flag = false;
        uint8_t fault0 = readBQ8(0x20);
        uint8_t fault1 = readBQ8(0x21);
        uint8_t stat = readBQ8(0x1B);
        bool vbus_present = ((stat >> 5) & 0x07) != 0;
        uint8_t charge_state = (stat >> 3) & 0x03;
        Serial.println("=== BQ25798 INT ===");
        Serial.printf("VBUS present: %s\n", vbus_present ? "YES" : "NO");
        Serial.printf("Charge state: %d\n", charge_state);
        Serial.printf("Fault: 0x%02X 0x%02X\n", fault0, fault1);
    }

    // --- 1. Обработка прерывания Холла (Event Handler) ---
    if (hall_event) {
        hall_event = false;
        if (!force_stop_display) {
            if (!peripherals_active) {
                digitalWrite(PIN_EN_DCDC, HIGH);
                digitalWrite(PIN_EN_LEVEL_SHIFT, HIGH);
                peripherals_active = true;
                is_rendering_rotation = false; 
            } else {
                is_rendering_rotation = true; 
            }
        }
    }

    // --- 1.5 Пробуждение от PIN_WAKEUP ---
    if (wakeup_event) {
        wakeup_event = false;
        if (!peripherals_active && !force_stop_display) {
            Serial.println("Rotation detected via PIN_WAKEUP - restarting DCDC");
            digitalWrite(PIN_EN_DCDC, HIGH);
            digitalWrite(PIN_EN_LEVEL_SHIFT, HIGH);
            peripherals_active = true;
            is_rendering_rotation = false;
            last_hall_time = now_us; 
        }
    }

    // Вычисляем реальное время с последнего прохода магнита (с защитой от переполнения)
    uint32_t time_since_magnet_us = 0;
    if (safe_last_hall_time > 0) {
        if (now_us >= safe_last_hall_time) {
            time_since_magnet_us = now_us - safe_last_hall_time;
        } else {
            time_since_magnet_us = (0xFFFFFFFF - safe_last_hall_time) + now_us + 1;
        }
    }

    // --- 2. Логика остановки (Таймаут 1 секунда) ---
    if (peripherals_active && time_since_magnet_us > 1000000 && !force_stop_display) {
        Serial.println("Остановка рендеринга (Таймаут 1с). Отключение питания LED");
        FastLED.clear(true);
        FastLED.show();
        digitalWrite(PIN_EN_LEVEL_SHIFT, LOW);
        digitalWrite(PIN_EN_DCDC, LOW);
        last_dcdc_off_time = millis(); // Запоминаем время отключения
        peripherals_active = false;
        is_rendering_rotation = false;
    }

    // --- 3. Принудительная остановка из Web UI (Stop Display) ---
    if (force_stop_display && peripherals_active) {
        Serial.println("Web Interface: Принудительная остановка рендеринга");
        FastLED.clear(true);
        FastLED.show();
        digitalWrite(PIN_EN_LEVEL_SHIFT, LOW);
        digitalWrite(PIN_EN_DCDC, LOW);
        last_dcdc_off_time = millis(); // Запоминаем время отключения
        peripherals_active = false;
        is_rendering_rotation = false;
    }

    // --- 4. Deep Sleep (Таймаут 1 минута) ---
    uint32_t time_since_web_activity_ms = now_ms - last_web_activity_time; // Вычисляем время бездействия в веб-интерфейсе
    
    if (!peripherals_active && time_since_magnet_us > 60000000 && time_since_web_activity_ms > 60000) { // 1 минута в микросекундах и 1 минута в веб-интерфейсе
        bool is_adapter_connected = ((readBQ8(0x1B) >> 5) & 0x07) != 0;
        
        if (is_adapter_connected) {
            last_hall_time = now_us;
            Serial.println("External adapter connected. Preventing Deep Sleep.");
        } else {
        Serial.println("Таймаут. Подготовка к Deep Sleep...");
            
            // 1. Отключаем АЦП BQ25798 (чтобы он не дернул INT во сне по окончании замера)
            Wire.beginTransmission(BQ25792_ADDR);
            Wire.write(0x2E);
            Wire.write(0x00); 
            Wire.endTransmission();

            // 2. Усыпляем ваши датчики
            Wire.beginTransmission(0x23); Wire.write(0x00); Wire.endTransmission();
            digitalWrite(PIN_CS, LOW); SPI.transfer(0x4E); SPI.transfer(0x01); digitalWrite(PIN_CS, HIGH);
            
            // ВАЖНО: Даем напряжению стабилизироваться после отключения датчиков
            delay(50); 
            
            // 3. ТОЛЬКО ТЕПЕРЬ очищаем прерывания BQ25798! 
            readBQ8(0x20); readBQ8(0x21); readBQ8(0x1B); readBQ8(0x1C);
            
            // 4. Глушим датчики Холла
            detachInterrupt(digitalPinToInterrupt(PIN_COLOR_INT));
            detachInterrupt(digitalPinToInterrupt(PIN_WAKEUP));

            // 5. Настраиваем пробуждение по PIN_WAKEUP
            if (digitalRead(PIN_WAKEUP) == LOW) {
                esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_WAKEUP, 1); 
            } else {
                esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_WAKEUP, 0); 
            }
            
            // 6. БОРЬБА СО 100кОм ПОДТЯЖКОЙ НА BQ INT
            // Жестко запрещаем ESP32 тянуть пин к земле и включаем подтяжку к питанию
            gpio_pulldown_dis((gpio_num_t)21);
            gpio_pullup_en((gpio_num_t)21);
            
            // ВАЖНО: Замораживаем текущее состояние пина (Hold), 
            // чтобы сон не сбросил наши настройки pull-up!
            gpio_hold_en((gpio_num_t)21);
            gpio_deep_sleep_hold_en(); // Применяем удержание пинов в Deep Sleep
            
            // Проверка "на дурака": если пин всё ещё в нуле - мы где-то не дочитали регистр BQ
            if (digitalRead(21) == LOW) {
                Serial.println("ОШИБКА: Пин BQ25798 INT все еще LOW. Сон будет прерван!");
            }
            
            esp_sleep_enable_ext1_wakeup(1ULL << 21, ESP_EXT1_WAKEUP_ANY_LOW);
            
            Serial.println("Ушел в сон.");
            Serial.flush();
            delay(10); 
            
            esp_deep_sleep_start();
        }
    }

    // --- 5. Авто-яркость ---
    if (peripherals_active) {
        static uint32_t last_lux_time = 0;
        if (now_ms - last_lux_time > 200) { 
            float lux = lightMeter.readLightLevel();
            if (lux >= 0) {
                uint8_t target_b = map((long)lux, 0, 1000, min_brightness, max_brightness);
                target_b = constrain(target_b, min_brightness, max_brightness);
                if (target_b > 50) target_b = 50; 
                
                if (global_brightness != target_b) {
                    global_brightness = target_b;
                    FastLED.setBrightness((global_brightness * 255) / 100);
                }
            }
            last_lux_time = now_ms;
        }
    }

    // --- 6. Мониторинг BMS (Непрерывный мониторинг, даже при остановленном рендере) ---
    static uint32_t last_bms_check_time = 0;
    if (now_ms - last_bms_check_time > 1000) { 
        last_bms_check_time = now_ms;
        
        // Включаем ADC для уверенности, что данные свежие
        Wire.beginTransmission(BQ25792_ADDR);
        Wire.write(0x2E);
        Wire.write(0x80); // ADC_EN = 1
        Wire.endTransmission();
        
        int16_t vbat_raw = readBQ16(0x3B);
        static uint32_t last_bms_recovery_time = 0;
        
        // Триггер: Напряжение батареи застряло в диапазоне 1.1V - 1.3V
        if (vbat_raw >= 1000 && vbat_raw <= 1400) {
            // Lockout: 30 секунд между запусками цикла восстановления
            if (now_ms - last_bms_recovery_time > 30000 || last_bms_recovery_time == 0) {
                Serial.println("BMS Latch Detected (VBAT 1.0-1.4V)! Initiating Recovery Sequence...");
                
                // STEP 1 — Disable charging (ИСПРАВЛЕНО: 0x82 вместо 0x1A)
                Wire.beginTransmission(BQ25792_ADDR);
                Wire.write(0x0F);
                Wire.write(0x82);
                Wire.endTransmission();

                // STEP 2 — Turn off power rails
                digitalWrite(PIN_EN_DCDC, LOW);
                digitalWrite(PIN_EN_LEVEL_SHIFT, LOW);

                // STEP 3 — Wait 5 seconds
                delay(5000);

                // STEP 4 — Re-enable rails
                digitalWrite(PIN_EN_DCDC, HIGH);
                digitalWrite(PIN_EN_LEVEL_SHIFT, HIGH);

                // STEP 5 — Re-enable charging (ИСПРАВЛЕНО: 0xA2 вместо 0x3A)
                Wire.beginTransmission(BQ25792_ADDR);
                Wire.write(0x0F);
                Wire.write(0xA2);
                Wire.endTransmission();
                
                last_bms_recovery_time = millis(); // Обновляем таймер после завершения
                Serial.println("BMS Recovery Sequence Complete.");
            }
        }
    }

    // --- 7. Слайд-шоу ---
    if (!isSlaveMode && slideshowActive && savedFiles.size() > 0 && now_ms - lastSlideTime > slideInterval) {
        lastSlideTime = now_ms;
        String currentFile = savedFiles[currentSlideIndex];
        loadFrameFromFile("/" + currentFile);
        currentSlideIndex++;
        if (currentSlideIndex >= savedFiles.size()) currentSlideIndex = 0;
    }

    // --- 8. Переключение кадров анимации (GIF таймер) ---
    if (totalFrames > 1 && peripherals_active && !force_stop_display) {
        if (now_ms - lastFrameSwitchTime >= frameDelay) {
            lastFrameSwitchTime = now_ms;
            currentFrameIndex++;
            if (currentFrameIndex >= totalFrames) currentFrameIndex = 0;
        }
    }

// --- 9. Синхронизированный рендеринг ---
    if (newFrameReady && peripherals_active && safe_last_hall_time > 0 && !force_stop_display) {
        
        // Рендерим только если есть разрешение на этот оборот
        if (is_rendering_rotation && safe_rotation_period > 0) {
            
            // ОБНОВЛЯЕМ время прямо перед рендером! 
            // Это полностью исключает влияние задержек (джиттера) от I2C или WebUI выше по коду.
            uint32_t render_now_us = micros();
            uint32_t precise_time_since_magnet_us = 0;
            
            if (render_now_us >= safe_last_hall_time) {
                precise_time_since_magnet_us = render_now_us - safe_last_hall_time;
            } else {
                precise_time_since_magnet_us = (0xFFFFFFFF - safe_last_hall_time) + render_now_us + 1;
            }
            
            int base_sector = (precise_time_since_magnet_us * 120) / safe_rotation_period;
            
            // Если колесо чуть замедлилось (время превысило ожидаемый период),
            // удерживаем последний сектор горящим в ожидании датчика Холла.
            if (base_sector >= 120) {
                base_sector = 119; 
            }

            int offset_sectors = (global_angle_offset * 120) / 360;
            int current_sector = (base_sector + offset_sectors) % 120;

            if (current_sector != last_drawn_sector) {
                drawSector(current_sector);
                last_drawn_sector = current_sector;
            }
        }
    }
}