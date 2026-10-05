#include <Arduino.h>
#include <RadioSecurityFrames.h>

namespace s = radiosensors::security;
namespace f = radiosensors::security::frames;
s::Context context;
uint8_t wire[61];
volatile uint8_t sink;

void setup() {
    uint8_t factory[16], salt[8];
    for (uint8_t i = 0; i < 16; ++i) factory[i] = sink;
    for (uint8_t i = 0; i < 8; ++i) salt[i] = sink;
    osk::crypto::cmacInit(context.authentication, factory);
    f::JoinRequest request;
    request.profileId = 6;
    request.identity.requestNonce = sink;
    request.maxPowerLevel = sink & 31;
    sink = f::sealJoinRequest(context.authentication, s::Transport{100,0,0}, request, wire, sizeof(wire));
    f::JoinAccept accept;
    sink = f::openJoinAccept(context.authentication, s::Transport{0,100,0}, wire, f::kJoinAcceptSize, accept);
    s::initialize(context, factory, salt);
    sink = f::sealJoinProof(context.authentication, s::Transport{100,0,0}, request.identity,
                           salt, false, wire, sizeof(wire));
    sink = f::openJoinProof(context.authentication, s::Transport{0,100,0}, wire,
                           f::kJoinCompleteSize, salt, true, request.identity);
    f::Command command;
    bool hasCommand;
    sink = f::openCommandReply(context, 7, s::Transport{7,100,0}, sink,
                               wire, sink, command, hasCommand);
    f::CommandResult result;
    result.commandId = command.commandId;
    result.dataSize = sink & 7;
    size_t size = 0;
    sink = f::encodeCommandResult(result, wire, sizeof(wire), size);
    sink = s::seal(context, s::Direction::Node, 7, s::Transport{100,7,0x40}, 1,
                   f::kCommandResultHeader, wire, size, wire, sizeof(wire), size);
}
void loop() {}
