// SPDX-License-Identifier: GPL-3.0-or-later
// ue5 bridge: general progression bonuses for the local human player of a command-line skirmish.
// Enabled with -genzhbonus <path>. The file is plain text written by the client, one effect per line:
//   <type> <kind> <value>                 kind is ALL, INFANTRY, VEHICLE (not aircraft), AIRCRAFT, STRUCTURE,
//                                         DEFENSE (KINDOF_FS_BASE_DEFENSE) or - (= ALL); values are numbers
//   cash - <amount>                       starting money (no sound, not counted as income)
//   cost <kind> <pct>                     production cost change in percent (-10 = 10% cheaper)
//   time <kind> <pct>                     production time change in percent (-10 = 10% faster)
//   veteran <kind> <level>                new units start at this veterancy (1 veteran, 2 elite, 3 heroic)
//   promotion - <points>                  general (science purchase) points
//   income - <pct>                        supply box value change in percent
//   health <kind> <pct>                   max health change in percent (health ratio kept)
//   damage <kind> <pct>                   weapon damage change in percent, by the firing object's kind
//   speed <kind> <pct>                    locomotor max speed change in percent
//   power - <pct>                         special power reload time change in percent (-20 = 20% faster)
// Empty lines and lines starting with # or ; are skipped; other unknown lines are logged and ignored.
// Every applied line is printed as "GENZH bonus ...". Not applied with -skirmishobserve.
#pragma once

namespace GeneralBonus
{
	// Applies TheGlobalData->m_generalBonusFile once per game; called by GameLogic after the first logic frame,
	// when the players and the starting objects exist. Does nothing without -genzhbonus.
	void applyCommandLineBonuses();
}
