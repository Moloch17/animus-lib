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
#include "Env.h"
#include "EpisodeInfoTable.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StageScenario.h"

/*
 * A hazard drill with nothing to fight: fire lands under the seat every few seconds and stays, so the only thing
 * that hurts is standing still.
 *
 * Why a trap gameobject rather than a caster's ground effect. Encoding::FindNearestHazard reads two things as
 * hazards: a DynamicObject, which it takes only from a caster hostile to the seat, and a GAMEOBJECT_TYPE_TRAP,
 * which it takes from nobody at all. The second is the only ground hazard in the game that needs no unit behind
 * it, and Map::SummonGameObject needs no owner to place one, so a stage can have fire without an enemy.
 *
 * Why under the seat. A hazard drill whose fire sits in fixed places teaches nothing: every reward in an episode
 * with no enemy is a penalty (RewardTerm::Hazard is charged and never paid), so a policy would learn to stand in
 * a clear corner and do nothing, which is the behaviour the hazard cap exists to prevent in the stages that do
 * have a fight. Fire that lands where the seat is standing removes that option -- the only way to spend less is to
 * move -- so the penalty alone is enough and the drill needs no objective of its own.
 *
 * What it is not: a fight. No creature is spawned, nothing is targetable, and the episode runs its full length.
 * The numbers to read are hazard_seconds and hazard_damage, both of which should fall.
 */

namespace
{
    /// "Blaze" (gameobject_template 194010): trap diameter 12, so a radius of 6 yards, firing spell 23485. Picked
    /// over the wider Roaring Flame (20) so that stepping out is a step rather than a journey, and over the
    /// narrower Inferno (10) so that standing still is unambiguously wrong.
    constexpr uint32 HAZARD_ENTRY = 194010;

    /// A new patch under each living seat this often. At the 250 ms decision that is a patch every ten decisions:
    /// long enough to have moved out deliberately rather than by accident, short enough that a stationary seat is
    /// always standing in something.
    constexpr uint32 PLACE_EVERY_MS = 2500;

    /// How long one lasts. Longer than the interval, so the ground fills in behind a seat that keeps moving and
    /// the drill is about where to go rather than only about leaving.
    constexpr uint32 PATCH_SECONDS = 8;
}

Animus::Curriculum::HazardEncounter::HazardEncounter(StageScenario& scenario, uint32 envs)
    : Encounter(scenario), _envs(envs)
{
}

void Animus::Curriculum::HazardEncounter::AddEpisodeInfo(EpisodeInfoTable& table)
{
    table.Add("hazard_patches", [this](Env const& env, uint32) { return float(_envs[env.Index].Placed); });
}

void Animus::Curriculum::HazardEncounter::ResetEpisode(Env& env)
{
    Clear(env);
    EnvHazards& state = _envs[env.Index];
    state.Placed = 0;
    state.NextMs = 0;
}

void Animus::Curriculum::HazardEncounter::BeforeRebuild(Env& env)
{
    Clear(env);
}

bool Animus::Curriculum::HazardEncounter::Build(Env& env, Map* /*map*/, uint8 /*level*/)
{
    // Nothing to spawn: the fire arrives on the clock, and the first patch lands one interval in so that a seat
    // is not already standing in one on its first decision.
    EnvHazards& state = _envs[env.Index];
    state.NextMs = PLACE_EVERY_MS;
    return true;
}

void Animus::Curriculum::HazardEncounter::Update(Env& env)
{
    if (_scenario.Arena(env).Against != Opposition::Hazards)
        return;

    EnvHazards& state = _envs[env.Index];
    if (env.EpisodeElapsedMs < state.NextMs)
        return;

    Map* map = env.FindMap();
    if (!map)
        return;

    state.NextMs = env.EpisodeElapsedMs + PLACE_EVERY_MS;
    for (uint32 seat = 0; seat < uint32(env.Bots.size()); ++seat)
    {
        Player* bot = _scenario.SeatBot(env, seat);
        if (!bot || !bot->IsAlive())
            continue;

        // Under the seat, where it is standing now. The trap despawns itself after PATCH_SECONDS
        // (Map::SummonGameObject takes seconds and marks the object temporary), but the guids are kept so an
        // episode that ends early does not leave its fire burning into the next one.
        if (GameObject* fire = map->SummonGameObject(HAZARD_ENTRY, *bot, 0.0f, 0.0f, 0.0f, 0.0f, PATCH_SECONDS))
        {
            state.Live.push_back(fire->GetGUID());
            ++state.Placed;
        }
    }
}

bool Animus::Curriculum::HazardEncounter::IsTerminal(Env const& env) const
{
    if (_scenario.Arena(env).Against != Opposition::Hazards)
        return false;

    // Nothing to clear, so the episode runs its length. It ends early only when every seat that exists is dead.
    //
    // "Exists" is the load-bearing word. Asking only whether anything is alive says yes-it-is-over in the moment
    // before the seats are built, which ended all 128 episodes of the first run at once, none of them having taken
    // a single decision. A seat with no bot behind it has not died; it has not started.
    bool anySeat = false;
    for (uint32 seat = 0; seat < uint32(env.Bots.size()); ++seat)
    {
        Player const* bot = _scenario.SeatBot(env, seat);
        if (!bot)
            continue;

        anySeat = true;
        if (bot->IsAlive())
            return false;
    }
    return anySeat;
}

void Animus::Curriculum::HazardEncounter::Clear(Env& env)
{
    EnvHazards& state = _envs[env.Index];
    Map* map = env.FindMap();
    for (ObjectGuid const& guid : state.Live)
        if (map)
            if (GameObject* fire = map->GetGameObject(guid))
                fire->Delete();

    state.Live.clear();
}
