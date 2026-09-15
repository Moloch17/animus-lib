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

#ifndef ANIMUS_LIB_CURRICULUM_PET_BLOCK_H
#define ANIMUS_LIB_CURRICULUM_PET_BLOCK_H

#include "Block.h"

class Creature;
class Player;

namespace Animus::Curriculum
{
    /// The pet bar of the classes with a controllable pet (hunters, warlocks, death knights, mages): its state, its
    /// most useful abilities (a Felhunter's Spell Lock, a Succubus' Seduction, a Water Elemental's Freeze, a ghoul's
    /// Gnaw, a beast's specials) and its stance and follow orders, as a player uses them. Empty for other classes.
    class PetBlock final : public Block
    {
    public:
        enum Obs : uint32
        {
            OBS_PRESENT                 = 0,
            OBS_ALIVE                   = 1,
            OBS_HEALTH                  = 2,
            OBS_POWER                   = 3,    // focus, energy or mana as a fraction
            OBS_TARGET_DISTANCE         = 4,    // yards from the pet to the bot's target / 60
            OBS_ATTACKING_TARGET        = 5,
            OBS_CASTING                 = 6,
            OBS_REACT_FIRST             = 7,    // one-hot: passive, defensive, aggressive
            OBS_FOLLOWING               = 10,
            OBS_STAYING                 = 11,
            OBS_SLOT_FIRST              = 12,   // per ability slot SLOT_FEATURES
        };

        /// Per ability slot: present, on cooldown, interrupt, crowd control, dispel, threat, helps an ally, damage.
        enum SlotFeature : uint32
        {
            SLOT_PRESENT = 0,
            SLOT_ON_COOLDOWN,
            SLOT_INTERRUPT,
            SLOT_CONTROL,
            SLOT_DISPEL,
            SLOT_THREAT,
            SLOT_POSITIVE,
            SLOT_DAMAGE,
            SLOT_FEATURES
        };

        static constexpr uint32 ABILITY_SLOTS = 4;

        enum Action : uint32
        {
            ACTION_ABILITY_FIRST        = 0,    // cast slot 0..ABILITY_SLOTS-1: at the target, or on itself or the bot
            ACTION_PASSIVE              = 4,
            ACTION_DEFENSIVE            = 5,
            ACTION_AGGRESSIVE           = 6,
            ACTION_FOLLOW               = 7,
            ACTION_STAY                 = 8,
            ACTION_COUNT                = 9
        };

        [[nodiscard]] BlockId Id() const override { return BlockId::Pet; }
        [[nodiscard]] BlockSize Size(Layout const& layout) const override;
        void Observe(SeatView const& view, float* obs, uint8* mask) const override;
        void Apply(SeatView& view, uint32 local, SeatActionResult& result) const override;

        /// Whether the class has a pet this block controls.
        [[nodiscard]] static bool HasPet(uint8 playerClass);
        /// The bot's controllable pet, if one is out.
        [[nodiscard]] static Creature* FindPet(Player* bot);
    };
}

#endif
