/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LfgTriggers.h"
#include "Playerbots.h"

bool LfgProposalActiveTrigger::IsActive() { return AI_VALUE(uint32, "lfg proposal"); }

bool UnknownDungeonTrigger::IsActive()
{
    // If the master already opted this bot into "grind" (autonomous pull/attack) mode, that's an
    // explicit signal the player wants it to act on its own, including holding onto leadership
    // instead of being auto-handed it back and reset to defaults. See ops HISTORY.md for context.
    if (botAI->HasStrategy("dungeon lead", BOT_STATE_NON_COMBAT))
        return false;
    if (botAI->HasStrategy("grind", BOT_STATE_NON_COMBAT))
        return false;

    return IsRealPlayer(botAI->GetMaster()) && botAI->GetMaster() && botAI->GetMaster()->IsInWorld() &&
           botAI->GetMaster()->GetMap()->IsDungeon() && bot->GetMapId() == botAI->GetMaster()->GetMapId();
}
