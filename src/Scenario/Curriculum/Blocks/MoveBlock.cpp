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

#include "MoveBlock.h"
#include "EncoderSupport.h"
#include "Layout.h"
#include <boost/json/object.hpp>
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
#include "Player.h"
#include "SeatView.h"
#include "TravelBlock.h"
#include <algorithm>
#include <cmath>

namespace
{
    using Animus::Curriculum::MoveBlock;
    namespace Encoding = Animus::Curriculum::Encoding;
    using Animus::Curriculum::TravelBlock;
    using Animus::Curriculum::SeatOptionKind;

    constexpr uint32 MOVE_POINT_ID = 0x4D56;    // "MV": this block's spline, distinct from the duel block's
    constexpr float YARD_SCALE = 40.0f;         // distances are reported as a fraction of this
    constexpr float OBJECTIVE_SCALE = 500.0f;   // an objective is further off than anything else it looks at
    constexpr float RUN_SPEED = 7.0f;           // yards a second, unmounted and unhasted (TravelBlock's)

    /// The world angle a bearing points at, given where the seat is looking. Bearings run clockwise from straight
    /// ahead, and WoW orientation runs counter-clockwise, so the eighth-turns are subtracted.
    float HeadingOf(float facing, uint32 bearing)
    {
        float const heading = facing - float(bearing) * float(M_PI) / 4.0f;
        return Position::NormalizeOrientation(heading);
    }

    /// `to`'s direction in the seat's own frame: 0 straight ahead, wrapped to (-pi, pi].
    float RelativeBearing(Position const& from, float facing, WorldObject const& to)
    {
        float const relative = from.GetAngle(to.GetPositionX(), to.GetPositionY()) - facing;
        return std::atan2(std::sin(relative), std::cos(relative));
    }

    float RelativeBearing(Position const& from, float facing, Position const& to)
    {
        float const relative = from.GetAngle(to.GetPositionX(), to.GetPositionY()) - facing;
        return std::atan2(std::sin(relative), std::cos(relative));
    }

    /// Off the ground, where the third dimension is real and pitch steers: swimming, or flying.
    bool Airborne(Player const* bot)
    {
        return bot && (bot->IsInWater() || bot->CanFly());
    }

    /// Bring the seat's own heading up to date for this decision, before anything is measured off it.
    ///
    /// `view.Facing` is the frame, not `bot->GetOrientation()`. The orientation belongs to whatever spline is
    /// running -- it is overwritten with the direction of travel every tick -- so measuring a bearing off it made
    /// the frame rotate by the bearing's own angle every decision, and a seat holding anything but straight ahead
    /// walked a spiral. Only BEARING_FORWARD was a fixed point, which is exactly the shape the failures had.
    ///
    /// Holding is therefore the default, including the 0xFF a seat starts an episode with: keep the heading you
    /// have unless you asked for something else. Facing along the path is the opt-in now, not the fallback.
    void UpdateFacing(Animus::Curriculum::SeatView& view)
    {
        Player const* bot = view.Bot;
        if (!bot)
            return;

        switch (view.FacingMode)
        {
            case MoveBlock::ACTION_FACE_TARGET:
                if (view.Target)
                    view.Facing = bot->GetAngle(view.Target);
                return;
            case MoveBlock::ACTION_FACE_OBJECTIVE:
                // Recomputed every decision, so with this held BEARING_FORWARD is the exact heading to the
                // objective continuously -- which is what the action was designed to mean and what the rotating
                // frame prevented it from meaning.
                if (view.HasObjective)
                    view.Facing = bot->GetAngle(view.Objective.GetPositionX(), view.Objective.GetPositionY());
                return;
            case MoveBlock::ACTION_FACE_HEADING:
                // Turn to face the way the feet are going -- and then, by construction, the way the feet are
                // going is straight ahead. Leaving the bearing where it was would rotate the frame again on the
                // next decision and re-create the spiral this whole change exists to remove: "face where I am
                // going" is a snap to a heading, not a standing instruction to keep turning.
                if (view.HeldBearing < MoveBlock::BEARING_COUNT)
                {
                    view.Facing = HeadingOf(view.Facing, view.HeldBearing);
                    view.HeldBearing = MoveBlock::BEARING_FORWARD;
                }
                return;
            case MoveBlock::ACTION_FACE_HOLD:
            default:
                return;                     // keep the heading it has, which is the point of both
        }
    }

    /// How far the seat could walk along `heading` before the ground stops cooperating: 1 for ground it could step
    /// onto at PROBE_YARDS, falling to 0 for a wall or a drop. One height sample a bearing -- the same call
    /// SnapToGround already makes every decision, eight times over rather than once.
    float GroundReach(Player* bot, float heading, float* stepOut = nullptr, float* waterOut = nullptr,
        float* burnsOut = nullptr)
    {
        Map* map = bot ? bot->GetMap() : nullptr;   // non-const: Map::GetLiquidData is not a const member
        if (!map)
            return 1.0f;

        float const x = bot->GetPositionX() + MoveBlock::PROBE_YARDS * std::cos(heading);
        float const y = bot->GetPositionY() + MoveBlock::PROBE_YARDS * std::sin(heading);
        float const from = bot->GetPositionZ();

        // Water before ground, because the ground test cannot tell a lake from a cliff and would call it the
        // latter. mmaps drops the terrain under real liquid and the bed is metres below the band a step is
        // judged in, so GetHeight comes back INVALID_HEIGHT over any water worth swimming -- the same answer it
        // gives for the edge of the map. Reported as its own feature and as walkable reach, because water is
        // somewhere the seat can go; what it costs to go there is OBS_SWIM_SPEED's to say.
        //
        // Which liquid it is decides all of that, and LiquidData::Flags is what carries it: Status only says how
        // deep the stuff is, so a test on Status alone called magma and slime "water" and handed the seat a lava
        // lake as ground it could cross. Swimmable is water and ocean. Magma and slime are neither ground nor
        // water but a way to die, so they read as no reach at all -- the same as a wall, which is the honest
        // answer until a stage teaches crossing them at the narrow point.
        LiquidData const liquid = map->GetLiquidData(bot->GetPhaseMask(), x, y, from,
            bot->GetCollisionHeight(), {});
        bool const liquidHere = liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > INVALID_HEIGHT
            && liquid.Level >= from - MoveBlock::MAX_STEP;
        bool const swimmable = liquidHere
            && (liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) != 0;
        bool const burns = liquidHere && (liquid.Flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME)) != 0;

        if (waterOut)
            *waterOut = swimmable ? 1.0f : 0.0f;
        if (burnsOut)
            *burnsOut = burns ? 1.0f : 0.0f;

        if (burns)
        {
            if (stepOut)
                *stepOut = 0.0f;
            return 0.0f;
        }

        if (swimmable)
        {
            if (stepOut)
                *stepOut = 0.0f;
            return 1.0f;
        }

        float const z = map->GetHeight(bot->GetPhaseMask(), x, y, from + MoveBlock::MAX_STEP, true,
            MoveBlock::MAX_STEP * 2.0f);

        if (z <= INVALID_HEIGHT)
        {
            if (stepOut)
                *stepOut = 0.0f;
            return 0.0f;
        }

        float const step = z - from;
        if (stepOut)
            *stepOut = std::clamp(step / MoveBlock::MAX_STEP, -1.0f, 1.0f);

        // A wall and a cliff are both "not that way" for something on legs, so the size of the change is what is
        // reported rather than its sign; the sign is reported separately, straight ahead only.
        return std::max(0.0f, 1.0f - std::fabs(step) / MoveBlock::MAX_STEP);
    }

    /// One step of a held turn, and one of a held pitch. Exactly once per decision, whoever asks: BeforeApply
    /// on a decision the key is merely held, Apply on the decision it goes down, so the first press does
    /// something rather than waiting 250 ms for the next decision to notice it.
    void StepTurn(Animus::Curriculum::SeatView& view)
    {
        Player const* bot = view.Bot;
        if (!bot || view.Turning == 0 || !bot->IsAlive() || bot->HasUnitState(Encoding::IMMOBILE_STATES))
            return;

        // Straight onto the seat's own heading. SetFacingTo launches an orientation-only spline, and the move
        // spline Steer issues does MotionMaster::Clear() and replaces it before a single world tick can apply
        // it -- so a held turn did nothing at all while the seat was walking, which is every decision that
        // matters. It also left GetOrientation() unchanged for the heading Steer computes, so the turn did not
        // even steer the decision it was pressed on.
        view.Facing = Position::NormalizeOrientation(view.Facing + float(view.Turning) * MoveBlock::TURN_STEP);
        // Turning is taking hold of the head by hand. Leaving the mode alone would have FACE_OBJECTIVE overwrite
        // the turn inside UpdateFacing a moment later, so the turn would be invisible again for a different
        // reason.
        if (view.FacingMode == MoveBlock::ACTION_FACE_TARGET
            || view.FacingMode == MoveBlock::ACTION_FACE_OBJECTIVE
            || view.FacingMode == MoveBlock::ACTION_FACE_HEADING)
            view.FacingMode = MoveBlock::ACTION_FACE_HOLD;
    }

    void StepPitch(Animus::Curriculum::SeatView& view)
    {
        if (view.PitchTurning != 0 && Airborne(view.Bot))
            view.Pitch = std::clamp(view.Pitch + float(view.PitchTurning) * MoveBlock::PITCH_STEP,
                -MoveBlock::PITCH_MAX, MoveBlock::PITCH_MAX);
    }

    /// Settle where the seat is looking and carry it -- on the move spline if the feet are going somewhere, as a
    /// turn on the spot if they are not.
    ///
    /// Split out of BeforeApply so that it can be re-run the moment an action changes the steering, without
    /// re-running the things that must happen exactly once a decision. Pressing a facing or a bearing used to
    /// call the whole of BeforeApply again, which stepped a held turn a second time in the same 250 ms: the turn
    /// rate doubled whenever the policy did anything else while turning.
    void Steer(Animus::Curriculum::SeatView& view)
    {
        Player* bot = view.Bot;
        if (!bot || !view.Option)
            return;

        bool const alive = bot->IsAlive() && !bot->HasUnitState(Encoding::IMMOBILE_STATES);

        // Where the head is pointing this decision, settled before a single bearing is measured off it.
        UpdateFacing(view);

        if (!view.Option->Running(SeatOptionKind::MoveBearing, view.NowMs)
            || view.HeldBearing >= MoveBlock::BEARING_COUNT)
        {
            view.HeldBearing = 0xFF;
            // Standing still, so no move spline will carry the heading: push it onto the unit here instead, or a
            // turn and a FACE_* would be things the seat believed about itself that the world did not share.
            // UpdatePosition with the same coordinates is a pure turn, and it is a no-op when the angle is unchanged.
            if (alive)
                bot->UpdatePosition(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), view.Facing);
            return;
        }

        if (!alive)
            return;

        // Re-aimed from where the seat is now, every decision it keeps walking. Aiming once at a point chosen when the
        // key went down would walk it into the first wall the ground put in the way; recomputing lets the path bend.
        float const heading = HeadingOf(view.Facing, view.HeldBearing);
        bool const airborne = Airborne(bot);
        float const pitch = airborne ? view.Pitch : 0.0f;
        float const reach = MoveBlock::STEP_YARDS * std::cos(pitch);

        Position destination = *bot;
        destination.Relocate(bot->GetPositionX() + reach * std::cos(heading),
            bot->GetPositionY() + reach * std::sin(heading),
            bot->GetPositionZ() + MoveBlock::STEP_YARDS * std::sin(pitch));

        if (airborne)
        {
            // Swimming and flying are steered in three dimensions and must not be snapped to the ground: the whole
            // point of a pitch is to leave it. A climb still stops at the ceiling the air has.
            float const facing = view.Facing;
            if (!bot->CanFly())
            {
                // In the water. Keep the seat under the surface rather than skimming along the top of it, and swim
                // rather than fly: a spline with the fly flag on a swimmer is a different animal.
                Encoding::SwimTo(bot, destination.GetPositionX(), destination.GetPositionY(),
                    destination.GetPositionZ(), &facing);
                return;
            }

            float const ceiling = bot->GetPositionZ()
                + (TravelBlock::MAX_ALTITUDE - TravelBlock::HeightAboveGround(bot));
            destination.Relocate(destination.GetPositionX(), destination.GetPositionY(),
                std::min(destination.GetPositionZ(), ceiling));

            Encoding::FlyTo(bot, destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(),
                &facing);
            return;
        }

        // On land, but the step leads into water. This is the one move the seat could never make: the walkable mesh
        // ends at the waterline, so a pathfound ground step into a lake has nowhere to land and the seat stops on
        // the shore -- and it could not start swimming, because swimming was only ever reached by already being in
        // the water. Over 200 sampled decisions across a whole run, no seat ever got its feet below the surface;
        // every sample near water sat 0.1 to 0.4 yards above it. Entering is therefore its own case: go straight in,
        // to just under the surface, and from the next decision `airborne` is true and the seat is swimming.
        if (Map* map = bot->GetMap())
        {
            LiquidData const liquid = map->GetLiquidData(bot->GetPhaseMask(), destination.GetPositionX(),
                destination.GetPositionY(), bot->GetPositionZ(), bot->GetCollisionHeight(), {});
            // Water and ocean only: stepping into magma or slime is not a crossing, it is a death, and the probe
            // reports it as no reach for that reason.
            if (liquid.Status != LIQUID_MAP_NO_WATER && liquid.Level > INVALID_HEIGHT
                && (liquid.Flags & (MAP_LIQUID_TYPE_WATER | MAP_LIQUID_TYPE_OCEAN)) != 0
                && liquid.Level >= bot->GetPositionZ() - MoveBlock::MAX_STEP)
            {
                Encoding::SwimTo(bot, destination.GetPositionX(), destination.GetPositionY(),
                    liquid.Level - bot->GetCollisionHeight() * 0.5f, &view.Facing);
                return;
            }
        }

        // Put the destination on the ground where there is ground to put it on. Where there is not -- the bearing
        // leads off the map, or over a drop deeper than a step -- the move is issued anyway, at the unsnapped point.
        //
        // Refusing to move in that case is what it looked like it should do, and it is wrong: the seat then stands
        // still, and standing still is the one outcome this whole block exists to prevent. The direction is still the
        // policy's; only the height of a point eight yards away is being guessed at, and the path the spline takes
        // sorts that out. The ground probe is how the seat learns not to choose such a bearing in the first place.
        // Deliberately discarded: see above -- a failure is a reason to move anyway, not a reason to stand still.
        (void)Encoding::SnapToGround(bot->GetMap(), bot->GetPhaseMask(), destination, bot->GetPositionZ(),
            MoveBlock::STEP_YARDS);

        // Pathfinding on, which is the default: a bearing is where the seat wants to go, not a licence to walk through
        // a wall to get there.
        Encoding::MoveTo(bot, MOVE_POINT_ID, destination.GetPositionX(), destination.GetPositionY(),
            destination.GetPositionZ(), &view.Facing);
    }
}


Animus::Curriculum::BlockSize Animus::Curriculum::MoveBlock::Size(Layout const& /*layout*/) const
{
    return { OBS_COUNT, ACTION_COUNT };
}

void Animus::Curriculum::MoveBlock::DescribeManifest(Layout const& /*layout*/, boost::json::object& block) const
{
    block["bearings"] = uint32(BEARING_COUNT);
    block["step_yards"] = double(STEP_YARDS);
    block["turn_step"] = double(TURN_STEP);
    block["pitch_step"] = double(PITCH_STEP);
    block["pitch_max"] = double(PITCH_MAX);
    block["probe_yards"] = double(PROBE_YARDS);
}

std::string Animus::Curriculum::MoveBlock::ActionName(Layout const& /*layout*/, uint32 local) const
{
    static constexpr std::array<char const*, ACTION_COUNT> NAMES =
    {
        "move_forward", "move_forward_right", "move_right", "move_back_right",
        "move_back", "move_back_left", "move_left", "move_forward_left",
        "halt", "face_target", "face_heading", "face_hold", "face_objective",
        "turn_left", "turn_right", "pitch_up", "pitch_down", "pitch_level",
    };

    return local < NAMES.size() ? NAMES[local] : std::string();
}

void Animus::Curriculum::MoveBlock::Observe(SeatView const& view, float* obs, uint8* mask) const
{
    // `obs` and `mask` are already this block's own slice of the seat's row: SeatEncoder::Observe offsets them
    // before it calls a block. Offsetting again wrote the whole block past the end of its slice, so every bearing
    // stayed masked and no seat could steer (stage1_move, 2026-09-21).
    float* out = obs;
    Player* bot = view.Bot;

    // Everything a seat needs to place its feet, and nothing about whether it has an enemy: this block is the one
    // that still works when there is nothing to fight.
    bool const canMove = bot && bot->IsAlive() && !bot->HasUnitState(Encoding::IMMOBILE_STATES);
    bool const airborne = Airborne(bot);

    if (bot)
    {
        // Every bearing in this block is measured off the seat's own heading, so the observation has to report
        // that and not the spline's idea of it.
        float const facing = view.Facing;
        out[OBS_MOVING] = bot->isMoving() ? 1.0f : 0.0f;
        out[OBS_SPEED] = bot->GetSpeed(MOVE_RUN) / RUN_SPEED;
        out[OBS_FACING_SIN] = std::sin(facing);
        out[OBS_FACING_COS] = std::cos(facing);

        if (view.HeldBearing < BEARING_COUNT)
            out[OBS_BEARING_HELD + view.HeldBearing] = 1.0f;
        else
            out[OBS_BEARING_NONE] = 1.0f;

        out[OBS_TURNING_LEFT] = view.Turning < 0 ? 1.0f : 0.0f;
        out[OBS_TURNING_RIGHT] = view.Turning > 0 ? 1.0f : 0.0f;
        out[OBS_PITCH_SIN] = std::sin(view.Pitch);
        out[OBS_PITCH_COS] = std::cos(view.Pitch);

        if (Unit const* target = view.Target)
        {
            float const relative = RelativeBearing(*bot, facing, *target);
            out[OBS_TARGET_BEARING_SIN] = std::sin(relative);
            out[OBS_TARGET_BEARING_COS] = std::cos(relative);
            out[OBS_TARGET_DISTANCE] = std::min(1.0f, bot->GetExactDist2d(target) / YARD_SCALE);
        }

        // The nearest ground effect it is not standing in, in the same frame: which way it lies and how wide, so
        // the seat can walk round one rather than only out of one. Its bearing is already relative to facing.
        if (view.NearestHazard.Present)
        {
            out[OBS_HAZARD_BEARING_SIN] = std::sin(view.NearestHazard.Bearing);
            out[OBS_HAZARD_BEARING_COS] = std::cos(view.NearestHazard.Bearing);
            out[OBS_HAZARD_DISTANCE] = std::min(1.0f, view.NearestHazard.Distance / YARD_SCALE);
            out[OBS_HAZARD_RADIUS] = std::min(1.0f, view.NearestHazard.Radius / YARD_SCALE);
        }

        if (view.HasObjective)
        {
            float const relative = RelativeBearing(*bot, facing, view.Objective);
            out[OBS_OBJECTIVE] = 1.0f;
            out[OBS_OBJECTIVE_BEARING_SIN] = std::sin(relative);
            out[OBS_OBJECTIVE_BEARING_COS] = std::cos(relative);
            out[OBS_OBJECTIVE_DISTANCE] = std::min(1.0f, bot->GetExactDist2d(&view.Objective) / OBJECTIVE_SCALE);
        }

        // What the ground is like each way it could go. Skipped in the air and in the water, where the ground is
        // not what the seat is steering against and the samples would only report the bottom.
        if (!airborne)
        {
            for (uint32 bearing = 0; bearing < BEARING_COUNT; ++bearing)
            {
                float step = 0.0f;
                float wet = 0.0f;
                float hot = 0.0f;
                out[OBS_GROUND_FIRST + bearing] = GroundReach(bot, HeadingOf(facing, bearing), &step, &wet, &hot);
                out[OBS_STEP_FIRST + bearing] = step;
                out[OBS_WATER_FIRST + bearing] = wet;
                out[OBS_BURNS_FIRST + bearing] = hot;
            }
        }
        else
            for (uint32 bearing = 0; bearing < BEARING_COUNT; ++bearing)
            {
                // Off the ground there is nothing underfoot to walk onto or refuse: every way is open, the ground
                // changes by nothing, and a seat that is swimming is surrounded by the water it is in.
                out[OBS_GROUND_FIRST + bearing] = 1.0f;
                out[OBS_WATER_FIRST + bearing] = bot->IsInWater() ? 1.0f : 0.0f;
            }

        out[OBS_IN_WATER] = bot->IsInWater() ? 1.0f : 0.0f;
        out[OBS_SUBMERGED] = bot->IsUnderWater() ? 1.0f : 0.0f;
        out[OBS_SUBMERGED_TIME] = std::min(1.0f, view.SubmergedTime / BREATH_SECONDS);
        out[OBS_SWIM_SPEED] = bot->GetSpeed(MOVE_SWIM) / RUN_SPEED;
        out[OBS_AIRBORNE] = airborne ? 1.0f : 0.0f;
    }

    // The way round against the way through, and whether the legs are getting anywhere. All three are the
    // scenario's to measure -- one at the episode's build, two over the last second -- because none of them can
    // be seen from a probe of any length.
    out[OBS_DETOUR] = std::clamp(view.Detour / 4.0f, 0.0f, 1.0f);
    out[OBS_MOVE_RATE] = std::clamp(view.MoveRate, 0.0f, 1.0f);
    out[OBS_CLOSE_RATE] = std::clamp(view.CloseRate, -1.0f, 1.0f);

    if (!mask)
        return;

    uint8* allowed = mask;
    for (uint32 bearing = 0; bearing < BEARING_COUNT; ++bearing)
        allowed[ACTION_BEARING_FIRST + bearing] = canMove ? 1 : 0;

    // The bearing already being walked is masked: pressing it again would be a repeat of a key already held, and
    // the option machinery re-issues the spline each decision without being asked.
    if (canMove && view.HeldBearing < BEARING_COUNT && view.Option
        && view.Option->Running(SeatOptionKind::MoveBearing, view.NowMs))
        allowed[ACTION_BEARING_FIRST + view.HeldBearing] = 0;

    // Halting is only worth offering while something is being walked.
    allowed[ACTION_HALT] = canMove && view.HeldBearing < BEARING_COUNT ? 1 : 0;

    // Where it looks is a choice it can always make, alive and able to turn. Facing the target needs one, and
    // facing the objective needs somewhere to be going.
    bool const canTurn = bot && bot->IsAlive() && !bot->HasUnitState(Encoding::STUN_STATES);
    allowed[ACTION_FACE_TARGET] = canTurn && view.Target && view.Target->IsAlive() ? 1 : 0;
    allowed[ACTION_FACE_HEADING] = canTurn ? 1 : 0;
    allowed[ACTION_FACE_HOLD] = canTurn ? 1 : 0;
    allowed[ACTION_FACE_OBJECTIVE] = canTurn && view.HasObjective ? 1 : 0;
    for (uint32 face = ACTION_FACE_TARGET; face <= ACTION_FACE_OBJECTIVE; ++face)
        if (view.FacingMode == face)
            allowed[face] = 0;              // already holding its head that way

    // Turning is the mouse-look and is always available to something that can turn at all; the one being held is
    // masked for the same reason a held bearing is.
    allowed[ACTION_TURN_LEFT] = canTurn && view.Turning >= 0 ? 1 : 0;
    allowed[ACTION_TURN_RIGHT] = canTurn && view.Turning <= 0 ? 1 : 0;

    // Pitch only means something off the ground. On foot the ground decides the seat's height, so the three
    // actions are masked rather than merely useless -- a masked action cannot be explored into.
    bool const canPitch = canTurn && airborne;
    allowed[ACTION_PITCH_UP] = canPitch && view.Pitch < PITCH_MAX ? 1 : 0;
    allowed[ACTION_PITCH_DOWN] = canPitch && view.Pitch > -PITCH_MAX ? 1 : 0;
    allowed[ACTION_PITCH_LEVEL] = canPitch && std::fabs(view.Pitch) > 0.01f ? 1 : 0;
}

void Animus::Curriculum::MoveBlock::BeforeApply(SeatView& view, SeatActionResult& /*result*/) const
{
    if (!view.Bot || !view.Option)
        return;

    // The held keys come up on their own when their clocks run out, and what they turned to is kept.
    if (!view.Option->Running(SeatOptionKind::MoveTurn, view.NowMs))
        view.Turning = 0;
    if (!view.Option->Running(SeatOptionKind::MovePitch, view.NowMs))
        view.PitchTurning = 0;

    // A held turn swings the seat a little further every decision it stays down, which is what makes every
    // heading between two compass points reachable. It happens before the feet are re-aimed, so a bearing walked
    // under a turn curves rather than stepping.
    StepTurn(view);
    StepPitch(view);
    Steer(view);
}

void Animus::Curriculum::MoveBlock::Apply(SeatView& view, uint32 local, SeatActionResult& /*result*/) const
{
    Player* bot = view.Bot;
    if (!bot || !view.Option)
        return;

    if (local < ACTION_HALT)
    {
        view.HeldBearing = uint8(local - ACTION_BEARING_FIRST);
        view.Option->Start(SeatOptionKind::MoveBearing, view.NowMs + view.Options.MoveBearingMs);
        // Steer walks it on the next decision anyway; do it now so the seat is not still for one.
        Steer(view);
        return;
    }

    if (local == ACTION_HALT)
    {
        view.HeldBearing = 0xFF;
        view.Option->Stop(SeatOptionKind::MoveBearing);
        bot->StopMoving();
        return;
    }

    if (local <= ACTION_FACE_OBJECTIVE)
    {
        view.FacingMode = uint8(local);
        // Steer settles the heading and carries it, whether the seat is walking (on the move spline) or standing
        // still (as a turn on the spot). Choosing where to look should do something on the decision it is
        // chosen, not on the next one the seat happens to move.
        Steer(view);
        return;
    }

    if (local == ACTION_TURN_LEFT || local == ACTION_TURN_RIGHT)
    {
        view.Turning = local == ACTION_TURN_LEFT ? -1 : 1;
        view.Option->Start(SeatOptionKind::MoveTurn, view.NowMs + view.Options.MoveTurnMs);
        // Turn now rather than a decision from now, so the first press of a key does something. Only the turn:
        // re-running the whole of BeforeApply would step the turn a second time in the same 250 ms.
        StepTurn(view);
        Steer(view);
        return;
    }

    if (local == ACTION_PITCH_UP || local == ACTION_PITCH_DOWN)
    {
        view.PitchTurning = local == ACTION_PITCH_UP ? 1 : -1;
        view.Option->Start(SeatOptionKind::MovePitch, view.NowMs + view.Options.MovePitchMs);
        StepPitch(view);
        Steer(view);
        return;
    }

    if (local == ACTION_PITCH_LEVEL)
    {
        view.Pitch = 0.0f;
        view.PitchTurning = 0;
        view.Option->Stop(SeatOptionKind::MovePitch);
    }
}
