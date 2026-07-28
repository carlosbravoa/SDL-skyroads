#include "core/gameplay.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>

namespace skyroads::core {
namespace {

using skyroads::data::GROUND_Y_128;
using skyroads::data::LEVEL_CENTER_X_128;
using skyroads::data::LEVEL_MAX_X_128;
using skyroads::data::LEVEL_MIN_X_128;

// Round to nearest with halves away from zero, after a division by a positive
// power-of-two denominator: the integer image of the reference's
// round16_nearest (f64::round / std::round both round half away from zero).
int32_t round_half_away(int32_t value, int32_t denominator) {
    const int32_t half = denominator / 2;
    return (value >= 0 ? value + half : value - half) / denominator;
}

// The reference compares position deltas against 0.01 to separate "really
// moved" from float fuzz. On the 1/128 grid the smallest nonzero |delta|
// exceeding 0.01 is 2/128, exactly.
bool differs_by_more_than_hundredth_128(int32_t a_128, int32_t b_128) {
    return std::abs(a_128 - b_128) >= 2;
}

bool in_range_inclusive(int32_t value, int32_t lo, int32_t hi) {
    return value >= lo && value <= hi;
}

} // namespace

ControllerState ControllerState::make(int8_t turn_input, int8_t accel_input,
                                      bool jump_input) {
    assert(turn_input >= -1 && turn_input <= 1);
    assert(accel_input >= -1 && accel_input <= 1);
    return ControllerState{turn_input, accel_input, jump_input};
}

Ship::Ship()
    : x_position_128(LEVEL_CENTER_X_128),
      y_position_128(GROUND_Y_128),
      z_position_fp16(3 << 16),
      slide_amount_128(0),
      sliding_accel(0),
      x_movement_base_128(0),
      y_velocity_128(0),
      z_velocity_fp16(0),
      fuel_remaining(0x7530),
      oxygen_remaining(0x7530),
      offset_at_which_not_inside_tile(0),
      is_on_ground(true),
      is_going_up(false),
      has_run_jump_o_master(false),
      jump_o_master_velocity_delta_fp16(0),
      jump_o_master_in_use(false),
      jumped_from_y_position_128(0),
      state(ShipState::Alive) {}

bool Ship::is_different_height(const Ship& other) const {
    return differs_by_more_than_hundredth_128(other.y_position_128,
                                              y_position_128);
}

std::vector<GameplayEvent> Ship::update(const Level& level, Ship& expected,
                                        ControllerState controls) {
    std::vector<GameplayEvent> events;
    // The reference re-quantized ("sanitized") its doubles here and after every
    // motion step; integer fields are on-grid by construction, so the only
    // surviving quantization is the explicit round in attempt_motion.
    const bool can_control = state == ShipState::Alive;

    const LevelCell cell =
        level.get_cell_fp16(x_position_128 * 512, z_position_fp16);
    const bool is_above_nothing = cell.is_empty();
    const TouchEffect touch_effect = get_touch_effect(cell);
    const bool is_on_sliding_tile = touch_effect == TouchEffect::Slide;
    const bool is_on_decel_pad = touch_effect == TouchEffect::Decelerate;

    apply_touch_effect(touch_effect, events);
    update_y_velocity(expected, level, events);
    update_z_velocity(can_control, controls.accel_input);
    update_x_velocity(can_control, controls.turn_input, is_on_sliding_tile,
                      is_above_nothing);
    update_jump(can_control, is_above_nothing, controls.jump_input, level);
    update_jump_o_master(controls, level);
    update_gravity(level.gravity_acceleration_128());

    expected = *this;
    expected.attempt_motion(is_on_decel_pad);
    move_to(expected, level);
    handle_bumps(expected, level, events);
    handle_collision(expected, events);
    handle_slide_collision(expected);
    handle_bounce(expected, level);
    handle_oxygen_and_fuel(level);
    handle_fall_below_ground();

    return events;
}

TouchEffect Ship::get_touch_effect(const LevelCell& cell) const {
    if (!is_on_ground) {
        return TouchEffect::None;
    }
    const int32_t y_units = y_position_128 >> 7; // floor(y); y >= 0 on ground
    if (y_units == GROUND_Y_128 / 128 && cell.has_tile) {
        return cell.tile_effect;
    }
    if (y_units > GROUND_Y_128 / 128 && cell.cube_height.has_value() &&
        *cell.cube_height == static_cast<uint16_t>(y_units)) {
        return cell.cube_effect;
    }
    return TouchEffect::None;
}

void Ship::apply_touch_effect(TouchEffect effect,
                              std::vector<GameplayEvent>& events) {
    switch (effect) {
        case TouchEffect::Accelerate: // @0x1b2f
            z_velocity_fp16 += 0x12F;
            break;
        case TouchEffect::Decelerate: // @0x1b17
            z_velocity_fp16 -= 0x12F;
            break;
        case TouchEffect::Kill:
            if (state != ShipState::Exploded) {
                events.push_back(GameplayEvent::ShipExploded);
            }
            state = ShipState::Exploded;
            break;
        case TouchEffect::RefillOxygen:
            if (state == ShipState::Alive) {
                if (fuel_remaining < 0x6978 || oxygen_remaining < 0x6978) {
                    events.push_back(GameplayEvent::ShipRefilled);
                }
                fuel_remaining = 0x7530;
                oxygen_remaining = 0x7530;
            }
            break;
        case TouchEffect::Slide:
        case TouchEffect::None:
            break;
    }
    clamp_global_z_velocity();
}

void Ship::update_y_velocity(const Ship& expected, const Level& level,
                             std::vector<GameplayEvent>& events) {
    if (is_different_height(expected)) {
        if (slide_amount_128 == 0 || offset_at_which_not_inside_tile >= 2) {
            // EXE (@0x243c-0x244d): the bounce threshold is the INTEGER
            // quotient (0x104 * gravity) / 8, and the comparison is |y_vel| >=
            // threshold ("jae"). The double form compared against the unfloored
            // gravity * 0x104 / 8.0 with a strict >, which differs only when
            // |y_vel| lands exactly on the (floored) threshold.
            const int32_t threshold =
                0x104 * static_cast<int32_t>(level.gravity) / 8;
            if (std::abs(y_velocity_128) >= threshold) {
                if (y_velocity_128 < 0) {
                    events.push_back(GameplayEvent::ShipBounced);
                }
                // EXE (@0x2485): bounce = -(y_vel_raw * 5) / 10 on the raw
                // 1/128 fixed-point value, with integer truncation -- not float
                // -0.5*v.
                y_velocity_128 = -(y_velocity_128 * 5) / 10;
            } else {
                y_velocity_128 = 0;
            }
        } else {
            y_velocity_128 = 0;
        }
    }
}

void Ship::update_z_velocity(bool can_control, int8_t accel_input) {
    // @0x24a8-0x24bd: z_vel += accel * 0x4B, then the 0..0x2AAA clamp.
    z_velocity_fp16 +=
        (can_control ? static_cast<int32_t>(accel_input) : 0) * 0x4B;
    clamp_global_z_velocity();
}

void Ship::update_x_velocity(bool can_control, int8_t turn_input,
                             bool is_on_sliding_tile, bool is_above_nothing) {
    if (!is_on_sliding_tile) {
        const bool can_control_1 =
            (is_going_up || is_above_nothing) && x_movement_base_128 == 0 &&
            y_velocity_128 > 0 &&
            (y_position_128 - jumped_from_y_position_128) < 30 * 128; // @0x2544
        const bool can_control_2 = !is_going_up && !is_above_nothing;
        if (can_control_1 || can_control_2) {
            x_movement_base_128 =
                can_control ? static_cast<int32_t>(turn_input) * 0x1D : 0;
        }
    }
}

void Ship::update_jump(bool can_control, bool is_above_nothing, bool jump_input,
                       const Level& level) {
    if (!is_going_up && !is_above_nothing && jump_input && level.gravity < 0x14 &&
        can_control) {
        y_velocity_128 = 0x480; // @0x257a
        is_going_up = true;
        jumped_from_y_position_128 = y_position_128;
    }
}

void Ship::update_jump_o_master(ControllerState controls, const Level& level) {
    if (is_going_up && !has_run_jump_o_master &&
        y_position_128 >= 110 * 128) { // @0x25ac: 0x3700
        run_jump_o_master(controls, level);
        has_run_jump_o_master = true;
    }
}

void Ship::update_gravity(int32_t gravity_acceleration_128) {
    // EXE (@0x25c9): threshold is y >= 0x2800 (raw) = GROUND_Y (80.0); the port
    // had 0x28 (40.0), a dropped-digit transcription. Terminal fall velocity is
    // -106 (0xff96), not -105. The gravity acceleration is integer raw 1/128
    // already, so the reference's post-add s_floor was a no-op and is gone.
    if (y_position_128 >= GROUND_Y_128) {
        y_velocity_128 += gravity_acceleration_128;
    } else if (y_velocity_128 > -106) {
        y_velocity_128 = -106;
    }
}

void Ship::attempt_motion(bool on_decel_pad) {
    int32_t motion_vel = z_velocity_fp16;
    if (!on_decel_pad) {
        motion_vel += 0x618; // @0x2646
    }
    // @0x265a-0x2681: expected_x = x + (x_movement_base * motion_vel) / 0x200
    // + slide. The divide helper @0x5e1c takes the absolute value of both
    // operands, does an unsigned div and re-applies the sign, i.e. it TRUNCATES
    // toward zero. C++ integer division does the same, so this is a plain /.
    const int32_t x_motion_512 = x_movement_base_128 * motion_vel;
    x_position_128 += slide_amount_128 + x_motion_512 / 512;
    // Motion keeps being integrated after death: the EXE's motion integrator
    // (@0x1900..0x1a9b) has no death-code gate, which is what lets a ship that
    // fell off the road keep dropping out of view. Only the *inputs* are cut
    // (can_control), so x_movement_base decays to 0 on its own.
    y_position_128 += y_velocity_128;
    z_position_fp16 += z_velocity_fp16;
}

// EXE (@0x17fd-0x18d0): each substep position is pos + (delta * fifths) / 5
// per axis, with the idiv/ldiv truncation toward zero. The double form floored
// `pos + delta * (fifths / 5.0)` instead, which differs for negative deltas
// that are not multiples of 5 (and 0.2 is not exact in binary anyway); the
// EXE's truncating form is authoritative.
void Ship::interp(const Ship& dest, int32_t fifths) {
    x_position_128 += (dest.x_position_128 - x_position_128) * fifths / 5;
    y_position_128 += (dest.y_position_128 - y_position_128) * fifths / 5;
    z_position_fp16 += (dest.z_position_fp16 - z_position_fp16) * fifths / 5;
}

void Ship::move_to(const Ship& dest, const Level& level) {
    if (x_position_128 == dest.x_position_128 &&
        y_position_128 == dest.y_position_128 &&
        z_position_fp16 == dest.z_position_fp16) {
        return;
    }

    Ship fake = *this;
    int32_t iter = 1;
    for (int32_t step = 1; step <= 5; ++step) {
        fake = *this;
        fake.interp(dest, step);
        if (level.is_inside_tile_128(fake.x_position_128, fake.y_position_128,
                                     fake.z_position_fp16)) {
            iter = step;
            break;
        }
        iter = step + 1;
    }

    interp(dest, iter - 1);

    // @0x18d4-0x1956: z granularity walk, 0x1000 shrinking by /16 (floor; the
    // value stays non-negative, so the reference's floor32 was the same shift).
    int32_t z_gran = 0x1000;
    while (z_gran != 0) {
        if (dest.z_position_fp16 - z_position_fp16 >= z_gran &&
            !level.is_inside_tile_128(x_position_128, y_position_128,
                                      z_position_fp16 + z_gran)) {
            z_position_fp16 += z_gran;
        } else {
            z_gran /= 16;
        }
    }

    // X/Y granularity walks: 0x7D shrinking by a truncating /5 (the
    // reference's s_floor(gran / 5.0 * 128.0) is exactly raw/5 truncated:
    // 125 -> 25 -> 5 -> 1 -> 0).
    int32_t x_gran =
        dest.x_position_128 > x_position_128 ? 0x7D : -0x7D;
    while (x_gran != 0) {
        if (std::abs(dest.x_position_128 - x_position_128) >=
                std::abs(x_gran) &&
            !level.is_inside_tile_128(x_position_128 + x_gran, y_position_128,
                                      z_position_fp16)) {
            x_position_128 += x_gran;
        } else {
            x_gran /= 5;
        }
    }

    int32_t y_gran =
        dest.y_position_128 > y_position_128 ? 0x7D : -0x7D;
    while (y_gran != 0) {
        if (std::abs(dest.y_position_128 - y_position_128) >=
                std::abs(y_gran) &&
            !level.is_inside_tile_128(x_position_128, y_position_128 + y_gran,
                                      z_position_fp16)) {
            y_position_128 += y_gran;
        } else {
            y_gran /= 5;
        }
    }
}

void Ship::handle_bumps(Ship& expected, const Level& level,
                        std::vector<GameplayEvent>& events) {
    if (z_position_fp16 != expected.z_position_fp16 &&
        level.is_inside_tile_128(x_position_128, y_position_128,
                                 expected.z_position_fp16)) {
        const int32_t bump_off = 0x3A0; // @0x272f/@0x276c
        if (!level.is_inside_tile_128(x_position_128 - bump_off,
                                      y_position_128,
                                      expected.z_position_fp16)) {
            x_position_128 -= bump_off;
            expected.z_position_fp16 = z_position_fp16;
            events.push_back(GameplayEvent::ShipBumpedWall);
        } else if (!level.is_inside_tile_128(x_position_128 + bump_off,
                                             y_position_128,
                                             expected.z_position_fp16)) {
            x_position_128 += bump_off;
            expected.z_position_fp16 = z_position_fp16;
            events.push_back(GameplayEvent::ShipBumpedWall);
        }
    }
}

void Ship::handle_collision(const Ship& expected,
                            std::vector<GameplayEvent>& events) {
    if (std::abs(z_position_fp16 - expected.z_position_fp16) >= 656) {
        // EXE (@0x27ae): survivable if z_vel < 0xE38 -- the raw third of
        // 0x2AAA, already truncated. The double (1/3) * 0x2AAA/65536 landed a
        // whisker above it, so only an exact z_vel of 0xE38 decides
        // differently (the EXE explodes; the double form bumped).
        if (z_velocity_fp16 < 0xE38) {
            z_velocity_fp16 = 0;
            events.push_back(GameplayEvent::ShipBumpedWall);
        } else if (state != ShipState::Exploded) {
            state = ShipState::Exploded;
            events.push_back(GameplayEvent::ShipExploded);
        }
    }
}

void Ship::handle_slide_collision(Ship& expected) {
    if (differs_by_more_than_hundredth_128(x_position_128,
                                           expected.x_position_128)) {
        x_movement_base_128 = 0;
        if (slide_amount_128 != 0) {
            expected.x_position_128 = x_position_128;
            slide_amount_128 = 0;
        }
        z_velocity_fp16 -= 0x97; // @0x2862
        clamp_global_z_velocity();
    }
}

void Ship::handle_bounce(const Ship& expected, const Level& level) {
    is_on_ground = false;
    if (y_velocity_128 < 0 && expected.y_position_128 != y_position_128) {
        z_velocity_fp16 += jump_o_master_velocity_delta_fp16;
        jump_o_master_velocity_delta_fp16 = 0;
        has_run_jump_o_master = false;
        jump_o_master_in_use = false;
        is_going_up = false;
        is_on_ground = true;
        sliding_accel = 0;

        // @0x2958-0x29e9: probe x +/- i whole units at y - 1/128.
        for (int32_t i = 1; i <= 0xE; ++i) {
            if (!level.is_inside_tile_128(x_position_128 + i * 128,
                                          y_position_128 - 1,
                                          z_position_fp16)) {
                sliding_accel += 1;
                offset_at_which_not_inside_tile = static_cast<int16_t>(i);
                break;
            }
        }

        for (int32_t i = 1; i <= 0xE; ++i) {
            if (!level.is_inside_tile_128(x_position_128 - i * 128,
                                          y_position_128 - 1,
                                          z_position_fp16)) {
                sliding_accel -= 1;
                offset_at_which_not_inside_tile = static_cast<int16_t>(i);
                break;
            }
        }

        if (sliding_accel != 0) {
            slide_amount_128 += 0x11 * sliding_accel; // @0x29f7
        } else {
            slide_amount_128 = 0;
        }
    }
}

void Ship::handle_oxygen_and_fuel(const Level& level) {
    // Oxygen burns on TIME (a fixed slice of the tank every tick) and fuel
    // burns on DISTANCE (proportional to forward speed), so a stationary ship
    // still suffocates but uses no fuel. Both are pure 16-bit integer math in
    // the EXE (@0x2a23-0x2a85): the per-tick decrements are the truncated
    // quotients below, and the "went past empty" test is an unsigned-underflow
    // clamp. The double form kept fractional tanks; the integer one drains
    // oxygen slightly slower (e.g. 13 vs 13.888../tick on the demo road) and
    // does not drain fuel at all until z_vel reaches 0x10000 / (0x7530 /
    // level.fuel) raw -- both exactly as the original.
    const int32_t oxygen_dec =
        level.oxygen > 0 ? 0x7530 / (0x24 * static_cast<int32_t>(level.oxygen))
                         : 0x7530;
    oxygen_remaining -= oxygen_dec;
    if (oxygen_remaining <= 0) oxygen_remaining = 0;

    const int32_t fuel_dec =
        level.fuel > 0
            ? ((0x7530 / static_cast<int32_t>(level.fuel)) * z_velocity_fp16) >>
                  16
            : 0x7530;
    fuel_remaining -= fuel_dec;
    if (fuel_remaining <= 0) fuel_remaining = 0;

    // The EXE assigns the outcome unconditionally in the order fuel then oxygen
    // (@0x2ab0, @0x2ac0), so when both empty on the same tick OXYGEN is the one
    // that sticks. Keep that order; it decides which dashboard label flashes.
    if (fuel_remaining <= 0) state = ShipState::OutOfFuel;
    if (oxygen_remaining <= 0) state = ShipState::OutOfOxygen;
}

void Ship::handle_fall_below_ground() {
    // @0x2a95-0x2aa0, in full:  if (y < 0x2800) outcome = 3.
    //
    // That is the WHOLE of it. Nothing is zeroed, nothing is clamped. The port
    // used to also null y_velocity, z_velocity, x_movement_base, the slide and
    // the jump-o-master delta, and pin y to the road -- so the moment you went
    // over an edge the world froze and the ship dropped straight down on the
    // spot. In the original you keep every bit of the speed you had: z carries
    // you on past the end of the road while update_gravity, seeing y below the
    // road, pins the fall to terminal velocity (-106). That is the long,
    // committed plunge away from the camera, and it is what makes falling read
    // as falling rather than as the game stopping.
    if (state == ShipState::Alive && y_position_128 < GROUND_Y_128) {
        state = ShipState::Fallen;
    }
}

void Ship::run_jump_o_master(ControllerState controls, const Level& level) {
    if (will_land_on_tile(controls, *this, level)) {
        return;
    }

    const int32_t z_velocity_start = z_velocity_fp16;
    const int32_t x_movement_base_start = x_movement_base_128;
    bool success = false;
    for (int32_t i = 1; i <= 6; ++i) {
        // EXE (@0x1d9b-0x1dea): both x-movement variants are start +/-
        // (start * i) / 10 with idiv truncation. The double form floored the
        // sum instead, which differs for the negative (turning-left) base.
        x_movement_base_128 =
            x_movement_base_start + x_movement_base_start * i / 10;
        if (will_land_on_tile(controls, *this, level)) {
            success = true;
            break;
        }

        x_movement_base_128 =
            x_movement_base_start - x_movement_base_start * i / 10;
        if (will_land_on_tile(controls, *this, level)) {
            success = true;
            break;
        }

        x_movement_base_128 = x_movement_base_start;

        // @0x1e1c-0x1e42: same truncating scale for the z-velocity variants
        // (z_vel >= 0, so this matches the reference's floor32 exactly),
        // then the clamp; a clamped-away variant is skipped.
        int32_t zv2 = z_velocity_start + z_velocity_start * i / 10;
        z_velocity_fp16 = clamp_z_velocity(zv2);
        if (z_velocity_fp16 == zv2 && will_land_on_tile(controls, *this, level)) {
            success = true;
            break;
        }

        zv2 = z_velocity_start - z_velocity_start * i / 10;
        z_velocity_fp16 = clamp_z_velocity(zv2);
        if (z_velocity_fp16 == zv2 && will_land_on_tile(controls, *this, level)) {
            success = true;
            break;
        }

        z_velocity_fp16 = z_velocity_start;
    }

    jump_o_master_velocity_delta_fp16 = z_velocity_start - z_velocity_fp16;
    if (success) {
        jump_o_master_in_use = true;
    }
}

bool Ship::will_land_on_tile(ControllerState controls, Ship ship,
                             const Level& level) const {
    // The EXE's landing predictor (@0x1c20-0x1d49). x runs at 16.16 precision
    // here: the reference accumulated the exact product x_movement_base *
    // x_rate * 128 (a multiple of 1/65536) without requantizing, and
    // x_128 * 512 == x_fp16 keeps that bit-exact.
    int32_t x_fp16 = ship.x_position_128 * 512;
    int32_t y_128 = ship.y_position_128;
    int32_t z_fp16 = ship.z_position_fp16;
    const int32_t x_velocity_128 = ship.x_movement_base_128;
    int32_t y_velocity_l = ship.y_velocity_128;
    int32_t z_velocity_l = ship.z_velocity_fp16;
    const int32_t gravity_128 = level.gravity_acceleration_128();

    auto is_on_nothing = [&level](int32_t x_pos_fp16,
                                  int32_t z_pos_fp16) -> bool {
        const LevelCell cell = level.get_cell_fp16(x_pos_fp16, z_pos_fp16);
        return cell.is_empty() ||
               (cell.has_tile && cell.tile_effect == TouchEffect::Kill);
    };

    while (true) {
        const int32_t current_x_fp16 = x_fp16;
        const int32_t current_slide_128 = slide_amount_128;
        const int32_t current_z_fp16 = z_fp16;

        y_velocity_l += gravity_128;
        z_fp16 += z_velocity_l;

        const int32_t x_rate_fp16 = z_velocity_l + 0x618;
        x_fp16 += x_velocity_128 * x_rate_fp16 + current_slide_128 * 512;
        if (!in_range_inclusive(x_fp16, LEVEL_MIN_X_128 * 512,
                                LEVEL_MAX_X_128 * 512)) {
            return false;
        }

        y_128 += y_velocity_l;
        z_velocity_l = clamp_z_velocity(
            z_velocity_l + static_cast<int32_t>(controls.accel_input) * 0x4B);

        if (y_128 <= GROUND_Y_128) {
            return !is_on_nothing(current_x_fp16, current_z_fp16) &&
                   !is_on_nothing(x_fp16, z_fp16);
        }
    }
}

void Ship::clamp_global_z_velocity() {
    z_velocity_fp16 = clamp_z_velocity(z_velocity_fp16);
}

int32_t Ship::clamp_z_velocity(int32_t z_velocity_in_fp16) const {
    return std::clamp<int32_t>(z_velocity_in_fp16, 0, 0x2AAA);
}

GameplaySession::GameplaySession(Level level_in)
    : level(std::move(level_in)), ship(), expected_ship(ship) {}

bool GameplaySession::death_animation_finished() const {
    if (ship.state == ShipState::Alive) return false;
    // Dead but the death tick has not been stamped yet: the animation has not even
    // started, so it certainly has not finished.
    if (!death_frame_index.has_value()) return false;
    const std::size_t elapsed = frame_index_ - *death_frame_index;
    std::size_t needed = OTHER_DEATH_TICKS;
    if (ship.state == ShipState::Exploded) needed = EXPLOSION_DEATH_TICKS;
    else if (ship.state == ShipState::Fallen) needed = FALL_DEATH_TICKS;
    return elapsed >= needed;
}

GameplayFrameResult GameplaySession::run_frame(ControllerState controls) {
    last_controls = controls;
    const ShipState previous_state = ship.state;
    std::vector<GameplayEvent> events =
        ship.update(level, expected_ship, controls);
    // Stamp the tick the ship died on, which drives the death animation. Keyed on
    // "dead and not yet stamped" rather than on the Alive->dead edge so that a
    // session seeded into a dead state still animates.
    (void)previous_state;
    if (ship.state != ShipState::Alive && !death_frame_index.has_value()) {
        death_frame_index = frame_index_;
    }
    if (ship.z_position_fp16 >=
            static_cast<int32_t>(level.length()) * 65536 - 32768 &&
        level.is_inside_tunnel_128(ship.x_position_128, ship.y_position_128,
                                   ship.z_position_fp16)) {
        did_win = true;
    }

    GameplayFrameResult result;
    result.frame_index = frame_index_;
    result.controls = controls;
    // The one place per tick doubles appear: the render/HUD snapshot.
    result.snapshot = GameSnapshot{
        ship.x_position_128 / 128.0,
        ship.y_position_128 / 128.0,
        ship.z_position_fp16 / 65536.0,
        (ship.z_velocity_fp16 + ship.jump_o_master_velocity_delta_fp16) /
            65536.0,
        ship.state,
        ship.oxygen_remaining / static_cast<double>(0x7530),
        ship.fuel_remaining / static_cast<double>(0x7530),
        ship.jump_o_master_in_use,
        ship.jump_o_master_velocity_delta_fp16 / 65536.0};
    result.events = std::move(events);
    result.did_win = did_win;
    result.road_row_index = static_cast<std::size_t>(
        std::max<int32_t>(ship.z_position_fp16 >> 16, 0));
    frame_index_ += 1;
    return result;
}

GameplayFrameResult GameplaySession::run_demo_frame(const DemoRecording& demo) {
    return run_frame(controller_state_from_demo_input(
        sample_demo_input_for_ship(demo, ship)));
}

const DemoInput* sample_demo_input_for_ship(const DemoRecording& demo,
                                            const Ship& ship) {
    // One demo entry per 0x666 of raw z (the planner's
    // DEMO_TILE_POSITION_STEP_FP16); a plain truncating division, where the
    // double form multiplied by the inexact 65536.0 / 0x666.
    if (ship.z_position_fp16 < 0) {
        return nullptr;
    }
    const std::size_t index =
        static_cast<std::size_t>(ship.z_position_fp16 / 0x666);
    if (index >= demo.entries.size()) {
        return nullptr;
    }
    return &demo.entries[index];
}

ControllerState controller_state_from_demo_input(const DemoInput* input) {
    if (input == nullptr) {
        return ControllerState::neutral();
    }
    return ControllerState::make(input->left_right, input->accelerate_decelerate,
                                 input->jump);
}

} // namespace skyroads::core
