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

#include "PvpBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include "Player.h"
#include "Spell.h"
#include "SpellChecks.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include <algorithm>

namespace
{
    enum PvpSpells : uint32
    {
        SPELL_EVERY_MAN_FOR_HIMSELF = 59752,
        SPELL_WILL_OF_THE_FORSAKEN  = 7744,
    };

    constexpr uint32 MAJOR_COOLDOWN_MS = 60000;
}

Animus::Curriculum::BlockSize Animus::Curriculum::PvpBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, 0 };
}

void Animus::Curriculum::PvpBlock::Observe(SeatView const& view, float* obs, uint8* /*mask*/) const
{
    Player* bot = view.Bot;

    obs[OBS_BOT_STUNNED] = bot->HasUnitState(Encoding::STUN_STATES) ? 1.0f : 0.0f;
    obs[OBS_BOT_ROOTED] = bot->HasUnitState(UNIT_STATE_ROOT) ? 1.0f : 0.0f;
    obs[OBS_BOT_SILENCED] = bot->HasAuraType(SPELL_AURA_MOD_SILENCE) || bot->HasAuraType(SPELL_AURA_MOD_PACIFY_SILENCE)
        ? 1.0f : 0.0f;
    obs[OBS_MIRROR] = view.Mirror ? 1.0f : 0.0f;

    Player* opponent = view.Opponent;
    if (!opponent)
        return;

    WriteOneHot(PLAYABLE_CLASSES, view.OpponentClass, obs + OBS_OPPONENT_CLASS_FIRST);
    obs[OBS_OPPONENT_ROLE_FIRST + uint32(view.OpponentRole)] = 1.0f;
    obs[OBS_OPPONENT_LEVEL_DIFF] = (float(opponent->GetLevel()) - float(bot->GetLevel())) / 5.0f;

    for (uint8 slot : { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 })
        if (SpellInfo const* use = Encoding::TrinketSpell(opponent->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            obs[OBS_OPPONENT_TRINKET_CD] = std::max(obs[OBS_OPPONENT_TRINKET_CD],
                Animus::SpellChecks::CooldownFraction(opponent, use));

    for (uint32 racial : { SPELL_EVERY_MAN_FOR_HIMSELF, SPELL_WILL_OF_THE_FORSAKEN })
        if (opponent->HasSpell(racial))
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(racial))
                obs[OBS_OPPONENT_BREAK_CD] = Animus::SpellChecks::CooldownFraction(opponent, info);

    // Only spells on cooldown are in the map, so this stays cheap.
    uint32 const now = getMSTime();
    uint32 majors = 0;
    for (auto const& [spellId, cooldown] : opponent->GetSpellCooldownMap())
        if (!cooldown.itemid && cooldown.end > now && cooldown.maxduration >= MAJOR_COOLDOWN_MS)
            ++majors;
    obs[OBS_OPPONENT_MAJOR_CDS] = std::min(1.0f, float(majors) / 4.0f);

    if (view.OpponentHidden)
    {
        obs[OBS_OPPONENT_HIDDEN] = 1.0f;
        return;
    }

    if (uint32 const maxMana = opponent->GetMaxPower(POWER_MANA))
        obs[OBS_OPPONENT_MANA] = float(opponent->GetPower(POWER_MANA)) / float(maxMana);

    Powers const power = opponent->getPowerType();
    if (power != POWER_MANA)
        if (uint32 const maxPower = opponent->GetMaxPower(power))
            obs[OBS_OPPONENT_RAGE_ENERGY] = float(opponent->GetPower(power)) / float(maxPower);

    obs[OBS_OPPONENT_CONTROLLED] = Encoding::IsCrowdControlled(opponent) ? 1.0f : 0.0f;
    obs[OBS_OPPONENT_STEALTHED] = opponent->HasAuraType(SPELL_AURA_MOD_STEALTH) ? 1.0f : 0.0f;
    obs[OBS_OPPONENT_PET_OUT] = opponent->GetPetGUID() || !opponent->m_Controlled.empty() ? 1.0f : 0.0f;

    if (Spell const* cast = opponent->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (cast->m_spellInfo->HasEffect(SPELL_EFFECT_HEAL) || cast->m_spellInfo->HasAura(SPELL_AURA_PERIODIC_HEAL))
            obs[OBS_OPPONENT_HEALING] = 1.0f;
}
