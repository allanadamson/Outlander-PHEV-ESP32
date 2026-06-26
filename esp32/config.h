#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>

// =====================================================
// WiFi Configuration
// =====================================================

#define WIFI_SSID      "REMOTE57zerv"
#define WIFI_PASSWORD  "JfxSFvXFeM72C5"

// =====================================================
// Vehicle
// =====================================================

#define VEHICLE_IP     "192.168.8.46"
#define VEHICLE_PORT   8080

// ESP32 static IP
#define LOCAL_IP       IPAddress(192,168,8,48)
#define LOCAL_GATEWAY  IPAddress(192,168,8,46)
#define LOCAL_SUBNET   IPAddress(255,255,255,0)

// =====================================================
// MQTT
// =====================================================

#define MQTT_SERVER    "213.35.245.246"
#define MQTT_PORT      1883

#define MQTT_CLIENT_ID "OutlanderESP32"

#define MQTT_BASE_TOPIC "outlander"

// =====================================================
// SIMCOM
// =====================================================

#define MODEM_TX_PIN   26
#define MODEM_RX_PIN   27
#define MODEM_PWR_PIN  4

// =====================================================
// Timing
// =====================================================

#define TCP_TIMEOUT_MS         1000
#define PING_INTERVAL_MS      30000
#define MQTT_RECONNECT_MS      5000
#define WIFI_RECONNECT_MS      5000
#define TCP_RECONNECT_MS       3000

// =====================================================
// Protocol
// =====================================================

#define PHEV_TCP_BUFFER_SIZE   1024
#define PHEV_PACKET_SIZE       256

// =====================================================
// Registered Phone MAC
// =====================================================

static const uint8_t REGISTERED_MAC[6] =
{
    0xFA,
    0xBC,
    0xC6,
    0x52,
    0xC6,
    0xAE
};

#endif