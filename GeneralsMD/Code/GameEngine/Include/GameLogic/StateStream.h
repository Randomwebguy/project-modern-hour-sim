// SPDX-License-Identifier: GPL-3.0-or-later
// ue5 bridge: read-only stream of the logic state to an external client (the Unreal client) over a local TCP socket.
// Enabled with -streamstate <port>. The game waits for one client on 127.0.0.1:<port> before the first frame is sent.
// Protocol (text, one record per line, version 1):
//   H zhstate 1                                 hello, sent once after connect
//   W <mapPath>                                 loaded map (rest of the line), sent once after the hello
//   K <timeOfDay 1-4> <weather 0|1>            map time of day (morning..night) and weather (snowy), once
//   A <river 0|1> <n> <x y z>*n                 water area polygon (world units), once, before the first frame
//   B <template> <from x y z> <to x y z> <width> map bridge (Roads.ini Bridge), once, before the first frame
//   P <playerIndex> <AARRGGBB> <side>          player colour and side, before the first frame that sees the player
//   F <frame> <objectCount>                     start of a logic frame
//   S <id> <playerIndex> <templateName>         object seen for the first time
//   U <id> <x> <y> <z> <angle> <health> <maxHealth> <dead> <conditions hex: moving, attacking, firing A/B/C,
//       dying, damaged, really damaged, rubble, actively constructing, partially constructed, panicking, prone,
//       garrisoned, carrying, docking, using weapon A/B/C, preattack A/B/C, between shots A/B, cheering, exploded
//       flailing/bouncing, climbing, parachuting, freefall, raising flag, captured = bits 0..31>
//       <weapon sets hex: 1 player upgrade, 2 crate upgrade one, 4 crate upgrade two, 8 veteran, 16 elite, 32 heroic> <height above terrain>
//   T <id> <turret> <angle> <pitch>             turret angles relative to the body (radians), sent when they change
//   Z <id> <sx> <sy> <sz> <ex> <ey> <ez> <w> laser beam ends and width scale (W3DLaserDraw objects), when they change
//   Y <drawableId> <template> <ends> <w>  laser drawable without an object (particle uplink cannon); YD <id> when gone
//   D <id>                                      object no longer exists
//   E                                           end of frame
//   L <playerIndex>                             local player (skirmish: the human slot), once
//   M <money> <powerProduced> <powerConsumed>   local player economy, when it changes
//   V <cellsX> <cellsY> <cellSize> <originX> <originY> <runs>  local fog of war, rows of partition cells as
//                                               runs <c|f|s><count> (clear, fogged, shrouded), when it changes
//   X <fxList> <x> <y> <z> [<x2> <y2> <z2>]     effect played (name of the INI FXList), between frames
//   N <audioEvent> <handle> <objectId> [<x> <y> <z>]  sound the local player hears (INI AudioEvent)
//   NE <audioEvent> <objectId> [<x> <y> <z>]   that sound stopped (the game no longer re-adds it each frame)
//   Q <factory> <count> <template>:<percent>... local production queue, when it changes
//   C <id> <percent>                            local building under construction (100 = done)
//   G <victory|defeat>                         local player's game result, once
//   GS <unitsDestroyed> <structuresDestroyed> <unitsBuilt> <moneyEarned> <gameSeconds>  local player's match stats
//                                               (ScoreKeeper, as on the score screen: units / buildings killed, units built, money earned),
//                                               once, right after G
//   R <ok|error> <command> <detail>             result of a client command
// Client -> zh-sim (one command per line): sel <id>..., move|amove|guard <x> <y> <z>, attack <id>, stop, sell, scatter, formation,
//   build <template> <x> <y> <z> <angle>, queue <template>, upgrade <upgrade>, power <CommandButton> [<x> <y> <z>], rally <x> <y> <z>. They become the local player's normal GameMessages,
//   so replays and lockstep stay valid; replays (playback) refuse them.
#pragma once

class Object;

namespace StateStream
{
	// Called once per completed logic frame. Does nothing unless -streamstate was given.
	void publish(UnsignedInt frame, Object *firstObject);
	// Closes the socket (game shutdown).
	void shutdown();
}
