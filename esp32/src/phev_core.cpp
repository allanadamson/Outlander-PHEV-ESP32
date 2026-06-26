#include "phev_core.h"

#include <string.h>

// =====================================================
// Checksum
// =====================================================

uint8_t calculateChecksum(const uint8_t *packet)
{
    uint8_t length = packet[1] + 2;

    uint8_t sum = 0;

    for (uint8_t i = 0; i < length - 1; i++)
    {
        sum += packet[i];
    }

    return sum;
}

bool verifyChecksum(const uint8_t *packet)
{
    uint8_t length = packet[1] + 2;

    if (length < 2)
        return false;

    return calculateChecksum(packet) == packet[length - 1];
}

// =====================================================
// XOR
// =====================================================

void xorBuffer(uint8_t *buffer,
               size_t length,
               uint8_t xorValue)
{
    if (xorValue == 0)
        return;

    for (size_t i = 0; i < length; i++)
    {
        buffer[i] ^= xorValue;
    }
}
// =====================================================
// Command helpers
// =====================================================

bool isIncomingCommand(uint8_t cmd)
{
    switch (cmd)
    {
        case 0x2F:
        case 0x3F:
        case 0x4E:
        case 0x5E:
        case 0x6F:
        case 0xBB:
        case 0xCC:
            return true;

        default:
            return false;
    }
}

bool isOutgoingCommand(uint8_t cmd)
{
    switch (cmd)
    {
        case 0xE4:
        case 0xE5:
        case 0xF3:
        case 0xF6:
            return true;

        default:
            return false;
    }
}
// =====================================================
// Decode packet
// =====================================================

bool decodePacket(const uint8_t *input,
                  size_t available,
                  PHEVPacket &decoded,
                  uint8_t &xorValue)
{
    if (available < 3)
        return false;

    //
    // First try plain packet
    //

    if (verifyChecksum(input))
    {
        decoded.length = input[1] + 2;

        if (decoded.length > available)
            return false;

        memcpy(decoded.data,
               input,
               decoded.length);

        xorValue = 0;

        return true;
    }

    //
    // Try XOR packet
    //

    uint8_t xorKey = input[2];

    decoded.length = (input[1] ^ xorKey) + 2;

    if (decoded.length > available)
        return false;

    memcpy(decoded.data,
           input,
           decoded.length);

    xorBuffer(decoded.data,
              decoded.length,
              xorKey);

    if (verifyChecksum(decoded.data))
    {
        xorValue = xorKey;

        return true;
    }

    //
    // phevctl tries xor ^ 1
    //

    xorKey ^= 1;

    decoded.length = (input[1] ^ xorKey) + 2;

    if (decoded.length > available)
        return false;

    memcpy(decoded.data,
           input,
           decoded.length);

    xorBuffer(decoded.data,
              decoded.length,
              xorKey);

    if (verifyChecksum(decoded.data))
    {
        xorValue = xorKey;

        return true;
    }

    return false;
}
// =====================================================
// Encode packet
// =====================================================

bool encodePacket(PHEVPacket &packet,
                  uint8_t xorValue)
{
    if (packet.length == 0)
        return false;

    packet.data[packet.length - 1] =
        calculateChecksum(packet.data);

    if (xorValue != 0)
    {
        xorBuffer(packet.data,
                  packet.length,
                  xorValue);
    }

    return true;
}