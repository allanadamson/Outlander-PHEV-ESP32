#ifndef PHEV_CORE_H
#define PHEV_CORE_H

#include <Arduino.h>

// ======================================================
// Protocol constants
// ======================================================

#define PHEV_MAX_MESSAGE_SIZE      256
#define PHEV_MAX_PAYLOAD_SIZE      250

// ======================================================
// Message Types
// ======================================================

enum PHEVMessageType : uint8_t
{
    REQUEST_TYPE  = 0x00,
    RESPONSE_TYPE = 0x01
};

// ======================================================
// Incoming Commands
// ======================================================

enum PHEVIncomingCommand : uint8_t
{
    CMD_START_ACK      = 0x2F,
    CMD_PING_RESPONSE  = 0x3F,
    CMD_START          = 0x4E,
    CMD_START_MY18     = 0x5E,
    CMD_REGISTER       = 0x6F,
    CMD_BB             = 0xBB,
    CMD_CC             = 0xCC
};

// ======================================================
// Outgoing Commands
// ======================================================

enum PHEVOutgoingCommand : uint8_t
{
    CMD_START_SEND     = 0xE4,
    CMD_START_SEND18   = 0xE5,
    CMD_PING_SEND      = 0xF3,
    CMD_REGISTER_SEND  = 0xF6
};

// ======================================================
// Parsed protocol message
// ======================================================

struct PHEVMessage
{
    uint8_t command;
    uint8_t type;
    uint8_t reg;

    uint8_t xorValue;

    uint8_t checksum;

    uint16_t payloadLength;

    uint8_t payload[PHEV_MAX_PAYLOAD_SIZE];
};

// ======================================================
// Decoder state
// ======================================================

struct PHEVDecoderContext
{
    uint8_t currentXOR;

    uint8_t commandXOR;

    uint8_t pingXOR;

    bool encrypt;

    uint8_t currentPing;

    uint8_t pingResponse;
};

// ======================================================
// Message Builder
// ======================================================

struct PHEVPacket
{
    uint16_t length;

    uint8_t data[PHEV_MAX_MESSAGE_SIZE];
};

// ======================================================
// Checksum
// ======================================================

uint8_t phevChecksum(const uint8_t *data);

bool phevValidateChecksum(const uint8_t *data);

// ======================================================
// XOR
// ======================================================

void phevXorData(const uint8_t *src,
                 uint8_t *dst,
                 size_t length,
                 uint8_t xorValue);

uint8_t phevGetMessageXOR(const uint8_t *buffer);

// ======================================================
// Decoder
// ======================================================

bool phevDecodeMessage(const uint8_t *buffer,
                       size_t available,
                       PHEVMessage &message,
                       size_t &packetLength);

// ======================================================
// Encoder
// ======================================================

bool phevEncodeMessage(const PHEVMessage &message,
                       PHEVPacket &packet);

// ======================================================
// Packet helpers
// ======================================================

bool phevIsIncomingCommand(uint8_t cmd);

bool phevIsOutgoingCommand(uint8_t cmd);

bool phevExtractIncomingPacket(const uint8_t *buffer,
                               size_t available,
                               PHEVPacket &packet,
                               uint8_t &xorValue);

bool phevExtractOutgoingPacket(const uint8_t *buffer,
                               size_t available,
                               PHEVPacket &packet,
                               uint8_t &xorValue);

// ======================================================
// Message Builders
// ======================================================

void phevCreatePingMessage(uint8_t counter,
                           PHEVMessage &msg);

void phevCreateStartMessage(const uint8_t mac[6],
                            PHEVMessage &msg);

void phevCreateRegisterMessage(uint8_t reg,
                               const uint8_t *data,
                               size_t length,
                               PHEVMessage &msg);

void phevCreateAck(const PHEVMessage &request,
                   PHEVMessage &response);

// ======================================================
// Outbound XOR
// ======================================================

void phevXorOutbound(PHEVPacket &packet,
                     uint8_t xorValue);

#endif