// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cctype>
#include <sstream>
#include <string>

namespace SkirmishCheats {
// Build (ZH2026, 1 Nov 2026, the user: the Particle Cannon and every structure buildable at once): prerequisites ignored.
enum Command { NotCheat, Invalid, Help, Money, FastBuild, Power, Reset, Build };
struct Request { Command command = NotCheat; unsigned amount = 50000; bool enabled = true; };
inline bool Allowed(bool skirmish, bool multiplayer, bool replay, bool activeLocalPlayer) {
    return skirmish && !multiplayer && !replay && activeLocalPlayer;
}
inline Request Parse(const char *text) {
    Request result;
    std::istringstream input(text ? text : "");
    std::string prefix, command, argument, extra;
    input >> prefix;
    for (char &c : prefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (prefix != "/cheat") return result;
    result.command = Invalid;
    input >> command >> argument >> extra;
    for (char &c : command) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char &c : argument) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!extra.empty()) return result;
    if (command.empty() || command == "help") { if (argument.empty()) result.command = Help; }
    else if (command == "reset") { if (argument.empty()) result.command = Reset; }
    else if (command == "money") {
        if (!argument.empty()) {
            result.amount = 0;
            for (char c : argument) {
                if (c < '0' || c > '9' || result.amount > 1000000) return result;
                result.amount = result.amount * 10 + (c - '0');
            }
            if (result.amount == 0 || result.amount > 10000000) return result;
        }
        result.command = Money;
    } else if (command == "fastbuild" || command == "power" || command == "build") {
        if (!argument.empty() && argument != "on" && argument != "off") return result;
        result.enabled = argument != "off";
        result.command = command == "fastbuild" ? FastBuild : command == "power" ? Power : Build;
    }
    return result;
}
}

class Player;
bool IsSkirmishCheatActive(SkirmishCheats::Command command, const Player *player);
void ResetSkirmishCheats();
// ZH2026 (27 Oct): the Unreal client's chat runs the same cheats through zh-sim (StateStream chat verb).
bool CanUseSkirmishCheatsNow();
void SetSkirmishCheat(SkirmishCheats::Command command, bool enabled);
