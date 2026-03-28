#include "network.h"
#include "esp_now_sync.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <FastLED.h>
#include <HTTPClient.h>

AsyncWebServer server(80);
Preferences prefs;
File uploadFile;

String hostName;
bool isSlaveMode = false;
uint32_t btnPressTime = 0;
bool btnState = false;

// --- ДАННЫЕ ТОЧКИ ДОСТУПА ТВОЕГО ТЕЛЕФОНА ---
//const char* HOTSPOT_SSID = "🦈 SHARK 🦈"; // Впиши реальное имя
//const char* HOTSPOT_PASS = "sharkshark";            // Впиши пароль
const char* HOTSPOT_SSID = "Bunnies Stan 2.4G"; // Впиши реальное имя
const char* HOTSPOT_PASS = "ValentinaAleksei";            // Впиши пароль

void safeOTAShutdown() {
    FastLED.clear(true); 
    FastLED.show();
}

void loadFrameFromFile(String path) {
    File f = LittleFS.open(path, "r");
    if (!f) return;
    
    // Освобождаем старый буфер из PSRAM, чтобы не было утечек памяти
    if (frameBuffer != nullptr) {
        free(frameBuffer);
        frameBuffer = nullptr;
    }
    
    // Сброс параметров анимации по умолчанию
    totalFrames = 1;
    frameDelay = 100;
    currentFrameIndex = 0;
    lastFrameSwitchTime = millis();
    
    size_t fileSize = f.size();
    
    // Если размер больше одного кадра, проверяем на наличие заголовка анимации ANIM
    if (fileSize > FRAME_SIZE) {
        char magic[4];
        f.read((uint8_t*)magic, 4);
        if (magic[0] == 'A' && magic[1] == 'N' && magic[2] == 'I' && magic[3] == 'M') {
            // Читаем количество кадров и задержку (little-endian)
            f.read((uint8_t*)&totalFrames, 2);
            f.read((uint8_t*)&frameDelay, 2);
            
            size_t dataSize = totalFrames * FRAME_SIZE;
            frameBuffer = (uint8_t*)ps_malloc(dataSize); // Выделяем память под все кадры в PSRAM
            
            if (frameBuffer) {
                f.read(frameBuffer, dataSize);
            } else {
                totalFrames = 0; // Ошибка выделения памяти
                Serial.println("Ошибка PSRAM при загрузке анимации!");
            }
        } else {
            // Файл большого размера, но не анимация (возможно старый кривой файл). Грузим только первый кадр.
            f.seek(0);
            frameBuffer = (uint8_t*)ps_malloc(FRAME_SIZE);
            if (frameBuffer) f.read(frameBuffer, FRAME_SIZE);
        }
    } else {
        // Обычная статичная картинка (ровно 13680 байт)
        frameBuffer = (uint8_t*)ps_malloc(FRAME_SIZE);
        if (frameBuffer) f.read(frameBuffer, FRAME_SIZE);
    }
    
    f.close();
    newFrameReady = true;
}

void sendNotification(String ip) {
    HTTPClient http;
    
    // Замени pov_wheel_belgrade на любое свое уникальное слово, 
    // чтобы никто другой не угадал твой канал.
    http.begin("http://ntfy.sh/pov_wheel_belgrade"); 
    
    // Отправляем кликабельную ссылку
    http.POST("http://" + ip); 
    http.end();
}

void setupNetwork() {
    prefs.begin("pov_config", false);
    isSlaveMode = prefs.getBool("is_slave", false);

    uint8_t mac[6];
    WiFi.macAddress(mac);
    char nameBuf[20];
    sprintf(nameBuf, "pov-wheel-%02x%02x", mac[4], mac[5]);
    hostName = String(nameBuf);

   // 1. Включаем ОДНОВРЕМЕННО режим клиента и точки доступа
    WiFi.mode(WIFI_AP_STA);

    // 2. СРАЗУ поднимаем свою AP, независимо от того, есть рядом роутер или нет
    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(hostName.c_str(), "", 1); 

    // 3. Пытаемся подключиться к домашней сети / телефону (STA)
    WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASS);
    
    uint32_t startAttempt = millis();
    bool connected = false;
    
    // Ждем 10 секунд
    while (millis() - startAttempt < 10000) {
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        delay(500);
    }

    if (connected) {
        // Успешно подключились к роутеру!
        String currentIP = WiFi.localIP().toString();
        
        // Отправляем свой IP в "облачный буфер"
        HTTPClient http;
        http.begin("http://dweet.io/dweet/for/pov-wheel-belgrade-777?ip=" + currentIP);
        http.GET();
        http.end();
        
    } else { 
        // Если роутер не найден, отключаем режим клиента (STA) для экономии энергии
        // и оставляем только режим точки доступа (AP)
        WiFi.mode(WIFI_AP); 
    }
    
    MDNS.begin(hostName.c_str());

    // --- НАСТРОЙКА ВЕБ-СЕРВЕРА ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна при загрузке страницы
        if (LittleFS.exists("/index.html")) request->send(LittleFS, "/index.html", "text/html");
        else request->send(404, "text/plain", "Upload Filesystem Image!");
    });

    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна при настройке
        if (request->hasParam("bmin")) min_brightness = request->getParam("bmin")->value().toInt();
        if (request->hasParam("bmax")) max_brightness = request->getParam("bmax")->value().toInt();
        if (request->hasParam("a")) global_angle_offset = request->getParam("a")->value().toInt();
        sendSettingsToSlave(); // Отправляем синхронизацию диапазонов и угла
        request->send(200, "text/plain", "OK");
    });

    server.on("/list", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна при обновлении списка файлов
        File root = LittleFS.open("/");
        String json = "[";
        File file = root.openNextFile();
        bool first = true;
        while(file) {
            String fn = String(file.name());
            if(fn.endsWith(".bin")) {
                if(!first) json += ",";
                json += "\"" + fn + "\"";
                first = false;
            }
            file = root.openNextFile();
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    server.on("/play", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна при запуске файла
        if (request->hasParam("file")) {
            String fname = request->getParam("file")->value();
            loadFrameFromFile("/" + fname);
            
            // Сохраняем имя файла для автозапуска при выходе из сна
            prefs.putString("last_file", fname);
            force_stop_display = false; // Сбрасываем флаг принудительной остановки
            
            Serial.println("Запуск рендеринга файла: " + fname);
            request->send(200, "text/plain", "Playing");
        }
    });

    server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна при остановке рендеринга
        force_stop_display = true;
        Serial.println("Web Interface: Принудительная остановка рендеринга (Stop Display)");
        request->send(200, "text/plain", "Stopped");
    });

    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна при удалении файла
        if (request->hasParam("file")) {
            LittleFS.remove("/" + request->getParam("file")->value());
            request->send(200, "text/plain", "Deleted");
        }
    });

    server.on("/album", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна при настройке альбома
        if (request->hasParam("state")) slideshowActive = (request->getParam("state")->value() == "1");
        if (request->hasParam("interval")) slideInterval = request->getParam("interval")->value().toInt() * 1000;
        request->send(200, "text/plain", "Album updated");
    });

    server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна при окончании загрузки файла
        request->send(200, "text/plain", "OK");
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        last_web_activity_time = millis(); // Сброс таймера сна в процессе загрузки частей файла
        String filepath = "/" + (request->hasParam("name") ? request->getParam("name")->value() : "temp.bin");
        if (index == 0) uploadFile = LittleFS.open(filepath, "w");
        if (uploadFile) uploadFile.write(data, len);
        if (index + len == total && uploadFile) uploadFile.close();
    });

    // --- Пинг для сброса таймера перед уходом на OTA ---
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна перед OTA-обновлением
        request->send(200, "text/plain", "OK");
    });

    // --- ОБРАБОТЧИКИ ESP-NOW СИНХРОНИЗАЦИИ ---
    server.on("/scan_slaves", HTTP_GET, [](AsyncWebServerRequest *request){ 
        last_web_activity_time = millis(); // Сброс таймера сна при поиске
        discoverSlaves(); 
        request->send(200, "text/plain", "Scanning..."); 
    });
    
    server.on("/scan_results", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{\"name\":\"" + foundSlaveName + "\", \"mac\":\"" + foundSlaveMAC + "\", \"paired\":" + (isPaired ? "true" : "false") + "}";
        request->send(200, "application/json", json);
    });
    
    server.on("/pair", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis(); // Сброс таймера сна при сопряжении
        if (request->hasParam("mac")) { 
            pairWithSlave(request->getParam("mac")->value()); 
            request->send(200, "text/plain", "OK"); 
        } else { 
            request->send(400, "text/plain", "Error"); 
        }
    });

    ElegantOTA.begin(&server);
    ElegantOTA.onStart(safeOTAShutdown);
    server.begin();

    ArduinoOTA.setHostname(hostName.c_str());
    ArduinoOTA.onStart(safeOTAShutdown);
    ArduinoOTA.begin();
}

void loopNetwork() {
    ArduinoOTA.handle();
    ElegantOTA.loop();

    // Логика кнопки переключения режимов (нажатие 3 сек)
    if (digitalRead(PIN_BUTTON) == LOW) {
        if (!btnState) { 
            btnState = true; 
            btnPressTime = millis(); 
        }
        else if (millis() - btnPressTime > 3000) {
            isSlaveMode = !isSlaveMode;
            prefs.putBool("is_slave", isSlaveMode);
            FastLED.clear(true);
            fill_solid(leds, NUM_LEDS, isSlaveMode ? CRGB::Blue : CRGB::Green);
            FastLED.show(); 
            delay(1000); 
            ESP.restart();
        }
    } else { 
        btnState = false; 
    }
}