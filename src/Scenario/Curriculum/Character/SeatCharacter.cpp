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

#include "SeatCharacter.h"
#include "ClassRoleAssets.h"
#include "Layout.h"
#include "Player.h"
#include "SpellChecks.h"
#include "Supplies.h"
#include "TravelBlock.h"

Animus::Curriculum::SeatCharacter::Built Animus::Curriculum::SeatCharacter::Configure(Player* bot,
    Layout const& layout, uint8 specIndex, bool pvp)
{
    ClassRoleAssets const& assets = *layout.Assets;
    SpecProfile const& spec = layout.Profile->Specs[specIndex];

    GearBuilder::LearnProficiencies(bot);

    Built built;
    built.Build = assets.Talents->Standard(spec.Name, spec.TabPage, bot->GetFreeTalentPoints());
    built.UnspentTalentPoints = assets.Talents->Apply(bot, built.Build);

    assets.Kit->Learn(bot);
    if (layout.Has(BlockId::Travel))
        TravelBlock::LearnRiding(bot);
    assets.Talents->ApplyGlyphs(bot, spec.Name);
    assets.Gear->Equip(bot, spec, pvp);

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++built.EquippedItems;

    bot->UpdateAllStats();
    bot->SetFullHealth();
    bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));
    bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));
    bot->SetPower(POWER_RAGE, 0);
    bot->SetPower(POWER_RUNIC_POWER, 0);
    return built;
}

std::vector<uint32> Animus::Curriculum::SeatCharacter::PrepareFighter(Player* bot, Layout const& layout)
{
    using namespace SpellChecks;

    bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);

    std::vector<uint32> stable = layout.Profile->Class == CLASS_HUNTER
        ? StablePool::Instance().Random(STABLE_SLOTS) : std::vector<uint32>();

    if (layout.Profile->Class == CLASS_WARRIOR)
        bot->CastSpell(bot, layout.PlayRole() == Role::Tank && bot->HasSpell(SPELL_DEFENSIVE_STANCE)
            ? SPELL_DEFENSIVE_STANCE : SPELL_BATTLE_STANCE, true);

    return stable;
}
