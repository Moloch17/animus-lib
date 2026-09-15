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

#ifndef ANIMUS_LIB_CURRICULUM_SEAT_CHARACTER_H
#define ANIMUS_LIB_CURRICULUM_SEAT_CHARACTER_H

#include "TalentBuilder.h"
#include <vector>

class Player;

/*
 * A learned character as a stage's seats are built, for everyone who plays a class/role model: the forge's seats and
 * mod-animus's companions. What a model saw in training (the standard talent build, trainer spells, gear, a stance) is
 * what it must get in play.
 */
namespace Animus::Curriculum
{
    struct Layout;

    namespace SeatCharacter
    {
        struct Built
        {
            TalentBuilder::Build Build;
            uint32 UnspentTalentPoints = 0;
            uint32 EquippedItems = 0;
        };

        /// Proficiencies, spec `spec`'s standard talents and glyphs, the class's trainer spells and gear (resilience gear
        /// with `pvp`), then full health and mana, full energy and no rage or runic power. The bot's talent points must
        /// be right for its map (InitTalentForLevel after placing it).
        Built Configure(Player* bot, Layout const& layout, uint8 spec, bool pvp);

        /// Get a character ready to fight something that fights back: no XP (levelling up would change the character
        /// under the model) and a warrior's stance (nothing works without one, and only a first login casts it).
        /// Returns a hunter's stable offer (STABLE_SLOTS beasts), empty for other classes.
        std::vector<uint32> PrepareFighter(Player* bot, Layout const& layout);
    }
}

#endif
