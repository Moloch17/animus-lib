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

#include "Baselines.h"
#include "CompanionBlock.h"
#include "CoreBlock.h"
#include "DuelBlock.h"
#include "GauntletBlock.h"
#include "PartyBlock.h"
#include "TravelBlock.h"
#include <optional>

namespace
{
    using namespace Animus::Curriculum;

    constexpr float EAT_BELOW = 0.8f;
    constexpr float DRINK_BELOW = 0.8f;
    constexpr float HEAL_OWNER_BELOW = 0.7f;
    constexpr float HEAL_TEAMMATE_BELOW = 0.6f;
    constexpr float CLOSE_IN_BEYOND_YARDS = 4.0f;
    constexpr float MOUNT_BEYOND_YARDS = 80.0f;
    constexpr float CRUISE_HEIGHT_YARDS = 20.0f;

    /// A seat's row, read by block: features and actions by their block-relative index.
    class Row
    {
    public:
        Row(Layout const& layout, float const* obs, uint8 const* mask) : _layout(layout), _obs(obs), _mask(mask) { }

        [[nodiscard]] bool Has(BlockId block) const { return _layout.Has(block); }

        [[nodiscard]] float Obs(BlockId block, uint32 feature) const
        {
            return _obs[_layout.Slice(block).ObsFirst + feature];
        }

        /// The row's action for `action` of `block`, if the layout has it and it is allowed.
        [[nodiscard]] std::optional<int32> Allowed(BlockId block, uint32 action) const
        {
            BlockSlice const& slice = _layout.Slice(block);
            if (!Has(block) || action >= slice.ActionCount || !_mask[slice.ActionFirst + action])
                return std::nullopt;
            return int32(slice.ActionFirst + action);
        }

    private:
        Layout const& _layout;
        float const* _obs;
        uint8 const* _mask;
    };

    std::optional<int32> Fight(Row const& row, Layout const& layout)
    {
        // A living target's health; not the distance, which is 0 in melee range (it is measured between reaches).
        bool const hasTarget = row.Obs(BlockId::Core, CoreBlock::OBS_TARGET_HEALTH) > 0.0f;
        uint32 const heals = layout.AllyHealCount;

        // Travel: a flying mount for a long trip where it flies, else a ground mount; fly at a safe height, land at
        // the objective and dismount there.
        if (row.Has(BlockId::Travel) && row.Obs(BlockId::Travel, TravelBlock::OBS_OBJECTIVE) > 0.0f)
        {
            float const yards = row.Obs(BlockId::Travel, TravelBlock::OBS_OBJECTIVE_DISTANCE) * 500.0f;
            float const height = row.Obs(BlockId::Travel, TravelBlock::OBS_HEIGHT) * 50.0f;
            bool const mounted = row.Obs(BlockId::Travel, TravelBlock::OBS_MOUNTED) > 0.0f;
            bool const flying = row.Obs(BlockId::Travel, TravelBlock::OBS_FLYING_MOUNT) > 0.0f;
            bool const moving = row.Obs(BlockId::Travel, TravelBlock::OBS_MOVING) > 0.0f;

            if (row.Obs(BlockId::Travel, TravelBlock::OBS_AT_OBJECTIVE) > 0.0f)
                return row.Allowed(BlockId::Travel, TravelBlock::ACTION_DISMOUNT);

            if (!mounted && yards > MOUNT_BEYOND_YARDS)
            {
                if (std::optional<int32> fly = row.Allowed(BlockId::Travel, TravelBlock::ACTION_MOUNT_FLYING))
                    return fly;
                if (std::optional<int32> ride = row.Allowed(BlockId::Travel, TravelBlock::ACTION_MOUNT_GROUND))
                    return ride;
            }

            if (flying && yards > MOUNT_BEYOND_YARDS * 0.5f && height < CRUISE_HEIGHT_YARDS && !moving)
                if (std::optional<int32> climb = row.Allowed(BlockId::Travel, TravelBlock::ACTION_ASCEND))
                    return climb;

            if (flying && yards < TravelBlock::ARRIVE_DISTANCE && height > 1.0f && !moving)
                if (std::optional<int32> land = row.Allowed(BlockId::Travel, TravelBlock::ACTION_DESCEND))
                    return land;

            if (!moving)
                if (std::optional<int32> go = row.Allowed(BlockId::Travel, TravelBlock::ACTION_MOVE_TO_OBJECTIVE))
                    return go;

            // On the way: wait (the no-op), rather than cast something that would take the mount away.
            return 0;
        }

        if (row.Has(BlockId::Gauntlet) && !hasTarget)
        {
            if (row.Obs(BlockId::Core, CoreBlock::OBS_HEALTH) < EAT_BELOW)
                if (std::optional<int32> eat = row.Allowed(BlockId::Gauntlet, GauntletBlock::ACTION_EAT))
                    return eat;

            float const mana = row.Obs(BlockId::Core, CoreBlock::OBS_MANA);
            if (mana > 0.0f && mana < DRINK_BELOW)
                if (std::optional<int32> drink = row.Allowed(BlockId::Gauntlet, GauntletBlock::ACTION_DRINK))
                    return drink;
        }

        if (row.Has(BlockId::Companion) && heals
            && row.Obs(BlockId::Companion, CompanionBlock::OBS_OWNER_ALIVE) > 0.0f
            && row.Obs(BlockId::Companion, CompanionBlock::OBS_OWNER_HEALTH) < HEAL_OWNER_BELOW)
        {
            for (uint32 heal = 0; heal < heals; ++heal)
                if (std::optional<int32> action =
                    row.Allowed(BlockId::Companion, CompanionBlock::ACTION_HEAL_FIRST + heal))
                    return action;

            // Heals cannot be cast in most forms.
            if (std::optional<int32> cancel = row.Allowed(BlockId::Duel, DuelBlock::ACTION_CANCEL_FORM))
                return cancel;
        }

        // A hurt teammate: the first heal that can reach it.
        if (row.Has(BlockId::Party) && heals)
        {
            for (uint32 member = 0; member < PARTY_MEMBERS; ++member)
            {
                uint32 const first = PartyBlock::OBS_GLOBAL_COUNT + member * PartyBlock::MEMBER_FEATURES;
                if (row.Obs(BlockId::Party, first + PartyBlock::MEMBER_ALIVE) == 0.0f
                    || row.Obs(BlockId::Party, first + PartyBlock::MEMBER_HEALTH) >= HEAL_TEAMMATE_BELOW)
                    continue;

                // Ally actions are laid out per teammate, every ally spell each; the heals come first.
                uint32 const stride = uint32(layout.AllySpells.size());
                for (uint32 heal = 0; heal < heals; ++heal)
                    if (std::optional<int32> action = row.Allowed(BlockId::Party,
                        PartyBlock::ACTION_HEAL_FIRST + member * stride + heal))
                        return action;
            }
        }

        if (std::optional<int32> attack = row.Allowed(BlockId::Duel, DuelBlock::ACTION_START_ATTACK))
            return attack;

        if (hasTarget && row.Obs(BlockId::Duel, DuelBlock::OBS_DISTANCE) * 60.0f > CLOSE_IN_BEYOND_YARDS
            && row.Obs(BlockId::Duel, DuelBlock::OBS_BOT_MOVING) == 0.0f)
            if (std::optional<int32> move = row.Allowed(BlockId::Duel, DuelBlock::ACTION_MOVE_TO_TARGET))
                return move;

        return std::nullopt;
    }
}

bool Animus::Curriculum::Baselines::Supports(std::string const& policy, Layout const& layout)
{
    return policy == "greedy" || (policy == "fight" && layout.Has(BlockId::Duel));
}

int32 Animus::Curriculum::Baselines::Choose(std::string const& policy, Layout const& layout, float const* obs,
    uint8 const* mask)
{
    Row const row(layout, obs, mask);

    if (policy == "fight" && layout.Has(BlockId::Duel))
        if (std::optional<int32> action = Fight(row, layout))
            return *action;

    // The first usable spell or trinket in catalog order.
    uint32 const catalog = uint32(layout.Catalog().Actions().size());
    for (uint32 action = CoreBlock::FIRST_CAST_ACTION; action < catalog; ++action)
        if (std::optional<int32> allowed = row.Allowed(BlockId::Core, action))
            return *allowed;

    return 0;
}
