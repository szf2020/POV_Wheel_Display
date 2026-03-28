// esp_now_sync.h
#pragma once
#include <Arduino.h>
#include "config.h"

#define PKT_SETTINGS   0x01
#define PKT_DISCOVER   0x03
#define PKT_REPLY      0x04

struct SyncSettings {
    uint8_t min_b;
    uint8_t max_b;
    uint16_t angle;
};

struct DiscoveryReply {
    char name[20];
};

extern uint8_t slaveAddress[6];
extern bool isPaired;
extern String foundSlaveName;
extern String foundSlaveMAC;

void initSync();
void sendSettingsToSlave();
void discoverSlaves();
void pairWithSlave(String macStr);