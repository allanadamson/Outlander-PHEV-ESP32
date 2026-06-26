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

// =====================================================
// PHEV protocol state
// =====================================================

struct PHEVState
{
    bool connected = false;
    bool encrypted = false;

    uint8_t currentXor = 0;
    uint8_t commandXor = 0;
    uint8_t pingXor = 0;

    uint8_t pingCounter = 1;

    uint32_t lastPing = 0;

    uint8_t rxBuffer[1024];
    size_t rxLength = 0;
};

PHEVState phev;

bool haveCommandXor = false;
uint8_t lastCommandXor = 0x00;

struct PhevCoreMessage
{
    uint8_t data[256];
    uint8_t length = 0;
    uint8_t xorValue = 0;
};

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

void xorPacket(const uint8_t *input,
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

    xorPacket(data, decoded.data, length, xorValue);
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

void parsePackets(const uint8_t *buffer,
                  size_t length)
{
    size_t offset = 0;

    while (offset + 3 <= length)
    {
        PhevCoreMessage decoded;

        if (!phev_core_extractAndDecodeIncomingMessageAndXORBounded(buffer + offset,
                                                                    length - offset,
                                                                    decoded))
        {
            offset++;
            continue;
        }

        Serial.printf(
            "PHEV PACKET CMD=%02X LEN=%u XOR=%02X\n",
            decoded.data[0],
            decoded.length,
            decoded.xorValue
        );

        if (decoded.data[0] == 0xBB && decoded.length >= 5)
        {
            lastCommandXor = decoded.data[4];
            haveCommandXor = true;
            phev.commandXor = decoded.data[4];
            phev.pingXor = decoded.data[4];

            Serial.printf(
                "COMMAND_XOR=%02X\n",
                lastCommandXor
            );
        }
        else if (decoded.data[0] == 0xCC && decoded.length >= 5)
        {
            phev.pingXor = decoded.data[4];

            Serial.printf(
                "PING_XOR=%02X\n",
                phev.pingXor
            );
        }

        offset += decoded.length;
    }
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

void sendPacket(WiFiClient &client, const char* label, const uint8_t* packet, size_t len)
{
    uint8_t out[64];

    if (len > sizeof(out))
        return;

    memcpy(out, packet, len);

    if (haveCommandXor)
    {
        xorPacket(packet, out, len, lastCommandXor);

        Serial.printf("TX XOR=%02X\n", lastCommandXor);
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

parsePackets(buffer, count);

return count;
}
void connectToCarWiFi();

bool connectCarTcp(WiFiClient &client) {
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
      return true;
    }

    delay(1000);
  }

  Serial.println("TCP CONNECT FAIL");
  return false;
}

void sendPhevInit(WiFiClient &client) {
  uint8_t buffer[1024];

  uint8_t init1[] = {
  0xF2, 0x0A, 0x00, 0x01,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00,
  0xFD
};

  uint8_t init2[] = {
    0xF6, 0x04, 0x00, 0xAA, 0x00, 0xA4
  };

  uint8_t init3[] = {
    0xF6, 0x04, 0x00, 0x06, 0x03, 0x03
  };

sendPacket(client, "INIT 1 MAC", init1, sizeof(init1));
readResponseToBuffer(client, "RESP INIT 1", 2000, buffer, sizeof(buffer));

sendPacket(client, "INIT 2 AA", init2, sizeof(init2));
readResponseToBuffer(client, "RESP INIT 2", 3000, buffer, sizeof(buffer));

sendPacket(client, "INIT 3", init3, sizeof(init3));
readResponseToBuffer(client, "RESP INIT 3", 3000, buffer, sizeof(buffer));

uint8_t reg1[] = {0xF3,0x04,0x00,0x01,0x00,0xF8};
uint8_t reg2[] = {0xF3,0x04,0x00,0x02,0x00,0xF9};
uint8_t reg3[] = {0xF3,0x04,0x00,0x03,0x00,0xFA};

sendPacket(client, "REG1", reg1, sizeof(reg1));
readResponseToBuffer(client, "RESP REG1", 3000, buffer, sizeof(buffer));

sendPacket(client, "REG2", reg2, sizeof(reg2));
readResponseToBuffer(client, "RESP REG2", 3000, buffer, sizeof(buffer));

sendPacket(client, "REG3", reg3, sizeof(reg3));
readResponseToBuffer(client, "RESP REG3", 3000, buffer, sizeof(buffer));

sendPacket(client, "REG3", reg3, sizeof(reg3));
readResponseToBuffer(client, "RESP REG3", 3000, buffer, sizeof(buffer));

delay(3000);

uint8_t e4[] = {0xE4,0x04,0x01,0x01,0x00,0xEA};

for(int i=0;i<3;i++)
{
    sendPacket(client, "E4", e4, sizeof(e4));

    size_t count =
        readResponseToBuffer(
            client,
            "RESP E4",
            3000,
            buffer,
            sizeof(buffer)
        );

    if(haveCommandXor)
    {
        Serial.printf(
            "CURRENT XOR = %02X\n",
            lastCommandXor
        );
    }
    else
    {
        Serial.println("NO XOR FOUND");
    }

    Serial.println("\nRAW E4 RESPONSE:");

    for(size_t j = 0; j < count; j++)
    {
        Serial.printf("%02X ", buffer[j]);
    }

    Serial.println("\n");
}

readResponseToBuffer(
  client,
  "RESP REGISTER DISP",
  3000,
  buffer,
  sizeof(buffer)
);
}

void sendRawPingAndRead(WiFiClient &client, int waitMs) {
  uint8_t buffer[1024];

  uint8_t rawPing[] = {
    0xF6, 0x04, 0x00, 0x06, 0x03, 0x03
  };

  sendPacket(client, "SEND RAW PING", rawPing, sizeof(rawPing));
  readResponseToBuffer(client, "RESP RAW PING", waitMs, buffer, sizeof(buffer));
}

void warmupSession(WiFiClient &client) {
  Serial.println("\n--- SESSION WARMUP ---");

  unsigned long endTime = millis() + 12000;

  while (millis() < endTime) {
    sendRawPingAndRead(client, 1200);

    if (haveCommandXor) {
      Serial.println("COMMAND_XOR found during warmup");
      return;
    }

    delay(300);
  }

  Serial.println("WARMUP DONE");
}

void sendXorCommandPair(WiFiClient &client, bool lightsOn)
{
    uint8_t buffer[1024];

    uint8_t rawPing[] = {
        0xF6, 0x04, 0x00, 0x06, 0x03, 0x03
    };

    uint8_t rawLightsOn[] = {
        0xF6, 0x04, 0x00, 0x0A, 0x01, 0x05
    };

    uint8_t rawLightsOff[] = {
        0xF6, 0x04, 0x00, 0x0A, 0x02, 0x06
    };

    Serial.printf("USING COMMAND_XOR=%02X\n", lastCommandXor);

    sendPacket(
        client,
        "SEND XOR PING",
        rawPing,
        sizeof(rawPing)
    );

    readResponseToBuffer(
        client,
        "RESP XOR PING",
        1200,
        buffer,
        sizeof(buffer)
    );

    sendPacket(
        client,
        lightsOn ? "SEND XOR LIGHTS ON" : "SEND XOR LIGHTS OFF",
        lightsOn ? rawLightsOn : rawLightsOff,
        sizeof(rawLightsOn)
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

  haveCommandXor = false;
  lastCommandXor = 0x00;

  if (!connectCarTcp(client)) return;

  sendPhevInit(client);

Serial.println("INIT DONE - WATCHING");

bool xorSent = false;

unsigned long endTime = millis() + 60000;

while (millis() < endTime)
{
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

    if(haveCommandXor && !xorSent)
    {
        Serial.printf(
            "\nFOUND XOR %02X\n",
            lastCommandXor
        );

        sendXorCommandPair(
            client,
            lightsOn
        );

        xorSent = true;
    }

    delay(50);
}

client.stop();
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
  Serial2.begin(115200, SERIAL_8N1, PIN_RX, PIN_TX);

  pinMode(PWR_PIN, OUTPUT);

  digitalWrite(PWR_PIN, LOW);
  delay(500);
  digitalWrite(PWR_PIN, HIGH);

  Serial.println("MODEM START...");
  delay(12000);

  sendAT("AT", 1000);
  sendAT("AT+CGDCONT=1,\"IP\",\"internet.emt.ee\"", 2000);
  sendAT("AT+CGACT=1,1", 5000);

  connectToMQTT();
  connectToCarWiFi();
  testTCPPort();
}

void loop() {
  if (Serial.available()) {
    Serial2.write(Serial.read());
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
