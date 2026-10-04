#include <Arduino.h>
#include <RadioSecurity.h>

using namespace radiosensors::security;
Context context;
volatile uint8_t sink;
uint8_t frame[61];
uint8_t ack[16];

void setup() {
    uint8_t key[16];
    uint8_t salt[8];
    for (uint8_t i = 0; i < 16; ++i) key[i] = sink;
    for (uint8_t i = 0; i < 8; ++i) salt[i] = sink;
    initialize(context, key, salt);
    size_t size = 0;
    seal(context, Direction::Node, 7, Transport{100,7,0x40}, 1, 0x60,
         ack, 10, frame, sizeof(frame), size);
    Ack decoded;
    sink = openAck(context.authentication, Transport{7,100,0x80}, 1,
                   ack, sizeof(ack), decoded);
    frame[0] = 0x61;
    sealJoin(context.authentication, Transport{100,0,0}, frame, 25,
             kNodeTagSize, frame, sizeof(frame), size);
    sink = verifyJoin(context.authentication, Transport{0,100,0},
                      frame, 35, kGatewayTagSize);
    uint32_t counter;
    uint8_t* payload;
    size_t payloadSize;
    sink = open(context, Direction::Gateway, 7, Transport{7,100,0},
                frame, size, counter, payload, payloadSize);
}
void loop() {}
