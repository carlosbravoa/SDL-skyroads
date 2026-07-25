#include "core/gameplay.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace skyroads::core {
namespace {

using skyroads::data::GROUND_Y;
using skyroads::data::LEVEL_CENTER_X;
using skyroads::data::LEVEL_MAX_X;
using skyroads::data::LEVEL_MIN_X;

// Fixed-point rounding helpers, identical to the reference free functions. the reference's
// f64::round() rounds half away from zero, which std::round() also does.
double floor16(double value) { return std::floor(value * 128.0) / 128.0; }
double floor32(double value) { return std::floor(value * 65536.0) / 65536.0; }
double round16_nearest(double value) { return std::round(value * 128.0) / 128.0; }
double round32_nearest(double value) {
    return std::round(value * 65536.0) / 65536.0;
}
// Truncation toward zero applied after a x128/x65536 scale.
double s_floor(double value) {
    return value >= 0.0 ? std::floor(value) : -std::floor(-value);
}

bool in_range_inclusive(double value, double lo, double hi) {
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
    : x_position(LEVEL_CENTER_X),
      y_position(GROUND_Y),
      z_position(3.0),
      slide_amount(0.0),
      sliding_accel(0),
      x_movement_base(0.0),
      y_velocity(0.0),
      z_velocity(0.0),
      fuel_remaining(0x7530),
      oxygen_remaining(0x7530),
      offset_at_which_not_inside_tile(0),
      is_on_ground(true),
      is_going_up(false),
      has_run_jump_o_master(false),
      jump_o_master_velocity_delta(0.0),
      jump_o_master_in_use(false),
      jumped_from_y_position(0.0),
      state(ShipState::Alive) {}

void Ship::sanitize_parameters() {
    x_position = round16_nearest(x_position);
    y_position = round16_nearest(y_position);
    z_position = round32_nearest(z_position);
}

bool Ship::is_different_height(const Ship& other) const {
    return std::fabs(other.y_position - y_position) > 0.01;
}

std::vector<GameplayEvent> Ship::update(const Level& level, Ship& expected,
                                        ControllerState controls) {
    std::vector<GameplayEvent> events;
    sanitize_parameters();
    const bool can_control = state == ShipState::Alive;

    const LevelCell cell = level.get_cell(x_position, y_position, z_position);
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
    update_gravity(level.gravity_acceleration());

    expected = *this;
    expected.attempt_motion(is_on_decel_pad);
    expected.sanitize_parameters();
    move_to(expected, level);
    sanitize_parameters();
    expected.sanitize_parameters();
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
    if (std::floor(y_position) == GROUND_Y && cell.has_tile) {
        return cell.tile_effect;
    }
    if (std::floor(y_position) > GROUND_Y && cell.cube_height.has_value() &&
        *cell.cube_height == static_cast<uint16_t>(std::floor(y_position))) {
        return cell.cube_effect;
    }
    return TouchEffect::None;
}

void Ship::apply_touch_effect(TouchEffect effect,
                              std::vector<GameplayEvent>& events) {
    switch (effect) {
        case TouchEffect::Accelerate:
            z_velocity += 0x12F / 65536.0;
            break;
        case TouchEffect::Decelerate:
            z_velocity -= 0x12F / 65536.0;
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
        if (slide_amount == 0.0 || offset_at_which_not_inside_tile >= 2) {
            const double yvel = std::fabs(y_velocity);
            if (yvel > (static_cast<double>(level.gravity) * 0x104 / 8.0 / 128.0)) {
                if (y_velocity < 0.0) {
                    events.push_back(GameplayEvent::ShipBounced);
                }
                // EXE (@0x2485): bounce = -(y_vel_raw * 5) / 10 on the raw 1/128
                // fixed-point value, with integer truncation -- not float -0.5*v.
                const long raw = std::lround(y_velocity * 128.0);
                y_velocity = static_cast<double>(-(raw * 5) / 10) / 128.0;
            } else {
                y_velocity = 0.0;
            }
        } else {
            y_velocity = 0.0;
        }
    }
}

void Ship::update_z_velocity(bool can_control, int8_t accel_input) {
    z_velocity += (can_control ? static_cast<double>(accel_input) : 0.0) * 0x4B /
                  65536.0;
    clamp_global_z_velocity();
}

void Ship::update_x_velocity(bool can_control, int8_t turn_input,
                             bool is_on_sliding_tile, bool is_above_nothing) {
    if (!is_on_sliding_tile) {
        const bool can_control_1 = (is_going_up || is_above_nothing) &&
                                   x_movement_base == 0.0 && y_velocity > 0.0 &&
                                   (y_position - jumped_from_y_position) < 30.0;
        const bool can_control_2 = !is_going_up && !is_above_nothing;
        if (can_control_1 || can_control_2) {
            x_movement_base =
                can_control ? static_cast<double>(turn_input) * 0x1D / 128.0 : 0.0;
        }
    }
}

void Ship::update_jump(bool can_control, bool is_above_nothing, bool jump_input,
                       const Level& level) {
    if (!is_going_up && !is_above_nothing && jump_input && level.gravity < 0x14 &&
        can_control) {
        y_velocity = 0x480 / 128.0;
        is_going_up = true;
        jumped_from_y_position = y_position;
    }
}

void Ship::update_jump_o_master(ControllerState controls, const Level& level) {
    if (is_going_up && !has_run_jump_o_master && y_position >= 110.0) {
        run_jump_o_master(controls, level);
        has_run_jump_o_master = true;
    }
}

void Ship::update_gravity(double gravity_acceleration) {
    // EXE (@0x25c9): threshold is y >= 0x2800 (raw) = GROUND_Y (80.0); the port
    // had 0x28 (40.0), a dropped-digit transcription. Terminal fall velocity is
    // -106 (0xff96), not -105.
    if (y_position >= GROUND_Y) {
        y_velocity += gravity_acceleration;
        y_velocity = s_floor(y_velocity * 128.0) / 128.0;
    } else if (y_velocity > -(106.0 / 128.0)) {
        y_velocity = -(106.0 / 128.0);
    }
}

void Ship::attempt_motion(bool on_decel_pad) {
    double motion_vel = z_velocity;
    if (!on_decel_pad) {
        motion_vel += 0x618 / 65536.0;
    }
    const double x_motion = s_floor(x_movement_base * 128.0) *
                                s_floor(motion_vel * 65536.0) / 65536.0 +
                            slide_amount;
    // Motion keeps being integrated after death: the EXE's motion integrator
    // (@0x1900..0x1a9b) has no death-code gate, which is what lets a ship that
    // fell off the road keep dropping out of view. Only the *inputs* are cut
    // (can_control), so x_movement_base decays to 0 on its own.
    x_position += x_motion;
    y_position += y_velocity;
    z_position += z_velocity;
}

void Ship::move_to(const Ship& dest, const Level& level) {
    if (x_position == dest.x_position && y_position == dest.y_position &&
        z_position == dest.z_position) {
        return;
    }

    Ship fake = *this;
    std::size_t iter = 1;
    for (int step = 1; step <= 5; ++step) {
        fake = *this;
        fake.interp(dest, static_cast<double>(step) / 5.0);
        if (level.is_inside_tile(fake.x_position, fake.y_position,
                                 fake.z_position)) {
            iter = static_cast<std::size_t>(step);
            break;
        }
        iter = static_cast<std::size_t>(step) + 1;
    }

    const double percent =
        static_cast<double>(iter == 0 ? 0 : iter - 1) / 5.0;
    interp(dest, percent);

    double z_gran = 0x1000 / 65536.0;
    while (z_gran != 0.0) {
        fake = *this;
        fake.z_position += z_gran;
        if (dest.z_position - z_position >= z_gran &&
            !level.is_inside_tile(fake.x_position, fake.y_position,
                                  fake.z_position)) {
            z_position = fake.z_position;
        } else {
            z_gran /= 16.0;
            z_gran = floor32(z_gran);
        }
    }
    z_position = floor32(z_position);

    double x_gran = dest.x_position > x_position ? 0x7D / 128.0 : -(0x7D / 128.0);
    while (std::fabs(x_gran) > 0.0) {
        fake = *this;
        fake.x_position += x_gran;
        if (std::fabs(dest.x_position - x_position) >= std::fabs(x_gran) &&
            !level.is_inside_tile(fake.x_position, fake.y_position,
                                  fake.z_position)) {
            x_position = fake.x_position;
        } else {
            x_gran = s_floor(x_gran / 5.0 * 128.0) / 128.0;
        }
    }
    x_position = floor16(x_position);

    double y_gran = dest.y_position > y_position ? 0x7D / 128.0 : -(0x7D / 128.0);
    while (std::fabs(y_gran) > 0.0) {
        fake = *this;
        fake.y_position += y_gran;
        if (std::fabs(dest.y_position - y_position) >= std::fabs(y_gran) &&
            !level.is_inside_tile(fake.x_position, fake.y_position,
                                  fake.z_position)) {
            y_position = fake.y_position;
        } else {
            y_gran = s_floor(y_gran / 5.0 * 128.0) / 128.0;
        }
    }
    y_position = floor16(y_position);
}

void Ship::handle_bumps(Ship& expected, const Level& level,
                        std::vector<GameplayEvent>& events) {
    Ship moved_ship = *this;
    moved_ship.z_position = expected.z_position;
    if (z_position != expected.z_position &&
        level.is_inside_tile(moved_ship.x_position, moved_ship.y_position,
                             moved_ship.z_position)) {
        const double bump_off = 0x3A0 / 128.0;
        moved_ship = *this;
        moved_ship.x_position = x_position - bump_off;
        moved_ship.z_position = expected.z_position;
        if (!level.is_inside_tile(moved_ship.x_position, moved_ship.y_position,
                                  moved_ship.z_position)) {
            x_position = moved_ship.x_position;
            expected.z_position = z_position;
            events.push_back(GameplayEvent::ShipBumpedWall);
        } else {
            moved_ship.x_position = x_position + bump_off;
            if (!level.is_inside_tile(moved_ship.x_position,
                                      moved_ship.y_position,
                                      moved_ship.z_position)) {
                x_position = moved_ship.x_position;
                expected.z_position = z_position;
                events.push_back(GameplayEvent::ShipBumpedWall);
            }
        }
    }
}

void Ship::handle_collision(const Ship& expected,
                            std::vector<GameplayEvent>& events) {
    if (std::fabs(z_position - expected.z_position) > 0.01) {
        if (z_velocity < (1.0 / 3.0) * (0x2AAA / 65536.0)) {
            z_velocity = 0.0;
            events.push_back(GameplayEvent::ShipBumpedWall);
        } else if (state != ShipState::Exploded) {
            state = ShipState::Exploded;
            events.push_back(GameplayEvent::ShipExploded);
        }
    }
}

void Ship::handle_slide_collision(Ship& expected) {
    if (std::fabs(x_position - expected.x_position) > 0.01) {
        x_movement_base = 0.0;
        if (slide_amount != 0.0) {
            expected.x_position = x_position;
            slide_amount = 0.0;
        }
        z_velocity -= 0x97 / 65536.0;
        clamp_global_z_velocity();
    }
}

void Ship::handle_bounce(const Ship& expected, const Level& level) {
    is_on_ground = false;
    if (y_velocity < 0.0 && expected.y_position != y_position) {
        z_velocity += jump_o_master_velocity_delta;
        jump_o_master_velocity_delta = 0.0;
        has_run_jump_o_master = false;
        jump_o_master_in_use = false;
        is_going_up = false;
        is_on_ground = true;
        sliding_accel = 0;

        Ship moved_ship = *this;
        for (int i = 1; i <= 0xE; ++i) {
            moved_ship = *this;
            moved_ship.x_position += static_cast<double>(i);
            moved_ship.y_position -= 1.0 / 128.0;
            if (!level.is_inside_tile(moved_ship.x_position,
                                      moved_ship.y_position,
                                      moved_ship.z_position)) {
                sliding_accel += 1;
                offset_at_which_not_inside_tile = static_cast<int16_t>(i);
                break;
            }
        }

        for (int i = 1; i <= 0xE; ++i) {
            moved_ship = *this;
            moved_ship.x_position -= static_cast<double>(i);
            moved_ship.y_position -= 1.0 / 128.0;
            if (!level.is_inside_tile(moved_ship.x_position,
                                      moved_ship.y_position,
                                      moved_ship.z_position)) {
                sliding_accel -= 1;
                offset_at_which_not_inside_tile = static_cast<int16_t>(i);
                break;
            }
        }

        if (sliding_accel != 0) {
            slide_amount +=
                0x11 * static_cast<double>(sliding_accel) / 128.0;
        } else {
            slide_amount = 0.0;
        }
    }
}

void Ship::handle_oxygen_and_fuel(const Level& level) {
    oxygen_remaining -=
        0x7530 / (0x24 * static_cast<double>(level.oxygen));
    if (oxygen_remaining <= 0.0) {
        oxygen_remaining = 0.0;
        state = ShipState::OutOfOxygen;
    }

    fuel_remaining -= z_velocity * 0x7530 / static_cast<double>(level.fuel);
    if (fuel_remaining <= 0.0) {
        fuel_remaining = 0.0;
        state = ShipState::OutOfFuel;
    }
}

void Ship::handle_fall_below_ground() {
    if (state == ShipState::Alive && y_position < GROUND_Y) {
        state = ShipState::Fallen;
        y_position = std::min(y_position, GROUND_Y);
        y_velocity = 0.0;
        z_velocity = 0.0;
        x_movement_base = 0.0;
        slide_amount = 0.0;
        jump_o_master_velocity_delta = 0.0;
        jump_o_master_in_use = false;
        has_run_jump_o_master = false;
        is_on_ground = false;
        is_going_up = false;
    }
}

void Ship::interp(const Ship& dest, double percent) {
    x_position =
        floor16((dest.x_position - x_position) * percent + x_position);
    y_position =
        floor16((dest.y_position - y_position) * percent + y_position);
    z_position =
        floor32((dest.z_position - z_position) * percent + z_position);
}

void Ship::run_jump_o_master(ControllerState controls, const Level& level) {
    if (will_land_on_tile(controls, *this, level)) {
        return;
    }

    const double z_velocity_start = z_velocity;
    const double x_movement_base_start = x_movement_base;
    bool success = false;
    for (int i = 1; i <= 6; ++i) {
        x_movement_base = floor16(x_movement_base_start +
                                  x_movement_base_start * i / 10.0);
        if (will_land_on_tile(controls, *this, level)) {
            success = true;
            break;
        }

        x_movement_base = floor16(x_movement_base_start -
                                  x_movement_base_start * i / 10.0);
        if (will_land_on_tile(controls, *this, level)) {
            success = true;
            break;
        }

        x_movement_base = x_movement_base_start;

        double zv2 = floor32(z_velocity_start + z_velocity_start * i / 10.0);
        z_velocity = clamp_z_velocity(zv2);
        if (z_velocity == zv2 && will_land_on_tile(controls, *this, level)) {
            success = true;
            break;
        }

        zv2 = floor32(z_velocity_start - z_velocity_start * i / 10.0);
        z_velocity = clamp_z_velocity(zv2);
        if (z_velocity == zv2 && will_land_on_tile(controls, *this, level)) {
            success = true;
            break;
        }

        z_velocity = z_velocity_start;
    }

    jump_o_master_velocity_delta = z_velocity_start - z_velocity;
    if (success) {
        jump_o_master_in_use = true;
    }
}

bool Ship::will_land_on_tile(ControllerState controls, Ship ship,
                             const Level& level) const {
    double x_pos = ship.x_position;
    double y_pos = ship.y_position;
    double z_pos = ship.z_position;
    const double x_velocity = ship.x_movement_base;
    double y_velocity_l = ship.y_velocity;
    double z_velocity_l = ship.z_velocity;

    auto is_on_nothing = [&level](double x_position, double z_position) -> bool {
        const LevelCell cell = level.get_cell(x_position, 0.0, z_position);
        return cell.is_empty() ||
               (cell.has_tile && cell.tile_effect == TouchEffect::Kill);
    };

    while (true) {
        const double current_x = x_pos;
        const double current_slide_amount = slide_amount;
        const double current_z = z_pos;

        y_velocity_l += level.gravity_acceleration();
        z_pos += z_velocity_l;

        const double x_rate = z_velocity_l + 0x618 / 65536.0;
        const double x_mov = x_velocity * x_rate * 128.0 + current_slide_amount;
        x_pos += x_mov;
        if (!in_range_inclusive(x_pos, LEVEL_MIN_X, LEVEL_MAX_X)) {
            return false;
        }

        y_pos += y_velocity_l;
        z_velocity_l = clamp_z_velocity(
            z_velocity_l + static_cast<double>(controls.accel_input) * 0x4B /
                               65536.0);

        if (y_pos <= GROUND_Y) {
            return !is_on_nothing(current_x, current_z) &&
                   !is_on_nothing(x_pos, z_pos);
        }
    }
}

void Ship::clamp_global_z_velocity() {
    z_velocity = clamp_z_velocity(z_velocity);
}

double Ship::clamp_z_velocity(double z_velocity_in) const {
    return std::clamp(z_velocity_in, 0.0, 0x2AAA / 65536.0);
}

GameplaySession::GameplaySession(Level level_in)
    : level(std::move(level_in)), ship(), expected_ship(ship) {}

bool GameplaySession::death_animation_finished() const {
    if (ship.state == ShipState::Alive) return false;
    if (!death_frame_index.has_value()) return true;
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
    if (ship.z_position >= static_cast<double>(level.length()) - 0.5 &&
        level.is_inside_tunnel(ship.x_position, ship.y_position,
                               ship.z_position)) {
        did_win = true;
    }

    GameplayFrameResult result;
    result.frame_index = frame_index_;
    result.controls = controls;
    result.snapshot = GameSnapshot{
        ship.x_position,
        ship.y_position,
        ship.z_position,
        ship.z_velocity + ship.jump_o_master_velocity_delta,
        ship.state,
        ship.oxygen_remaining / 0x7530,
        ship.fuel_remaining / 0x7530,
        ship.jump_o_master_in_use,
        ship.jump_o_master_velocity_delta};
    result.events = std::move(events);
    result.did_win = did_win;
    result.road_row_index =
        static_cast<std::size_t>(std::max(std::floor(ship.z_position), 0.0));
    frame_index_ += 1;
    return result;
}

GameplayFrameResult GameplaySession::run_demo_frame(const DemoRecording& demo) {
    return run_frame(controller_state_from_demo_input(
        sample_demo_input_for_ship(demo, ship)));
}

const DemoInput* sample_demo_input_for_ship(const DemoRecording& demo,
                                            const Ship& ship) {
    const double index_f = std::floor(ship.z_position * (65536.0 / 0x0666));
    if (index_f < 0.0) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(index_f);
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
