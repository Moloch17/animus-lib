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

#include "BotFactory.h"
#include "CharacterCache.h"
#include "CoreHooks.h"
#include "GameTime.h"
#include "InstanceSaveMgr.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "SocialMgr.h"
#include "Transport.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    /// Distance from the owner a companion appears at, and the angle relative to the owner's facing.
    constexpr float NEAR_DISTANCE = 2.0f;
    constexpr float NEAR_ANGLE = float(M_PI) / 2;

    /// Where a bot joining `owner` stands: beside the owner; on the owner's spot on a transport (a deck can be
    /// narrow); on the ground below a flight path.
    Position SpotNear(Player* owner)
    {
        if (owner->GetTransport())
            return owner->GetPosition();

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        owner->GetClosePoint(x, y, z, owner->GetCombatReach(), NEAR_DISTANCE, NEAR_ANGLE);

        if (owner->IsInFlight())
            if (float const ground = owner->GetMapHeight(x, y, z, true, MAX_FALL_DISTANCE); ground > INVALID_HEIGHT)
                z = ground;

        return Position(x, y, z, owner->GetOrientation());
    }

    /// The owner's difficulties: the instance a grouped bot enters is chosen with them.
    void MatchDifficulty(Player* bot, Player* owner)
    {
        bot->SetDungeonDifficulty(owner->GetDungeonDifficulty());
        bot->SetRaidDifficulty(owner->GetRaidDifficulty());
    }

    /// A bot is beside its owner: on the owner's transport if it rides one (as a client moving onto the deck), off
    /// any other, and never waiting on an instance lock it cannot answer.
    void Arrived(Player* bot, Player* owner)
    {
        Transport* transport = owner->GetTransport();
        if (bot->GetTransport() != transport)
        {
            if (Transport* old = bot->GetTransport())
                old->RemovePassenger(bot, true);
            if (transport)
                transport->AddPassenger(bot, true);
        }

        bot->SetPendingBind(0, 0);
    }

    /// Player has no setter for m_social on a stock core; LoadFromDB assigns it from the login query. Explicit
    /// instantiation ignores access checks, so this names the private member without a core change.
    template <typename Tag, typename Tag::Type Member>
    struct PrivateMember
    {
        friend typename Tag::Type Access(Tag) { return Member; }
    };

    struct PlayerSocialTag
    {
        using Type = PlayerSocial* Player::*;
        friend Type Access(PlayerSocialTag);
    };

    template struct PrivateMember<PlayerSocialTag, &Player::m_social>;

    /// CharacterCreateInfo keeps its fields protected (it is filled from CMSG_CHAR_CREATE); a
    /// derived type may set them.
    class BotCreateInfo : public CharacterCreateInfo
    {
    public:
        explicit BotCreateInfo(Animus::BotFactory::BotSpec const& spec)
        {
            Name = spec.Name;
            Race = spec.Race;
            Class = spec.Class;
            Gender = spec.Gender;
        }
    };
}

Player* Animus::BotFactory::Create(BotSpec const& spec, WorldSession* session)
{
    bool const ownSession = !session;
    if (ownSession)
    {
        // accountFlags 0: no collector's edition voucher mail (Player::Create's only DB write).
        session = new WorldSession(spec.AccountId, std::string(spec.Name), 0, nullptr, SEC_PLAYER,
            EXPANSION_WRATH_OF_THE_LICH_KING, 0, LOCALE_enUS, 0, false, false, 0);

        // Default permissions for the security level, in memory. Must precede new Player, whose
        // constructor checks a permission and would otherwise run a sync login DB query.
        session->InitRBACDataForTest();

        // No account or character rows exist: on the forge core, logout, play time and instance binds write nothing.
        CoreHooks::MarkSimSession(session);
    }

    Player* bot = new Player(session);
    bot->GetMotionMaster()->Initialize();

    BotCreateInfo info(spec);
    ObjectGuid::LowType const guidLow = spec.GuidLow
        ? spec.GuidLow : sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    if (!bot->Create(guidLow, &info))
    {
        LOG_ERROR("module.animus", "Player::Create failed for bot {} (race {}, class {})", spec.Name, spec.Race,
            spec.Class);
        delete bot;
        if (ownSession)
            delete session;
        return nullptr;
    }

    sInstanceSaveMgr->PlayerCreateBoundInstancesMaps(bot->GetGUID());
    // A null result gives an empty list. LogoutPlayer removes it again.
    bot->*Access(PlayerSocialTag{}) = sSocialMgr->LoadFromDB(nullptr, bot->GetGUID());
    session->SetPlayer(bot);

    // Never save: 0 disables the autosave countdown in Player::Update.
    bot->SetSaveTimer(0);

    // SetLevel, not GiveLevel: GiveLevel sends level-reward mail.
    if (spec.Level && bot->GetLevel() != spec.Level)
    {
        bot->SetLevel(spec.Level, false);
        bot->InitStatsForLevel(true);
        bot->InitTalentForLevel();

        // Raise weapon and defense skill caps to the new level (5 per level), then fill them, as
        // a character who levelled normally would have. Left at level 1 values, every swing would
        // roll against a skill of 5 and mostly miss.
        bot->UpdateSkillsForLevel();
        bot->UpdateSkillsToMaxSkillsForLevel();
    }

    bot->SetCanModifyStats(true);
    bot->UpdateAllStats();
    bot->SetFullHealth();

    sCharacterCache->AddCharacterCacheEntry(bot->GetGUID(), spec.AccountId, spec.Name, spec.Gender, spec.Race,
        spec.Class, bot->GetLevel());

    return bot;
}

Map* Animus::BotFactory::PlaceInNewInstance(Player* bot, uint32 mapId, Position const& pos)
{
    // A groupless player with no bind for this map always gets a brand new instance.
    Map* map = sMapMgr->CreateMap(mapId, bot);
    if (!map || !map->IsDungeon())
    {
        LOG_ERROR("module.animus", "Map {} did not produce a dungeon instance for bot {}", mapId, bot->GetName());
        return nullptr;
    }

    return PlaceInMap(bot, map, pos) ? map : nullptr;
}

bool Animus::BotFactory::PlaceInMap(Player* bot, Map* map, Position const& pos)
{
    // Player::Create parked the bot on its race's start continent; move it before entering.
    bot->ResetMap();
    bot->Relocate(pos);
    bot->SetMap(map);
    bot->SetFallInformation(GameTime::GetGameTime().count(), pos.GetPositionZ());
    bot->SetMover(bot);

    ObjectAccessor::AddObject(bot);

    if (!map->AddPlayerToMap(bot))
    {
        LOG_ERROR("module.animus", "Could not add bot {} to map {} instance {}", bot->GetName(), map->GetId(),
            map->GetInstanceId());
        ObjectAccessor::RemoveObject(bot);
        return false;
    }

    return true;
}

bool Animus::BotFactory::CanJoin(Player* owner)
{
    return owner->IsInWorld() && !owner->IsBeingTeleported() && !owner->GetMap()->IsBattlegroundOrArena();
}

bool Animus::BotFactory::PlaceNear(Player* bot, Player* owner)
{
    if (!CanJoin(owner))
    {
        LOG_ERROR("module.animus", "Bot {} cannot be placed beside {} on map {} (between maps, or a battleground)",
            bot->GetName(), owner->GetName(), owner->GetMapId());
        DestroyUnplaced(bot);
        return false;
    }

    bot->SetPhaseMask(owner->GetPhaseMask(), false);
    MatchDifficulty(bot, owner);
    if (!PlaceInMap(bot, owner->GetMap(), SpotNear(owner)))
    {
        DestroyUnplaced(bot);
        return false;
    }

    Arrived(bot, owner);
    return true;
}

bool Animus::BotFactory::TeleportNear(Player* bot, Player* owner)
{
    if (!CanJoin(owner) || bot->IsBeingTeleported())
        return false;

    // Another map, or another instance of the owner's map: the instance must take the bot, which is checked here
    // rather than failing in the worldport (whose fallback is a teleport to the bot's homebind).
    Map* map = owner->GetMap();
    bool const sameMap = bot->FindMap() == map;
    if (!sameMap)
    {
        if (Map::EnterState const refused = map->CannotEnter(bot, false))
        {
            LOG_DEBUG("module.animus", "Bot {} cannot follow {} into map {} instance {} (enter state {})",
                bot->GetName(), owner->GetName(), map->GetId(), map->GetInstanceId(), uint32(refused));
            return false;
        }
    }

    MatchDifficulty(bot, owner);
    Position const spot = SpotNear(owner);

    // GM mode skips the map's entry requirements (level, attunement, keys, the hourly instance limit): a bot follows
    // its owner wherever the owner went. Another instance of the same map needs the far teleport too.
    bool const newInstance = !sameMap && bot->GetMapId() == owner->GetMapId();
    if (!bot->TeleportTo(owner->GetMapId(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(),
        spot.GetOrientation(), TELE_TO_GM_MODE, nullptr, newInstance))
        return false;

    CompleteTeleport(bot);
    if (bot->FindMap() != map || !bot->IsInWorld())
    {
        LOG_ERROR("module.animus", "Bot {} did not arrive in {}'s map {} instance {}", bot->GetName(),
            owner->GetName(), map->GetId(), map->GetInstanceId());
        return false;
    }

    Arrived(bot, owner);
    return true;
}

void Animus::BotFactory::CompleteTeleport(Player* bot)
{
    // A teleport requested during the bot's own update is delayed to the end of that update, which is over by the
    // time the world thread calls this: the destination is final.
    if (bot->IsBeingTeleportedFar())
    {
        bot->GetSession()->HandleMoveWorldportAck();

        // A bot never answers the instance lock warning: it stays temporarily bound, as if it declined.
        bot->SetPendingBind(0, 0);
    }
    else if (bot->IsBeingTeleportedNear())
    {
        WorldPacket ack(MSG_MOVE_TELEPORT_ACK);
        ack << bot->GetPackGUID();
        ack << uint32(0) << uint32(0);
        bot->GetSession()->HandleMoveTeleportAck(ack);
    }
}

void Animus::BotFactory::DestroyUnplaced(Player* bot)
{
    WorldSession* session = bot->GetSession();

    // ~Unit asserts that every aura (passives included) is gone; this is what removing it from a map
    // would have done. A failed placement may have set the map already.
    bot->CleanupsBeforeDelete();
    if (bot->FindMap())
        bot->ResetMap();

    // LogoutPlayer would have dropped these.
    sCharacterCache->DeleteCharacterCacheEntry(bot->GetGUID(), bot->GetName());
    sSocialMgr->RemovePlayerSocial(bot->GetGUID());

    session->SetPlayer(nullptr);
    delete bot;
    delete session;
}

WorldSession* Animus::BotFactory::Destroy(Player* bot, bool keepSession)
{
    WorldSession* session = bot->GetSession();
    ObjectGuid const guid = bot->GetGUID();
    uint32 const mapId = bot->GetMapId();

    // A bot in the middle of a far teleport is on no map.
    Map* map = bot->FindMap();
    Difficulty const difficulty = map ? map->GetDifficulty() : REGULAR_DIFFICULTY;

    // Off its transport first: the transport keeps a pointer to every passenger.
    if (Transport* transport = bot->GetTransport())
        transport->RemovePassenger(bot, true);

    // A dead bot would be repopped at a graveyard (a far teleport) by LogoutPlayer.
    if (!bot->IsAlive())
        bot->ResurrectPlayer(1.0f);

    // The pet goes first, unsaved: LogoutPlayer would save it to the character database. Totems and
    // guardians (trinket summons, ghouls, water elementals) go with it, while the bot is still in its map.
    if (map)
    {
        if (Pet* pet = bot->GetPet())
            bot->RemovePet(pet, PET_SAVE_AS_DELETED);

        bot->UnsummonAllTotems();
        bot->RemoveAllControlled();
    }

    if (bot->IsBeingTeleportedFar())
        LOG_WARN("module.animus", "Bot {} is being teleported to map {} ({}, {}, {}); logout completes the teleport "
            "first", bot->GetName(), bot->GetTeleportDest().GetMapId(), bot->GetTeleportDest().GetPositionX(),
            bot->GetTeleportDest().GetPositionY(), bot->GetTeleportDest().GetPositionZ());

    sCharacterCache->DeleteCharacterCacheEntry(guid, bot->GetName());

    // Removes the player from its map, drops its social list and deletes it; false = no SaveToDB.
    session->LogoutPlayer(false);

    // A sim session's bind was never written to the character database; on a stock core it was, and goes with the bot.
    sInstanceSaveMgr->PlayerUnbindInstance(guid, mapId, difficulty, !CoreHooks::HasSimSessions());

    if (keepSession)
        return session;

    delete session;
    return nullptr;
}
