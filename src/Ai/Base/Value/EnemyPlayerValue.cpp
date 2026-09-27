/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EnemyPlayerValue.h"
#include "BattlegroundWS.h"
#include "CombatManager.h"
#include "Config.h"
#include "GameTime.h"
#include "Group.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Vehicle.h"

#include <unordered_map>

namespace
{
constexpr uint32 WSG_TARGET_LOCK_MS = 5 * IN_MILLISECONDS;

bool IsWsgCasterOrHealer(Player const* player)
{
    switch (player->getClass())
    {
        case CLASS_DRUID:
        case CLASS_MAGE:
        case CLASS_PALADIN:
        case CLASS_PRIEST:
        case CLASS_SHAMAN:
        case CLASS_WARLOCK:
            return true;
        default:
            return false;
    }
}
}

Unit* EnemyPlayerValue::SelectWsgTarget()
{
    wsgReacting = false;
    if (!sConfigMgr->GetOption<bool>("AiPlayerbot.EraWsgSkillTiers", true))
        return SelectWsgTargetLegacy();

    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetBgTypeID() != BATTLEGROUND_WS || bg->GetStatus() != STATUS_IN_PROGRESS)
        return nullptr;

    auto isCandidate = [&](Unit* unit)
    {
        return unit && unit->IsPlayer() && unit->IsAlive() && botAI->IsOpposing(unit->ToPlayer()) &&
               bot->IsWithinDistInMap(unit, 40.0f) && bot->IsWithinLOSInMap(unit);
    };
    auto* ws = static_cast<BattlegroundWS*>(bg);
    Unit* enemyCarrier = botAI->GetUnit(ws->GetFlagPickerGUID(bot->GetTeamId()));
    if (!isCandidate(enemyCarrier))
        enemyCarrier = nullptr;
    Unit* teamCarrier = botAI->GetUnit(ws->GetFlagPickerGUID(bg->GetOtherTeamId(bot->GetTeamId())));

    uint32 const self = bot->GetGUID().GetCounter();
    EraWsgSkill::Tier const tier = EraWsgSkill::TierFor(self);
    EraWsgSkill::Params const params = EraWsgSkill::ParamsFor(tier);

    // Allies already hitting a target make focus emerge without a global call.
    std::unordered_map<uint64, uint32> allies;
    if (Group* group = bot->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                // Same map first: a member elsewhere is updated by another map thread.
                if (member != bot && bot->IsWithinDistInMap(member, 40.0f) && member->IsAlive())
                    if (Unit* target = member->GetVictim())
                        ++allies[target->GetGUID().GetRawValue()];

    std::vector<EraWsgSkill::Candidate> candidates;
    std::unordered_map<uint64, Unit*> units;
    for (ObjectGuid const guid : AI_VALUE(GuidVector, "nearest enemy players"))
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!isCandidate(unit))
            continue;
        bool const threat = teamCarrier && (unit->IsInCombatWith(teamCarrier) ||
            (params.anticipationRange > 0.0f && unit->GetExactDist(teamCarrier) <= params.anticipationRange));
        auto const hitting = allies.find(guid.GetRawValue());
        candidates.push_back({guid.GetRawValue(), unit->GetHealthPct(), bot->GetDistance(unit),
                              hitting == allies.end() ? 0 : hitting->second,
                              IsWsgCasterOrHealer(unit->ToPlayer()), threat,
                              unit->GetVictim() == bot || (unit->GetTarget() == bot->GetGUID() && unit->IsInCombatWith(bot))});
        units[guid.GetRawValue()] = unit;
    }

    uint64 const now = GameTime::GetGameTimeMS().count();
    Unit* victim = bot->GetVictim();
    uint64 const chosen = EraWsgSkill::Choose(tier, skillState, candidates,
        enemyCarrier ? enemyCarrier->GetGUID().GetRawValue() : 0,
        teamCarrier && EraWsgSkill::Chance(self, now, params.carrierDefenseChance),
        isCandidate(victim) ? victim->GetGUID().GetRawValue() : 0, now,
        EraWsgSkill::Chance(self ^ 0x9e3779b9U, now, params.answerChance));
    if (enemyCarrier && chosen == enemyCarrier->GetGUID().GetRawValue())
        return enemyCarrier;
    auto const unit = units.find(chosen);
    if (unit != units.end())
        return unit->second;
    // Still noticing enemies: no instant pick from the generic selection either.
    wsgReacting = !candidates.empty();
    return nullptr;
}

Unit* EnemyPlayerValue::SelectWsgTargetLegacy()
{
    Battleground* bg = bot->GetBattleground();
    if (!bg || bg->GetBgTypeID() != BATTLEGROUND_WS || bg->GetStatus() != STATUS_IN_PROGRESS)
        return nullptr;

    auto isCandidate = [&](Unit* unit)
    {
        return unit && unit->IsPlayer() && unit->IsAlive() && botAI->IsOpposing(unit->ToPlayer()) &&
               bot->IsWithinDistInMap(unit, 40.0f) && bot->IsWithinLOSInMap(unit);
    };
    auto const now = GameTime::GetGameTimeMS().count();
    auto find = [&](ObjectGuid guid) -> Unit*
    {
        Unit* unit = botAI->GetUnit(guid);
        return isCandidate(unit) ? unit : nullptr;
    };

    auto* ws = static_cast<BattlegroundWS*>(bg);
    Unit* enemyCarrier = find(ws->GetFlagPickerGUID(bot->GetTeamId()));
    Unit* teamCarrier = botAI->GetUnit(ws->GetFlagPickerGUID(bg->GetOtherTeamId(bot->GetTeamId())));
    if (enemyCarrier)
    {
        lockedTarget = ObjectGuid::Empty;
        return enemyCarrier;
    }

    Unit* locked = now < lockedTargetUntil ? find(lockedTarget) : nullptr;
    if (locked && teamCarrier && locked->IsInCombatWith(teamCarrier))
        return locked;

    GuidVector const players = AI_VALUE(GuidVector, "nearest enemy players");
    for (ObjectGuid const guid : players)
    {
        Unit* candidate = find(guid);
        if (candidate && teamCarrier && candidate->IsInCombatWith(teamCarrier))
        {
            lockedTarget = candidate->GetGUID();
            lockedTargetUntil = now + WSG_TARGET_LOCK_MS;
            return candidate;
        }
    }

    if (locked)
        return locked;

    for (ObjectGuid const guid : players)
    {
        Unit* candidate = find(guid);
        if (candidate && IsWsgCasterOrHealer(candidate->ToPlayer()))
        {
            lockedTarget = candidate->GetGUID();
            lockedTargetUntil = now + WSG_TARGET_LOCK_MS;
            return candidate;
        }
    }

    for (ObjectGuid const guid : players)
        if (Unit* candidate = find(guid))
        {
            lockedTarget = candidate->GetGUID();
            lockedTargetUntil = now + WSG_TARGET_LOCK_MS;
            return candidate;
        }

    lockedTarget = ObjectGuid::Empty;
    return nullptr;
}

bool NearestEnemyPlayersValue::AcceptUnit(Unit* unit)
{
    // Apply parent's filtering first (includes level difference checks)
    if (!PossibleTargetsValue::AcceptUnit(unit))
        return false;

    bool inCannon = botAI->IsInVehicle(false, true);
    Player* enemy = dynamic_cast<Player*>(unit);
    if (enemy && botAI->IsOpposing(enemy) && enemy->IsPvP() &&
        !sPlayerbotAIConfig.IsPvpProhibited(enemy->GetZoneId(), enemy->GetAreaId()) &&
        !enemy->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NON_ATTACKABLE_2) &&
        ((inCannon || !enemy->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE))) &&
        /*!enemy->HasStealthAura() && !enemy->HasInvisibilityAura()*/ enemy->CanSeeOrDetect(bot) &&
        !(enemy->HasSpiritOfRedemptionAura()))
    {
        // If with master, only attack if master is PvP flagged
        Player* master = botAI->GetMaster();
        if (master && !master->IsPvP() && !master->IsFFAPvP())
            return false;

        return true;
    }

    return false;
}

Unit* EnemyPlayerValue::Calculate()
{
    if (Unit* target = SelectWsgTarget())
        return target;
    if (wsgReacting)
        return nullptr;

    bool controllingCannon = false;
    bool controllingVehicle = false;
    if (Vehicle* vehicle = bot->GetVehicle())
    {
        VehicleSeatEntry const* seat = vehicle->GetSeatForPassenger(bot);
        if (!seat || !seat->CanControl())  // not in control of vehicle so cant attack anyone
            return nullptr;
        VehicleEntry const* vi = vehicle->GetVehicleInfo();
        if (vi && vi->m_flags & VEHICLE_FLAG_FIXED_POSITION)
            controllingCannon = true;
        else
            controllingVehicle = true;
    }

    // 1. Check units we are currently in PvP combat with.
    std::vector<Unit*> targets;
    Unit* pVictim = bot->GetVictim();
    for (auto const& [guid, combatRef] : bot->GetCombatManager().GetPvPCombatRefs())
    {
        Unit* pTarget = combatRef->GetOther(bot);
        if (!pTarget || pTarget == pVictim || !pTarget->IsPlayer() || !pTarget->CanSeeOrDetect(bot) ||
            !bot->IsWithinDist(pTarget, VISIBILITY_DISTANCE_NORMAL))
            continue;

        if ((bot->GetTeamId() == TEAM_HORDE && pTarget->HasAura(23333)) ||
            (bot->GetTeamId() == TEAM_ALLIANCE && pTarget->HasAura(23335)))
            return pTarget;

        targets.push_back(pTarget);
    }

    if (!targets.empty())
    {
        std::sort(targets.begin(), targets.end(),
                  [&](Unit const* pUnit1, Unit const* pUnit2)
                  { return bot->GetDistance(pUnit1) < bot->GetDistance(pUnit2); });

        return *targets.begin();
    }

    // 2. Find enemy player in range.

    GuidVector players = AI_VALUE(GuidVector, "nearest enemy players");
    float const maxAggroDistance = GetMaxAttackDistance();
    for (auto const& gTarget : players)
    {
        Unit* pUnit = botAI->GetUnit(gTarget);
        if (!pUnit)
            continue;

        Player* pTarget = dynamic_cast<Player*>(pUnit);
        if (!pTarget)
            continue;

        if (pTarget == pVictim)
            continue;

        if (bot->GetTeamId() == TEAM_HORDE)
        {
            if (pTarget->HasAura(23333))
                return pTarget;
        }
        else
        {
            if (pTarget->HasAura(23335))
                return pTarget;
        }

        // Aggro weak enemies from further away.
        // If controlling mobile vehicle only agro close enemies (otherwise will never reach objective)
        uint32 const aggroDistance = controllingVehicle                                               ? 5.0f
                                     : (controllingCannon || bot->GetHealth() > pTarget->GetHealth()) ? maxAggroDistance
                                                                                                      : 20.0f;
        if (!bot->IsWithinDist(pTarget, aggroDistance))
            continue;

        if (bot->IsWithinLOSInMap(pTarget) &&
            (controllingCannon || (fabs(bot->GetPositionZ() - pTarget->GetPositionZ()) < 30.0f)))
            return pTarget;
    }

    // 3. Check party attackers.

    if (Group* pGroup = bot->GetGroup())
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            if (Unit* pMember = itr->GetSource())
            {
                if (pMember == bot)
                    continue;

                if (ServerFacade::instance().GetDistance2d(bot, pMember) > 30.0f)
                    continue;

                if (Unit* pAttacker = pMember->getAttackerForHelper())
                    if (pAttacker->IsPlayer() && bot->IsWithinDist(pAttacker, maxAggroDistance * 2.0f) &&
                        bot->IsWithinLOSInMap(pAttacker) && pAttacker != pVictim && pAttacker->CanSeeOrDetect(bot))
                        return pAttacker;
            }
        }
    }

    return nullptr;
}

float EnemyPlayerValue::GetMaxAttackDistance()
{
    if (!bot->GetBattleground())
        return 60.0f;

    Battleground* bg = bot->GetBattleground();
    if (!bg)
        return 40.0f;

    BattlegroundTypeId bgType = bg->GetBgTypeID();
    if (bgType == BATTLEGROUND_RB)
        bgType = bg->GetBgTypeID(true);

    if (bgType == BATTLEGROUND_IC)
    {
        if (botAI->IsInVehicle(false, true))
            return 120.0f;
    }

    return 40.0f;
}
