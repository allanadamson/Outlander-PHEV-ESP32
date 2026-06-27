struct PHEVState;
struct PhevCoreMessage;

#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_wifi.h>

// Auto WiFi
const char* auto_ssid = "REMOTE57zerv";
const char* auto_pass = "JfxSFvXFeM72C5";

// MQTT server
const char* server_ip = "213.35.245.246";
const char* mqtt_topic = "auto/outlander/cmd";

// Auto IP ja port
IPAddress auto_IP(192, 168, 8, 46);
const int auto_port = 8080;

// ESP32 staatiline IP auto WiFi-s
IPAddress local_IP(192, 168, 8, 48);
IPAddress gateway(192, 168, 8, 46);
IPAddress subnet(255, 255, 255, 0);

// Registreeritud telefoni MAC
uint8_t registered_mac[] = {
  0xFA, 0xBC, 0xC6, 0x52, 0xC6, 0xAE
};

// SIMCOM UART
#define PIN_TX 26
#define PIN_RX 27
#define PWR_PIN 4

#define KO_WF_EV_UPDATE_SP 0x06
#define PING_SEND_CMD_MY18 0xF3
#define PHEV_PING_INTERVAL_MS 1000
#define PHEV_HANDSHAKE_WAIT_MS 10000
#define PHEV_READY_STABLE_MS 800

// =====================================================
// PHEV protocol state
// =====================================================

struct PHEVState
{
    bool connected = false;
    bool encrypted = false;
    bool sessionReady = false;
    bool commandLifecycleActive = false;
    bool bbSeenAfterEvAck = false;
    bool ccSeenAfterEvAck = false;
    bool readyCandidate = false;

    uint8_t currentXor = 0;
    uint8_t commandXor = 0;
    uint8_t pingXor = 0;
    uint8_t readyCandidateCommandXor = 0;
    uint8_t readyCandidatePingXor = 0;
    uint8_t lockedCommandXor = 0;
    uint8_t deferredCommandXor = 0;
    uint8_t deferredPingXor = 0;
    bool deferredCommandXorValid = false;
    bool deferredPingXorValid = false;

    uint8_t pingCounter = 1;
    uint8_t pingResponse = 0;

    bool pendingEvUpdate = false;

    uint32_t lastPing = 0;
    uint32_t readyStableSince = 0;

    uint8_t rxBuffer[2048];
    size_t rxLength = 0;
};

PHEVState phev;

struct PhevCoreMessage
{
    uint8_t data[256];
    uint8_t length = 0;
    uint8_t xorValue = 0;
};

void sendPacketWithXor(WiFiClient &client,
                       const char* label,
                       const uint8_t* packet,
                       size_t len,
                       uint8_t xorValue);
void phev_pipe_sendEvUpdate(WiFiClient &client);
void phev_pipe_ping(WiFiClient &client);
void phev_pipe_loopMinimal(WiFiClient &client);

bool phev_core_checkIncomingCommand(const uint8_t command)
{
    switch (command)
    {
        case 0x3F:
        case 0x6F:
        case 0x4E:
        case 0x5E:
        case 0xBB:
        case 0xCC:
        case 0x2F:
        case 0x2E:
            return true;

        default:
            return false;
    }
}

uint8_t phev_core_checksum(const uint8_t *data)
{
    uint8_t checksum = 0;
    int length = data[1] + 2;

    for (int i = 0;; i++)
    {
        if (i >= length - 1)
            return checksum;

        checksum = (uint8_t)(data[i] + checksum);
    }
}

void phev_core_xorDataOutbound(const uint8_t *input,
                               uint8_t *output,
                               size_t length,
                               uint8_t xorValue)
{
    for (size_t i = 0; i < length; i++)
    {
        output[i] = input[i] ^ xorValue;
    }
}

bool phev_core_xorDataWithValueBounded(const uint8_t *data,
                                       const uint8_t xorValue,
                                       const size_t bufLen,
                                       PhevCoreMessage &decoded)
{
    if (bufLen < 2)
        return false;

    uint8_t length = (data[1] ^ xorValue) + 2;

    if (length > bufLen || length > sizeof(decoded.data))
        return false;

    phev_core_xorDataOutbound(data, decoded.data, length, xorValue);
    decoded.length = length;
    decoded.xorValue = xorValue;

    return true;
}

bool phev_core_validateChecksum(const uint8_t *data)
{
    uint8_t length = data[1] + 2;
    uint8_t messageChecksum = data[length - 1];
    uint8_t calculatedChecksum = phev_core_checksum(data);

    return calculatedChecksum == messageChecksum;
}

bool phev_core_validateChecksumXORBounded(const uint8_t *data,
                                          const uint8_t xorValue,
                                          const size_t bufLen)
{
    PhevCoreMessage decoded;

    if (!phev_core_xorDataWithValueBounded(data, xorValue, bufLen, decoded))
        return false;

    return phev_core_validateChecksum(decoded.data);
}

bool phev_core_createMsgXOR(const uint8_t *data,
                            const size_t length,
                            const uint8_t xorValue,
                            PhevCoreMessage &message)
{
    if (length > sizeof(message.data))
        return false;

    memcpy(message.data, data, length);
    message.length = length;
    message.xorValue = xorValue;

    return true;
}

bool phev_core_encodeRawMessage(uint8_t command,
                                uint8_t type,
                                uint8_t reg,
                                const uint8_t *payload,
                                uint8_t payloadLength,
                                uint8_t *output,
                                size_t outputSize,
                                size_t &outputLength)
{
    uint8_t packetLength = payloadLength + 3;
    outputLength = packetLength + 2;

    if (outputLength > outputSize)
        return false;

    output[0] = command;
    output[1] = packetLength;
    output[2] = type;
    output[3] = reg;

    if (payloadLength > 0 && payload != NULL)
    {
        memcpy(output + 4, payload, payloadLength);
    }

    output[outputLength - 1] = phev_core_checksum(output);

    return true;
}

bool phev_core_startMessageEncoded(const uint8_t *mac,
                                   uint8_t *output,
                                   size_t outputSize,
                                   size_t &outputLength)
{
    (void)mac;

    const uint8_t startMessage[] = {
        0xF2, 0x0A, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xFD
    };

    if (sizeof(startMessage) > outputSize)
        return false;

    memcpy(output, startMessage, sizeof(startMessage));
    size_t startLength = sizeof(startMessage);

    const uint8_t aaPayload = 0;
    size_t aaLength = 0;

    if (!phev_core_encodeRawMessage(0xF6,
                                    0x00,
                                    0xAA,
                                    &aaPayload,
                                    1,
                                    output + startLength,
                                    outputSize - startLength,
                                    aaLength))
    {
        return false;
    }

    outputLength = startLength + aaLength;

    return true;
}

bool phev_core_unencodedIncomingMessage(const uint8_t *data,
                                        PhevCoreMessage &message)
{
    uint8_t command = data[0];
    uint8_t length = data[1] + 2;

    if (phev_core_validateChecksum(data))
    {
        switch (command)
        {
        case 0x4E:
        case 0x5E:
        case 0x3F:
        case 0x6F:
        case 0xBB:
        case 0xCC:
        case 0x2E:
        case 0x2F:
            return phev_core_createMsgXOR(data, length, 0, message);
        }
    }

    return false;
}

bool phev_core_encodedIncomingMessageBounded(const uint8_t *data,
                                             const size_t bufLen,
                                             PhevCoreMessage &message)
{
    if (bufLen < 3)
        return false;

    uint8_t xorValue = data[2];
    uint8_t command = data[0] ^ xorValue;
    uint8_t length = (data[1] ^ xorValue) + 2;

    if (length <= bufLen &&
        phev_core_checkIncomingCommand(command) &&
        phev_core_validateChecksumXORBounded(data, xorValue, bufLen))
    {
        return phev_core_createMsgXOR(data, length, xorValue, message);
    }

    xorValue ^= 1;
    command = data[0] ^ xorValue;
    length = (data[1] ^ xorValue) + 2;

    if (length <= bufLen &&
        phev_core_checkIncomingCommand(command) &&
        phev_core_validateChecksumXORBounded(data, xorValue, bufLen))
    {
        return phev_core_createMsgXOR(data, length, xorValue, message);
    }

    return false;
}

bool phev_core_extractIncomingMessageAndXORBounded(const uint8_t *data,
                                                   const size_t bufLen,
                                                   PhevCoreMessage &message)
{
    if (bufLen < 3)
        return false;

    if (phev_core_checkIncomingCommand(data[0]) &&
        phev_core_validateChecksumXORBounded(data, 0, bufLen))
    {
        return phev_core_unencodedIncomingMessage(data, message);
    }

    return phev_core_encodedIncomingMessageBounded(data, bufLen, message);
}

bool phev_core_extractAndDecodeIncomingMessageAndXORBounded(const uint8_t *data,
                                                            const size_t bufLen,
                                                            PhevCoreMessage &decoded)
{
    PhevCoreMessage message;

    if (!phev_core_extractIncomingMessageAndXORBounded(data, bufLen, message))
        return false;

    if (!phev_core_xorDataWithValueBounded(message.data,
                                           message.xorValue,
                                           message.length,
                                           decoded))
    {
        return false;
    }

    decoded.xorValue = message.xorValue;
    return true;
}

void phev_logRawMarkers(const uint8_t *buffer,
                        size_t length,
                        size_t baseOffset)
{
    for (size_t i = 0; i < length; i++)
    {
        switch (buffer[i])
        {
            case 0xBB:
            case 0xCC:
            case 0xCD:
            case 0xBA:
            case 0x2F:
            {
                size_t markerOffset = baseOffset + i;
                size_t windowStart = (i > 16) ? i - 16 : 0;
                size_t windowEnd = i + 33;

                if (windowEnd > length)
                    windowEnd = length;

                Serial.printf(
                    "RX RAW MARKER %02X at %u\n",
                    buffer[i],
                    (unsigned)markerOffset
                );

                Serial.printf(
                    "RX MARKER WINDOW marker=%02X pos=%u:\n",
                    buffer[i],
                    (unsigned)markerOffset
                );

                for (size_t j = windowStart; j < windowEnd; j++)
                {
                    Serial.printf("%02X ", buffer[j]);
                }
                Serial.println();

                if (i + 2 < length)
                {
                    uint8_t data0 = buffer[i];
                    uint8_t data1 = buffer[i + 1];
                    uint8_t data2 = buffer[i + 2];
                    uint8_t xorCandidate = data2;
                    uint8_t xorCandidateAlt = xorCandidate ^ 1;
                    uint16_t plainLen = (uint16_t)data1 + 2;
                    uint8_t decodedCommand = data0 ^ xorCandidate;
                    uint16_t decodedLen = (uint16_t)(data1 ^ xorCandidate) + 2;
                    uint8_t decodedCommandAlt = data0 ^ xorCandidateAlt;
                    uint16_t decodedLenAlt = (uint16_t)(data1 ^ xorCandidateAlt) + 2;

                    Serial.printf("RX MARKER data[0]=%02X\n", data0);
                    Serial.printf("RX MARKER data[1]=%02X\n", data1);
                    Serial.printf("RX MARKER data[2]=%02X\n", data2);
                    Serial.printf("RX MARKER plainLen=%u\n", (unsigned)plainLen);
                    Serial.printf("RX MARKER xorCandidate=%02X\n", xorCandidate);
                    Serial.printf("RX MARKER decodedCommand=%02X\n", decodedCommand);
                    Serial.printf("RX MARKER decodedLen=%u\n", (unsigned)decodedLen);
                    Serial.printf("RX MARKER decodedCommandAlt=%02X\n", decodedCommandAlt);
                    Serial.printf("RX MARKER decodedLenAlt=%u\n", (unsigned)decodedLenAlt);
                }
                else
                {
                    Serial.println("RX MARKER candidate bytes incomplete");
                }

                break;
            }

            default:
                break;
        }
    }
}

bool phev_rxLooksPartialPacket(const uint8_t *buffer,
                               size_t length)
{
    if (length < 3)
        return true;

    if (phev_core_checkIncomingCommand(buffer[0]))
    {
        size_t packetLength = buffer[1] + 2;

        if (packetLength > length)
            return true;
    }

    uint8_t xorValue = buffer[2];
    uint8_t command = buffer[0] ^ xorValue;
    size_t packetLength = (buffer[1] ^ xorValue) + 2;

    if (phev_core_checkIncomingCommand(command) && packetLength > length)
        return true;

    xorValue ^= 1;
    command = buffer[0] ^ xorValue;
    packetLength = (buffer[1] ^ xorValue) + 2;

    if (phev_core_checkIncomingCommand(command) && packetLength > length)
        return true;

    return false;
}

void parsePackets(WiFiClient &client,
                  const uint8_t *buffer,
                  size_t length)
{
    if (length == 0)
        return;

    phev_logRawMarkers(buffer, length, phev.rxLength);

    if (length > sizeof(phev.rxBuffer))
    {
        buffer += length - sizeof(phev.rxBuffer);
        length = sizeof(phev.rxBuffer);
        phev.rxLength = 0;
    }

    if (phev.rxLength + length > sizeof(phev.rxBuffer))
    {
        size_t drop = (phev.rxLength + length) - sizeof(phev.rxBuffer);

        if (drop > phev.rxLength)
            drop = phev.rxLength;

        for (size_t i = 0; i < drop; i++)
        {
            Serial.printf(
                "RX DROP byte=%02X reason=buffer overflow\n",
                phev.rxBuffer[i]
            );
        }

        memmove(phev.rxBuffer,
                phev.rxBuffer + drop,
                phev.rxLength - drop);
        phev.rxLength -= drop;
    }

    memcpy(phev.rxBuffer + phev.rxLength, buffer, length);
    phev.rxLength += length;

    Serial.printf(
        "RX APPEND len=%u total=%u\n",
        (unsigned)length,
        (unsigned)phev.rxLength
    );

    while (phev.rxLength > 0)
    {
        PhevCoreMessage decoded;

        if (!phev_core_extractAndDecodeIncomingMessageAndXORBounded(phev.rxBuffer,
                                                                    phev.rxLength,
                                                                    decoded))
        {
            if (phev_rxLooksPartialPacket(phev.rxBuffer, phev.rxLength))
            {
                Serial.printf(
                    "RX WAIT partial total=%u\n",
                    (unsigned)phev.rxLength
                );
                break;
            }

            Serial.printf(
                "RX DROP byte=%02X reason=no packet start\n",
                phev.rxBuffer[0]
            );

            memmove(phev.rxBuffer,
                    phev.rxBuffer + 1,
                    phev.rxLength - 1);
            phev.rxLength--;
            continue;
        }

        Serial.printf(
            "RX PACKET complete len=%u cmd=%02X xor=%02X\n",
            decoded.length,
            decoded.data[0],
            decoded.xorValue
        );

        Serial.printf(
            "PHEV PACKET CMD=%02X LEN=%u XOR=%02X\n",
            decoded.data[0],
            decoded.length,
            decoded.xorValue
        );

        if (phev.commandLifecycleActive)
        {
            uint8_t afterLightsCommandXor = phev.deferredCommandXorValid
                ? phev.deferredCommandXor
                : phev.commandXor;
            uint8_t afterLightsAckXor = decoded.xorValue ^ afterLightsCommandXor;

            Serial.printf(
                "PHEV AFTER LIGHTS RX cmd=%02X xor=%02X ackXor=%02X\n",
                decoded.data[0],
                decoded.xorValue,
                afterLightsAckXor
            );
        }

        if (decoded.length >= 4 &&
            decoded.data[2] == 0x01 &&
            decoded.data[3] == KO_WF_EV_UPDATE_SP &&
            phev.pendingEvUpdate)
        {
            phev.pendingEvUpdate = false;
            Serial.println("PHEV EV_UPDATE ACK received");
        }

        if ((decoded.data[0] == 0x4E || decoded.data[0] == 0x5E) && decoded.length >= 4)
        {
            uint8_t responseCommand =
                ((decoded.data[0] & 0x0F) << 4) |
                ((decoded.data[0] & 0xF0) >> 4);

            uint8_t responsePayload = 0x00;
            uint8_t response[8];
            size_t responseLength = 0;

            Serial.printf(
                "PHEV RX START %02X reg=%02X\n",
                decoded.data[0],
                decoded.data[3]
            );

            if (phev_core_encodeRawMessage(responseCommand,
                                           0x01,
                                           decoded.data[3],
                                           &responsePayload,
                                           1,
                                           response,
                                           sizeof(response),
                                           responseLength))
            {
                Serial.printf(
                    "PHEV TX START ACK cmd=%02X reg=%02X\n",
                    responseCommand,
                    decoded.data[3]
                );

                sendPacketWithXor(client,
                                  "START ACK",
                                  response,
                                  responseLength,
                                  decoded.xorValue);

                phev.encrypted = true;
                Serial.println("PHEV ENCRYPT ENABLED");
            }
            else
            {
                Serial.println("PHEV START ACK BUILD FAIL");
            }
        }
        else if (decoded.data[0] == 0xBB && decoded.length >= 5)
        {
            if (phev.commandLifecycleActive)
            {
                phev.deferredCommandXor = decoded.data[4];
                phev.deferredPingXor = decoded.data[4];
                phev.deferredCommandXorValid = true;
                phev.deferredPingXorValid = true;
            }
            else
            {
                phev.commandXor = decoded.data[4];
                phev.pingXor = decoded.data[4];
            }

            Serial.printf(
                "PHEV RX BB COMMAND_XOR=%02X\n",
                decoded.data[4]
            );

            if (!phev.pendingEvUpdate)
            {
                phev.bbSeenAfterEvAck = true;
            }

            if (phev.sessionReady)
            {
                Serial.println("EV_UPDATE resend skipped after session ready");
            }
            else if (phev.pendingEvUpdate)
            {
                Serial.println("PHEV EV_UPDATE resend on BB");
                phev_pipe_sendEvUpdate(client);
            }
        }
        else if (decoded.data[0] == 0x6F && decoded.length >= 4)
        {
            Serial.printf(
                "PHEV RX COMMAND 6F type=%02X reg=%02X xor=%02X\n",
                decoded.data[2],
                decoded.data[3],
                decoded.xorValue
            );

            if (phev.commandLifecycleActive &&
                decoded.data[2] == 0x01 &&
                decoded.data[3] == 0x0A)
            {
                Serial.println("PHEV PCAP MATCH POINT");
            }

            if (decoded.data[2] == 0x00)
            {
                uint8_t responseCommand =
                    ((decoded.data[0] & 0x0F) << 4) |
                    ((decoded.data[0] & 0xF0) >> 4);

                uint8_t responsePayload = 0x00;
                uint8_t responseRaw[8];
                size_t responseLength = 0;

                if (phev_core_encodeRawMessage(responseCommand,
                                               0x01,
                                               decoded.data[3],
                                               &responsePayload,
                                               1,
                                               responseRaw,
                                               sizeof(responseRaw),
                                               responseLength))
                {
                    uint8_t responseWithMessageXor[8];
                    uint8_t responseEncoded[8];

                    phev_core_xorDataOutbound(responseRaw,
                                              responseWithMessageXor,
                                              responseLength,
                                              decoded.xorValue);

                    uint8_t ackCommandXor = phev.deferredCommandXorValid
                        ? phev.deferredCommandXor
                        : phev.commandXor;

                    Serial.println("PHEV ACK XOR SOURCE=command");

                    phev_core_xorDataOutbound(responseWithMessageXor,
                                              responseEncoded,
                                              responseLength,
                                              ackCommandXor);

                    Serial.printf(
                        "PHEV TX COMMAND ACK F6 reg=%02X xor=%02X\n",
                        decoded.data[3],
                        ackCommandXor
                    );

                    Serial.print("TX raw: ");
                    for (size_t i = 0; i < responseLength; i++)
                    {
                        Serial.printf("%02X ", responseRaw[i]);
                    }
                    Serial.println();

                    Serial.print("TX encoded: ");
                    for (size_t i = 0; i < responseLength; i++)
                    {
                        Serial.printf("%02X ", responseEncoded[i]);
                    }
                    Serial.println();

                    client.write(responseEncoded, responseLength);
                    client.flush();
                }
                else
                {
                    Serial.println("PHEV COMMAND ACK BUILD FAIL");
                }
            }
        }
        else if (decoded.data[0] == 0xCC && decoded.length >= 5)
        {
            if (phev.commandLifecycleActive)
            {
                phev.deferredPingXor = decoded.data[4];
                phev.deferredPingXorValid = true;
            }
            else
            {
                phev.pingXor = decoded.data[4];
            }

            if (!phev.pendingEvUpdate)
            {
                phev.ccSeenAfterEvAck = true;
            }

            Serial.printf(
                "PHEV RX CC PING_XOR=%02X\n",
                decoded.data[4]
            );
        }
        else if (decoded.data[0] == 0x3F && decoded.length >= 4)
        {
            phev.pingResponse = decoded.data[3];

            Serial.printf(
                "PHEV RX PING_RESPONSE 3F=%02X\n",
                phev.pingResponse
            );
        }

        bool suppressCurrentXor =
            phev.commandLifecycleActive &&
            (decoded.data[0] == 0xBB ||
             decoded.data[0] == 0xCC ||
             decoded.data[0] == 0x6F ||
             decoded.data[0] == 0x3F);

        if (decoded.xorValue != 0 && decoded.xorValue != phev.currentXor)
        {
            if (suppressCurrentXor)
            {
                Serial.println("COMMAND WINDOW: suppress currentXor update");
            }
            else
            {
                Serial.printf(
                    "PHEV CURRENT_XOR %02X -> %02X\n",
                    phev.currentXor,
                    decoded.xorValue
                );

                phev.currentXor = decoded.xorValue;
            }
        }

        memmove(phev.rxBuffer,
                phev.rxBuffer + decoded.length,
                phev.rxLength - decoded.length);
        phev.rxLength -= decoded.length;
    }
}

void phev_pipe_resetPing(PHEVState &ctx)
{
    ctx.pingCounter = 1;
    ctx.pingResponse = 0;
    ctx.lastPing = millis();
}

void phev_pipe_resetConnection(PHEVState &ctx)
{
    ctx.connected = false;
    ctx.encrypted = false;
    ctx.sessionReady = false;
    ctx.commandLifecycleActive = false;
    ctx.bbSeenAfterEvAck = false;
    ctx.ccSeenAfterEvAck = false;
    ctx.readyCandidate = false;
    ctx.currentXor = 0;
    ctx.commandXor = 0;
    ctx.pingXor = 0;
    ctx.readyCandidateCommandXor = 0;
    ctx.readyCandidatePingXor = 0;
    ctx.lockedCommandXor = 0;
    ctx.deferredCommandXor = 0;
    ctx.deferredPingXor = 0;
    ctx.deferredCommandXorValid = false;
    ctx.deferredPingXorValid = false;
    ctx.pendingEvUpdate = false;
    ctx.rxLength = 0;
    ctx.readyStableSince = 0;

    phev_pipe_resetPing(ctx);

    Serial.printf(
        "PHEV STATE reset connected=%d encrypted=%d\n",
        ctx.connected,
        ctx.encrypted
    );
}

String sendAT(String cmd, int wait = 1500) {
  String response = "";
  Serial2.println(cmd);

  unsigned long timeout = millis() + wait;

  while (millis() < timeout) {
    while (Serial2.available()) {
      response += (char)Serial2.read();
    }
  }

  Serial.println("================================");
  Serial.println("AT COMMAND:");
  Serial.println(cmd);
  Serial.println("RESPONSE:");
  Serial.println(response);
  Serial.println("================================");

  return response;
}
inline void xorBuffer(uint8_t *buffer,
                      size_t len,
                      uint8_t key)
{
    if (key == 0)
        return;

    while (len--)
    {
        *buffer ^= key;
        buffer++;
    }
}

void sendPacketWithXor(WiFiClient &client,
                       const char* label,
                       const uint8_t* packet,
                       size_t len,
                       uint8_t xorValue)
{
    uint8_t out[64];

    if (len > sizeof(out))
        return;

    memcpy(out, packet, len);

    if (xorValue != 0)
    {
        phev_core_xorDataOutbound(packet, out, len, xorValue);

        Serial.printf("TX XOR=%02X\n", xorValue);
    }

    Serial.print(label);
    Serial.print(": ");

    for (size_t i = 0; i < len; i++)
    {
        if (out[i] < 0x10)
            Serial.print("0");

        Serial.print(out[i], HEX);
        Serial.print(" ");
    }

    Serial.println();

    client.write(out, len);
    client.flush();
}

void sendPacket(WiFiClient &client, const char* label, const uint8_t* packet, size_t len)
{
    sendPacketWithXor(client, label, packet, len, phev.commandXor);
}

void phev_pipe_outboundPublish(WiFiClient &client,
                               const char* label,
                               const uint8_t *packet,
                               size_t length)
{
    sendPacketWithXor(client, label, packet, length, phev.currentXor);
}

void phev_pipe_pingOutboundPublish(WiFiClient &client,
                                   const char* label,
                                   const uint8_t *packet,
                                   size_t length)
{
    uint8_t pingTxXor = phev.commandLifecycleActive
        ? phev.lockedCommandXor
        : phev.pingXor;

    sendPacketWithXor(client, label, packet, length, pingTxXor);
}

void phev_pipe_commandOutboundPublish(WiFiClient &client,
                                      const char* label,
                                      const uint8_t *packet,
                                      size_t length)
{
    sendPacketWithXor(client, label, packet, length, phev.commandXor);
}

void phev_pipe_sendMac(WiFiClient &client, uint8_t *mac)
{
    uint8_t message[32];
    size_t length = 0;

    Serial.println("PHEV TX START message");
    Serial.println("PHEV TX AA message");

    if (!phev_core_startMessageEncoded(mac, message, sizeof(message), length))
    {
        Serial.println("PHEV START BUILD FAIL");
        return;
    }

    phev_pipe_outboundPublish(client, "START MAC + AA", message, length);
}

void phev_pipe_sendEvUpdate(WiFiClient &client)
{
    if (phev.sessionReady)
    {
        Serial.println("EV_UPDATE resend skipped after session ready");
        return;
    }

    uint8_t payload = 0x03;
    uint8_t raw[8];
    uint8_t encoded[8];
    size_t length = 0;

    if (!phev_core_encodeRawMessage(0xF6,
                                    0x00,
                                    KO_WF_EV_UPDATE_SP,
                                    &payload,
                                    1,
                                    raw,
                                    sizeof(raw),
                                    length))
    {
        Serial.println("PHEV EV_UPDATE BUILD FAIL");
        return;
    }

    phev_core_xorDataOutbound(raw, encoded, length, phev.commandXor);

    Serial.printf(
        "PHEV TX EV_UPDATE reg=%02X value=03\n",
        KO_WF_EV_UPDATE_SP
    );

    Serial.print("TX raw: ");
    for (size_t i = 0; i < length; i++)
    {
        Serial.printf("%02X ", raw[i]);
    }
    Serial.println();

    Serial.print("TX encoded: ");
    for (size_t i = 0; i < length; i++)
    {
        Serial.printf("%02X ", encoded[i]);
    }
    Serial.println();

    phev_pipe_commandOutboundPublish(client, "EV_UPDATE", raw, length);
    phev.pendingEvUpdate = true;
    Serial.println("PHEV EV_UPDATE pending=1");
}

void phev_pipe_ping(WiFiClient &client)
{
    uint8_t payload = 0x00;
    uint8_t raw[8];
    uint8_t encoded[8];
    size_t length = 0;
    uint8_t counter = phev.pingCounter;
    uint8_t pingTxXor = phev.commandLifecycleActive
        ? phev.lockedCommandXor
        : phev.pingXor;

    if (!phev_core_encodeRawMessage(PING_SEND_CMD_MY18,
                                    0x00,
                                    counter,
                                    &payload,
                                    1,
                                    raw,
                                    sizeof(raw),
                                    length))
    {
        Serial.println("PHEV PING BUILD FAIL");
        return;
    }

    phev_core_xorDataOutbound(raw, encoded, length, pingTxXor);

    Serial.printf(
        "PHEV TX PING counter=%u xor=%02X\n",
        counter,
        pingTxXor
    );

    Serial.print("TX raw: ");
    for (size_t i = 0; i < length; i++)
    {
        Serial.printf("%02X ", raw[i]);
    }
    Serial.println();

    Serial.print("TX encoded: ");
    for (size_t i = 0; i < length; i++)
    {
        Serial.printf("%02X ", encoded[i]);
    }
    Serial.println();

    phev_pipe_pingOutboundPublish(client, "PING", raw, length);

    phev.pingCounter++;
    phev.pingCounter %= 0x30;
}

void phev_pipe_loopMinimal(WiFiClient &client)
{
    if (!client.connected() || phev.commandLifecycleActive)
        return;

    uint32_t now = millis();

    if (now - phev.lastPing >= PHEV_PING_INTERVAL_MS)
    {
        phev.lastPing = now;
        phev_pipe_ping(client);
    }
}

bool phev_pipe_updateSessionReady()
{
    if (phev.sessionReady)
        return true;

    bool readyInputsValid =
        !phev.pendingEvUpdate &&
        phev.bbSeenAfterEvAck &&
        phev.ccSeenAfterEvAck &&
        phev.commandXor != 0 &&
        phev.pingXor != 0;

    if (!readyInputsValid)
    {
        phev.readyCandidate = false;
        return false;
    }

    if (!phev.readyCandidate)
    {
        phev.readyCandidate = true;
        phev.readyCandidateCommandXor = phev.commandXor;
        phev.readyCandidatePingXor = phev.pingXor;
        phev.readyStableSince = millis();

        Serial.printf(
            "PHEV READY CANDIDATE commandXor=%02X pingXor=%02X\n",
            phev.commandXor,
            phev.pingXor
        );
    }
    else if (phev.commandXor != phev.readyCandidateCommandXor ||
             phev.pingXor != phev.readyCandidatePingXor)
    {
        phev.readyCandidateCommandXor = phev.commandXor;
        phev.readyCandidatePingXor = phev.pingXor;
        phev.readyStableSince = millis();

        Serial.println("PHEV READY STABLE reset");
    }
    else if (millis() - phev.readyStableSince >= PHEV_READY_STABLE_MS)
    {
        phev.connected = true;
        phev.encrypted = true;
        phev.sessionReady = true;

        Serial.printf(
            "PHEV SESSION READY commandXor=%02X pingXor=%02X\n",
            phev.commandXor,
            phev.pingXor
        );
    }

    return phev.sessionReady;
}

void phev_pipe_beginCommandLifecycle(uint8_t commandXor)
{
    phev.lockedCommandXor = commandXor;
    phev.commandLifecycleActive = true;
    phev.deferredCommandXor = 0;
    phev.deferredPingXor = 0;
    phev.deferredCommandXorValid = false;
    phev.deferredPingXorValid = false;

    Serial.printf(
        "PHEV COMMAND LOCK xor=%02X\n",
        phev.lockedCommandXor
    );
}

void phev_pipe_endCommandLifecycle()
{
    if (!phev.commandLifecycleActive)
        return;

    phev.commandLifecycleActive = false;

    if (phev.deferredCommandXorValid)
    {
        phev.commandXor = phev.deferredCommandXor;
    }

    if (phev.deferredPingXorValid)
    {
        phev.pingXor = phev.deferredPingXor;
    }

    Serial.printf(
        "PHEV COMMAND WINDOW END commandXor=%02X pingXor=%02X\n",
        phev.commandXor,
        phev.pingXor
    );

    phev.lockedCommandXor = 0;
    phev.deferredCommandXor = 0;
    phev.deferredPingXor = 0;
    phev.deferredCommandXorValid = false;
    phev.deferredPingXorValid = false;
}

size_t readResponseToBuffer(WiFiClient &client, const char* label, int waitMs, uint8_t* buffer, size_t maxLen) 
  {
  size_t count = 0;

  Serial.print(label);
  Serial.print(": ");

  unsigned long endTime = millis() + waitMs;

  while (millis() < endTime && count < maxLen) {
    while (client.available() && count < maxLen) {
      uint8_t b = client.read();
      buffer[count++] = b;

      if (b < 0x10) Serial.print("0");
      Serial.print(b, HEX);
      Serial.print(" ");
    }

    delay(10);
  }

  Serial.println();

uint8_t found;

bool foundAny = false;


Serial.printf(
    "COUNT=%u\n",
    (unsigned)count
);

for(size_t i = 0; i < count; i++)
{
    if(buffer[i] == 0x4E)
    {
        Serial.printf(
            "4E FOUND AT %u\n",
            (unsigned)i
        );
    }
}
Serial.printf("COUNT=%u\n", count);

parsePackets(client, buffer, count);

return count;
}
void connectToCarWiFi();

bool connectCarTcp(WiFiClient &client) {
  Serial.println("TRACE: connectCarTcp() begin");
  Serial.println("\n--- AUTO TCP CONNECT ---");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    connectToCarWiFi();
    delay(3000);
  }

  for (int i = 0; i < 5; i++) {
    Serial.printf("TCP attempt %d\n", i + 1);

    if (client.connect(auto_IP, auto_port)) {
      Serial.println("TCP CONNECT OK");
      phev.connected = true;
      Serial.printf(
        "PHEV STATE connected=%d encrypted=%d\n",
        phev.connected,
        phev.encrypted
      );
      return true;
    }

    delay(1000);
  }

  Serial.println("TCP CONNECT FAIL");
  phev.connected = false;
  Serial.printf(
    "PHEV STATE connected=%d encrypted=%d\n",
    phev.connected,
    phev.encrypted
  );
  return false;
}

void sendPhevInit(WiFiClient &client) {
  uint8_t buffer[1024];

  Serial.println("PHEV HANDSHAKE START");
  Serial.printf(
    "PHEV STATE connected=%d encrypted=%d\n",
    phev.connected,
    phev.encrypted
  );

  phev_pipe_sendMac(client, registered_mac);
  phev_pipe_sendEvUpdate(client);

  phev_pipe_ping(client);
  phev.lastPing = millis();

  unsigned long endTime = millis() + PHEV_HANDSHAKE_WAIT_MS;

  while (millis() < endTime && !phev.sessionReady)
  {
    phev_pipe_loopMinimal(client);

    if (client.available())
    {
      readResponseToBuffer(client, "RESP HANDSHAKE", 100, buffer, sizeof(buffer));
    }

    phev_pipe_updateSessionReady();
    delay(10);
  }

  if (!phev_pipe_updateSessionReady())
  {
    Serial.printf(
      "PHEV SESSION NOT READY pendingEvUpdate=%d commandXor=%02X\n",
      phev.pendingEvUpdate,
      phev.commandXor
    );
  }
}

void sendRawPingAndRead(WiFiClient &client, int waitMs) {
  uint8_t buffer[1024];

  uint8_t rawPing[] = {
    0xF6, 0x04, 0x00, 0x06, 0x03, 0x03
  };

  Serial.printf(
    "PHEV TX PING counter=%u pingXor=%02X\n",
    phev.pingCounter,
    phev.pingXor
  );

  phev_pipe_pingOutboundPublish(client, "SEND RAW PING", rawPing, sizeof(rawPing));
  readResponseToBuffer(client, "RESP RAW PING", waitMs, buffer, sizeof(buffer));
}

void warmupSession(WiFiClient &client) {
  Serial.println("\n--- SESSION WARMUP ---");

  unsigned long endTime = millis() + 12000;

  while (millis() < endTime) {
    sendRawPingAndRead(client, 1200);

    if (phev.commandXor != 0) {
      Serial.println("COMMAND_XOR found during warmup");
      return;
    }

    delay(300);
  }

  Serial.println("WARMUP DONE");
}

void sendXorCommandPair(WiFiClient &client,
                        bool lightsOn,
                        uint8_t commandXor)
{
    uint8_t buffer[1024];

    uint8_t rawLightsOn[] = {
        0xF6, 0x04, 0x00, 0x0A, 0x01, 0x05
    };

    uint8_t rawLightsOff[] = {
        0xF6, 0x04, 0x00, 0x0A, 0x02, 0x06
    };

    Serial.printf("USING COMMAND_XOR=%02X\n", commandXor);

    phev_pipe_beginCommandLifecycle(commandXor);

    sendPacketWithXor(
        client,
        lightsOn ? "SEND XOR LIGHTS ON" : "SEND XOR LIGHTS OFF",
        lightsOn ? rawLightsOn : rawLightsOff,
        sizeof(rawLightsOn),
        phev.lockedCommandXor
    );

    readResponseToBuffer(
        client,
        lightsOn ? "RESP XOR LIGHTS ON" : "RESP XOR LIGHTS OFF",
        1800,
        buffer,
        sizeof(buffer)
    );
}

void sendHeadlightsCommand(bool lightsOn) {
  Serial.println(lightsOn ? "\n--- HEADLIGHTS ON ---" : "\n--- HEADLIGHTS OFF ---");

  WiFiClient client;
  uint8_t buffer[1024];

  phev_pipe_resetConnection(phev);

  if (!connectCarTcp(client)) return;

  Serial.println("TRACE: before sendPhevInit()");
  sendPhevInit(client);
  Serial.println("TRACE: after sendPhevInit()");

Serial.println("INIT DONE - WATCHING");

bool commandSent = false;

unsigned long endTime = millis() + 60000;

while (millis() < endTime)
{
    phev_pipe_loopMinimal(client);

    if(client.available())
    {
        readResponseToBuffer(
            client,
            "ASYNC",
            100,
            buffer,
            sizeof(buffer)
        );
    }

    phev_pipe_updateSessionReady();

    if(phev.sessionReady && !commandSent)
    {
        const uint8_t commandTxXor = phev.commandXor;

        Serial.printf(
            "PHEV COMMAND WINDOW READY commandXor=%02X\n",
            commandTxXor
        );

        sendXorCommandPair(
            client,
            lightsOn,
            commandTxXor
        );

        commandSent = true;
        endTime = millis() + 5000;
        Serial.println("PHEV command sent, keeping TCP open for 5 seconds");
    }

    delay(50);
}

phev_pipe_endCommandLifecycle();
client.stop();
phev.connected = false;
Serial.printf(
    "PHEV STATE connected=%d encrypted=%d\n",
    phev.connected,
    phev.encrypted
);
Serial.println("TCP CLOSED");
return;
}
void sendHeadlightsOn() {
  sendHeadlightsCommand(true);
}

void sendHeadlightsOff() {
  sendHeadlightsCommand(false);
}

void subscribeMQTT() {
  Serial.println("\n--- MQTT SUBSCRIBE ---");

  sendAT("AT+CMQTTSUBTOPIC=0,18,0", 2000);
  delay(500);

  Serial2.print(mqtt_topic);
  delay(500);

  String resp = sendAT("AT+CMQTTSUB=0", 5000);

  if (resp.indexOf("+CMQTTSUB: 0,0") != -1) {
    Serial.println("MQTT SUBSCRIBE OK");
  } else {
    Serial.println("MQTT SUBSCRIBE FAIL");
  }
}

void connectToMQTT() {
  Serial.println("TRACE: connectToMQTT() begin");
  Serial.println("\n--- MQTT CONNECT ---");

  sendAT("AT+CMQTTSTART", 5000);
  sendAT("AT+CMQTTACCQ=0,\"ESP32A\",0", 3000);

  String cmd =
    "AT+CMQTTCONNECT=0,\"tcp://" +
    String(server_ip) +
    ":1883\",60,1";

  String resp = sendAT(cmd, 10000);

  if (resp.indexOf("+CMQTTCONNECT: 0,0") != -1) {
    Serial.println("MQTT CONNECT OK");
    subscribeMQTT();
  } else {
    Serial.println("MQTT CONNECT FAIL");
  }

  Serial.println("TRACE: connectToMQTT() end");
}

void testTCPPort() {
  Serial.println("\n--- TCP TEST ---");

  WiFiClient testClient;

  if (testClient.connect(auto_IP, auto_port)) {
    Serial.println("TCP PORT 8080 OPEN");
    testClient.stop();
  } else {
    Serial.println("TCP PORT 8080 CLOSED / NO RESPONSE");
  }
}

void connectToCarWiFi() {
  Serial.println("TRACE: connectToCarWiFi() begin");
  Serial.println("\n--- AUTO WIFI CONNECT ---");

  WiFi.mode(WIFI_STA);

  esp_wifi_stop();
  delay(200);

  esp_wifi_set_mac(WIFI_IF_STA, registered_mac);

  esp_wifi_start();
  delay(200);

  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(auto_ssid, auto_pass);

  Serial.print("Connecting");

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    Serial.print(".");
    delay(500);
  }

  Serial.println();

  Serial.print("WiFi status: ");
  Serial.println(WiFi.status());

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("AUTO WIFI CONNECTED");
    Serial.print("ESP IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Gateway: ");
    Serial.println(WiFi.gatewayIP());
    Serial.print("Subnet: ");
    Serial.println(WiFi.subnetMask());
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());
  } else {
    Serial.println("AUTO WIFI FAILED");
  }

  Serial.println("TRACE: connectToCarWiFi() end");
}

void handleModemData(String resp) {
  Serial.println("\n===== MODEM DATA =====");
  Serial.println(resp);
  Serial.println("======================");

  if (resp.indexOf("LIGHTS_ON") != -1) {
    Serial.println("\n[KÄSK] LIGHTS ON");
    sendHeadlightsOn();
  } else if (resp.indexOf("LIGHTS_OFF") != -1) {
    Serial.println("\n[KÄSK] LIGHTS OFF");
    sendHeadlightsOff();
  } else if (resp.indexOf("CONNLOST") != -1 || resp.indexOf("CLOSED") != -1) {
    Serial.println("\nMQTT CONNECTION LOST");
    connectToMQTT();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("TRACE: setup() begin");

  Serial.println("TRACE: Serial2/modem init begin");
  Serial2.begin(115200, SERIAL_8N1, PIN_RX, PIN_TX);
  Serial.println("TRACE: Serial2/modem init end");

  pinMode(PWR_PIN, OUTPUT);

  digitalWrite(PWR_PIN, LOW);
  delay(500);
  digitalWrite(PWR_PIN, HIGH);

  Serial.println("MODEM START...");
  delay(12000);

  sendAT("AT", 1000);
  sendAT("AT+CGDCONT=1,\"IP\",\"internet.emt.ee\"", 2000);
  sendAT("AT+CGACT=1,1", 5000);

  Serial.println("TRACE: MQTT connect sequence begin");
  connectToMQTT();
  Serial.println("TRACE: MQTT connect sequence end");

  Serial.println("TRACE: WiFi connect sequence begin");
  connectToCarWiFi();
  Serial.println("TRACE: WiFi connect sequence end");

  testTCPPort();

  Serial.println("TRACE: setup() end - PHEV handshake is not called in setup()");
  Serial.println("TRACE: PHEV handshake is called only after LIGHTS_ON or LIGHTS_OFF modem/MQTT data");
}

void loop() {
  static bool loopTracePrinted = false;

  if (!loopTracePrinted) {
    loopTracePrinted = true;
    Serial.println("TRACE: loop() begin - waiting for Serial2 MQTT command before PHEV handshake");
  }

  if (Serial.available()) {
    String usbCmd = Serial.readStringUntil('\n');
    usbCmd.trim();

    if (usbCmd == "LIGHTS_ON") {
      Serial.println("\n[USB TEST] LIGHTS ON");
      sendHeadlightsOn();
    } else if (usbCmd == "LIGHTS_OFF") {
      Serial.println("\n[USB TEST] LIGHTS OFF");
      sendHeadlightsOff();
    } else {
      Serial2.print(usbCmd);
      Serial2.print("\n");
    }
  }

  if (Serial2.available()) {
    String resp = Serial2.readString();
    handleModemData(resp);
  }

  static unsigned long lastCheck = 0;

  if (millis() - lastCheck > 60000) {
    lastCheck = millis();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("AUTO WIFI OK: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("AUTO WIFI RECONNECT");
      connectToCarWiFi();
      testTCPPort();
    }
  }
}
