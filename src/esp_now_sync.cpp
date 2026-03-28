// esp_now_sync.cpp
#include "esp_now_sync.h"
#include <esp_now.h>
#include <WiFi.h>
#include <Preferences.h>
#include <FastLED.h>

uint8_t slaveAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; 
bool isPaired = false;
String foundSlaveName = "";
String foundSlaveMAC = "";

extern Preferences prefs;

void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    uint8_t type = incomingData[0];

    // Принимаем настройки (работает для всех, кто их услышит)
    if (type == PKT_SETTINGS) {
        SyncSettings* s = (SyncSettings*)(incomingData + 1);
        min_brightness = s->min_b;
        max_brightness = s->max_b;
        global_angle_offset = s->angle;
    } 
    // Логика ответа на поиск (чтобы интерфейс не сломался)
    else if (isSlaveMode && type == PKT_DISCOVER) {
        if (!esp_now_is_peer_exist(mac)) {
            esp_now_peer_info_t peerInfo = {};
            memcpy(peerInfo.peer_addr, mac, 6);
            peerInfo.channel = 1; // Жестко фиксируем канал 1
            peerInfo.encrypt = false;
            esp_now_add_peer(&peerInfo);
        }
        uint8_t replyBuf[sizeof(DiscoveryReply) + 1];
        replyBuf[0] = PKT_REPLY;
        DiscoveryReply r;
        strcpy(r.name, hostName.c_str());
        memcpy(replyBuf + 1, &r, sizeof(DiscoveryReply));
        esp_now_send(mac, replyBuf, sizeof(replyBuf));
    } 
    // Мастер ловит ответы
    else if (!isSlaveMode && type == PKT_REPLY) {
        DiscoveryReply* r = (DiscoveryReply*)(incomingData + 1);
        foundSlaveName = String(r->name);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        foundSlaveMAC = String(macStr);
    }
}

void discoverSlaves() {
    if (isSlaveMode) return;
    foundSlaveName = ""; foundSlaveMAC = "";
    uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t pkt = PKT_DISCOVER;
    esp_now_send(broadcastMac, &pkt, 1);
}

void pairWithSlave(String macStr) {
    if (isSlaveMode) return;
    int mac[6];
    if (6 == sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5])) {
        for (int i = 0; i < 6; ++i) slaveAddress[i] = (uint8_t)mac[i];
        isPaired = true;
        prefs.putBytes("slave_mac", slaveAddress, 6);
        
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, slaveAddress, 6);
        peerInfo.channel = 1;  
        peerInfo.encrypt = false;
        if (!esp_now_is_peer_exist(slaveAddress)) esp_now_add_peer(&peerInfo);
    }
}

void initSync() {
    if (esp_now_init() != ESP_OK) return;
    esp_now_register_recv_cb(onDataRecv);

    if (!isSlaveMode && prefs.getBytesLength("slave_mac") == 6) {
        prefs.getBytes("slave_mac", slaveAddress, 6);
        isPaired = true;
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, slaveAddress, 6);
        peerInfo.channel = 1;  
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
    }
    
    esp_now_peer_info_t bcInfo = {};
    memset(bcInfo.peer_addr, 0xFF, 6);
    bcInfo.channel = 1;  
    bcInfo.encrypt = false;
    esp_now_add_peer(&bcInfo);
}

void sendSettingsToSlave() {
    uint8_t buffer[sizeof(SyncSettings) + 1];
    buffer[0] = PKT_SETTINGS;
    SyncSettings s = {min_brightness, max_brightness, (uint16_t)global_angle_offset};
    memcpy(buffer + 1, &s, sizeof(SyncSettings));
    
    uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(broadcastMac, buffer, sizeof(buffer));
}