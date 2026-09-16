/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Encounters.h"
#include "CombatReward.h"
#include "Creature.h"
#include "Env.h"
#include "Opponents.h"
#include "Player.h"

std::vector<Animus::Curriculum::RewardTerm> Animus::Curriculum::CreatureEncounter::RewardTerms() const
{
    return { RewardTerm::StepCost, RewardTerm::DamageDealt, RewardTerm::DamageTaken, RewardTerm::Casting,
        RewardTerm::Approach, RewardTerm::StealthOpener, RewardTerm::StealthUtility, RewardTerm::Kill,
        RewardTerm::HealthKept, RewardTerm::Death, RewardTerm::Timeout };
}

bool Animus::Curriculum::CreatureEncounter::Build(Env& env, Map* map, uint8 /*level*/)
{
    EnvState& data = _scenario.Data(env);
    Player* bot = _scenario.SeatBot(env, 0);

    data.OpponentEntry = Opponents::OpponentPool::Instance().Random(data.Seats[0].Level);
    Creature* opponent = data.OpponentEntry ? Opponents::SpawnOpponent(bot, map, data.OpponentEntry) : nullptr;
    if (!opponent)
        return false;

    env.Targets = { opponent->GetGUID() };

    // No pet and no attack: summoning one, stealthing and approaching are all the policy's to learn.
    _scenario.PrepareFighter(bot, data.Seats[0]);
    return true;
}

void Animus::Curriculum::CreatureEncounter::Reward(Env& env, uint32 seat, Player* bot, RewardLedger& ledger)
{
    Unit* opponent = env.FindTargetUnit(0);
    CombatReward::OneOnOne(_scenario, env, seat, bot, opponent, ledger);

    CombatTally& tally = _scenario.Data(env).Seats[seat].Combat;
    if (bot && opponent && opponent->IsAlive())
    {
        uint32 const decisionMs = _scenario.DecisionMs();
        if (Creature* creature = opponent->ToCreature(); creature && creature->IsInEvadeMode())
            tally.TargetEvadeMs += decisionMs;
        if (tally.Engaged && bot->IsAlive() && !bot->IsWithinLOSInMap(opponent))
            tally.OutOfSightMs += decisionMs;
    }

    // Out of time with neither side dead: the fight is lost (IsTerminal ends it as a loss, not a cut-off).
    if (!tally.Killed && !tally.Died && !tally.TimedOut && TimeIsUp(env))
    {
        tally.TimedOut = true;
        ledger.Add(RewardTerm::Timeout, -_scenario.Tuning().Duel.Timeout);
    }
}

bool Animus::Curriculum::CreatureEncounter::TimeIsUp(Env const& env)
{
    return env.EpisodeLengthMs && env.EpisodeElapsedMs >= env.EpisodeLengthMs;
}

bool Animus::Curriculum::CreatureEncounter::IsTerminal(Env const& env) const
{
    // A death ends it once no resurrection of its own is left to wait for. Running out of time ends it too, and as
    // an outcome: the duel is there to be won, so the learner must not bootstrap past the clock as if the fight went
    // on.
    return _scenario.Data(env).Seats[0].Combat.Killed || _scenario.DeadForGood(env, 0) || TimeIsUp(env);
}
