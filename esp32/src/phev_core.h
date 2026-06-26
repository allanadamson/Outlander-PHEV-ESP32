#ifndef PHEV_CORE_H
#define PHEV_CORE_H

#include <Arduino.h>

// ======================================================
// Protocol limits
// ======================================================

constexpr size_t PHEV_MAX_PACKET_SIZE  = 256;
constexpr size_t PHEV_MAX_PAYLOAD_SIZE = 248;

// ======================================================
// Packet types
// ======================================================

enum class PHEVPacketType : uint8_t
{
    Unknown  = 0,
    Request  = 0x00,
    Response = 0x01
};

// ======================================================
// Commands
// ======================================================

enum class PHEVCommand : uint8_t
{
    StartRequest     = 0xE4,
    StartRequest18   = 0xE5,
    PingRequest      = 0xF3,
    RegisterRequest  = 0xF6,

    StartAck         = 0x2F,
    PingResponse     = 0x3F,
    RegisterResponse = 0x6F,

    BB               = 0xBB,
    CC               = 0xCC,

    Unknown          = 0x00
};

// ======================================================
// Raw decoded message
// ======================================================

struct PHEVMessage
{
    uint8_t command = 0;

    uint8_t type = 0;

    uint8_t registerId = 0;

    uint8_t xorValue = 0;

    uint8_t checksum = 0;

    uint16_t payloadLength = 0;

    uint8_t payload[PHEV_MAX_PAYLOAD_SIZE];
};

// ======================================================
// Decoder Context
// ======================================================

struct PHEVContext
{
    bool connected = false;

    bool encrypted = false;

    uint8_t currentXor = 0;

    uint8_t commandXor = 0;

    uint8_t pingXor = 0;

    uint8_t pingCounter = 1;

    uint32_t lastPing = 0;
};

// ======================================================
// Packet Buffer
// ======================================================

struct PHEVPacket
{
    uint16_t length = 0;

    uint8_t data[PHEV_MAX_PACKET_SIZE];
};

// ======================================================
// Checksum
// ======================================================

uint8_t calculateChecksum(const uint8_t *packet);

bool verifyChecksum(const uint8_t *packet);

// ======================================================
// XOR
// ======================================================

void xorBuffer(uint8_t *buffer,
               size_t length,
               uint8_t xorValue);

bool decodePacket(const uint8_t *input,
                  size_t available,
                  PHEVPacket &decoded,
                  uint8_t &xorValue);

bool encodePacket(PHEVPacket &packet,
                  uint8_t xorValue);

// ======================================================
// Message conversion
// ======================================================

bool packetToMessage(const PHEVPacket &packet,
                     PHEVMessage &message);

bool messageToPacket(const PHEVMessage &message,
                     PHEVPacket &packet);

// ======================================================
// Builders
// ======================================================

void buildPing(PHEVContext &ctx,
               PHEVMessage &msg);

void buildStart(const uint8_t mac[6],
                PHEVMessage &msg);

void buildRegisterRead(uint8_t reg,
                       PHEVMessage &msg);

void buildRegisterWrite(uint8_t reg,
                        const uint8_t *payload,
                        uint8_t length,
                        PHEVMessage &msg);

// ======================================================
// Helpers
// ======================================================

bool isIncomingCommand(uint8_t cmd);

bool isOutgoingCommand(uint8_t cmd);

#endif