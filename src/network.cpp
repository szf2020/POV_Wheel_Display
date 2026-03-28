#include "network.h"
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

// --- WiFi credentials ---
const char* HOTSPOT_SSID = "Bunnies Stan 2.4G";
const char* HOTSPOT_PASS = "ValentinaAleksei";

void safeOTAShutdown() {
    FastLED.clear();
    sendLEDs_DMA();
}

void loadFrameFromFile(String path) {
    File f = LittleFS.open(path, "r");
    if (!f) return;

    if (frameBuffer != nullptr) {
        free(frameBuffer);
        frameBuffer = nullptr;
    }

    totalFrames = 1;
    frameDelay = 100;
    currentFrameIndex = 0;
    lastFrameSwitchTime = millis();

    size_t fileSize = f.size();

    if (fileSize > FRAME_SIZE) {
        char magic[4];
        f.read((uint8_t*)magic, 4);
        if (magic[0] == 'A' && magic[1] == 'N' && magic[2] == 'I' && magic[3] == 'M') {
            f.read((uint8_t*)&totalFrames, 2);
            f.read((uint8_t*)&frameDelay, 2);

            size_t dataSize = totalFrames * FRAME_SIZE;
            frameBuffer = (uint8_t*)ps_malloc(dataSize);

            if (frameBuffer) {
                f.read(frameBuffer, dataSize);
            } else {
                totalFrames = 0;
                Serial.println("PSRAM alloc error loading animation!");
            }
        } else {
            f.seek(0);
            frameBuffer = (uint8_t*)ps_malloc(FRAME_SIZE);
            if (frameBuffer) f.read(frameBuffer, FRAME_SIZE);
        }
    } else {
        frameBuffer = (uint8_t*)ps_malloc(FRAME_SIZE);
        if (frameBuffer) f.read(frameBuffer, FRAME_SIZE);
    }

    f.close();
    newFrameReady = true;
}

void setupNetwork() {
    prefs.begin("pov_config", false);

    uint8_t mac[6];
    WiFi.macAddress(mac);
    char nameBuf[20];
    sprintf(nameBuf, "pov-wheel-%02x%02x", mac[4], mac[5]);
    hostName = String(nameBuf);

    WiFi.mode(WIFI_AP_STA);

    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(hostName.c_str(), "", 1);

    WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASS);

    uint32_t startAttempt = millis();
    bool connected = false;

    while (millis() - startAttempt < 10000) {
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            break;
        }
        delay(500);
    }

    if (!connected) {
        WiFi.mode(WIFI_AP);
    }

    MDNS.begin(hostName.c_str());

    // --- WEB SERVER ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis();
        if (LittleFS.exists("/index.html")) request->send(LittleFS, "/index.html", "text/html");
        else request->send(404, "text/plain", "Upload Filesystem Image!");
    });

    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis();
        if (request->hasParam("bmin")) min_brightness = request->getParam("bmin")->value().toInt();
        if (request->hasParam("bmax")) max_brightness = request->getParam("bmax")->value().toInt();
        if (request->hasParam("a")) global_angle_offset = request->getParam("a")->value().toInt();
        request->send(200, "text/plain", "OK");
    });

    server.on("/get_settings", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        json += "\"bmin\":" + String(min_brightness) + ",";
        json += "\"bmax\":" + String(max_brightness) + ",";
        json += "\"angle\":" + String(global_angle_offset) + ",";
        json += "\"brightness\":" + String(global_brightness);
        json += "}";
        request->send(200, "application/json", json);
    });

    server.on("/list", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis();
        File root = LittleFS.open("/");
        String json = "[";
        File file = root.openNextFile();
        bool first = true;
        while(file) {
            String fn = String(file.name());
            if(fn.endsWith(".bin")) {
                if(!first) json += ",";
                json += "{\"name\":\"" + fn + "\",\"size\":" + String(file.size()) + "}";
                first = false;
            }
            file = root.openNextFile();
        }
        json += "]";
        request->send(200, "application/json", json);
    });

    server.on("/fs_info", HTTP_GET, [](AsyncWebServerRequest *request){
        size_t total = LittleFS.totalBytes();
        size_t used  = LittleFS.usedBytes();
        String json = "{\"total\":" + String(total) + ",\"used\":" + String(used) + ",\"free\":" + String(total - used) + "}";
        request->send(200, "application/json", json);
    });

    server.on("/play", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis();
        if (request->hasParam("file")) {
            String fname = request->getParam("file")->value();
            loadFrameFromFile("/" + fname);
            prefs.putString("last_file", fname);
            force_stop_display = false;
            request->send(200, "text/plain", "Playing");
        }
    });

    server.on("/stop", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis();
        force_stop_display = true;
        request->send(200, "text/plain", "Stopped");
    });

    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis();
        if (request->hasParam("file")) {
            LittleFS.remove("/" + request->getParam("file")->value());
            request->send(200, "text/plain", "Deleted");
        }
    });

    server.on("/album", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis();
        if (request->hasParam("state")) slideshowActive = (request->getParam("state")->value() == "1");
        if (request->hasParam("interval")) slideInterval = request->getParam("interval")->value().toInt() * 1000;
        request->send(200, "text/plain", "OK");
    });

    server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis();
        blink_ok_flag = true; // Trigger green blink on upload complete
        request->send(200, "text/plain", "OK");
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
        last_web_activity_time = millis();
        String filepath = "/" + (request->hasParam("name") ? request->getParam("name")->value() : "temp.bin");
        if (index == 0) uploadFile = LittleFS.open(filepath, "w");
        if (uploadFile) uploadFile.write(data, len);
        if (index + len == total && uploadFile) uploadFile.close();
    });

    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request){
        last_web_activity_time = millis();
        request->send(200, "text/plain", "OK");
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
}
