// Part of the SkyRoads SDL port
//
// The deterministic ship simulation. Every constant is kept as the exact
// `hex / hex` fixed-point ratio from the reference design, and the floating-point
// rounding helpers (floor16/floor32/s_floor/round*) reproduce the DOS math bit
// for bit. This is the layer whose output must match the reference reference exactly.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "data/demo.hpp"
#include "data/level.hpp"

namespace skyroads::core {

using skyroads::data::DemoInput;
using skyroads::data::DemoRecording;
using skyroads::data::Level;
using skyroads::data::LevelCell;
using skyroads::data::TouchEffect;

struct ControllerState {
    int8_t turn_input = 0;
    int8_t accel_input = 0;
    bool jump_input = false;

    static ControllerState neutral() { return ControllerState{}; }
    // Mirrors the reference's `ControllerState::new`, which asserts each axis is in -1..=1.
    static ControllerState make(int8_t turn_input, int8_t accel_input,
                                bool jump_input);

    bool operator==(const ControllerState& o) const {
        return turn_input == o.turn_input && accel_input == o.accel_input &&
               jump_input == o.jump_input;
    }
    bool operator!=(const ControllerState& o) const { return !(*this == o); }
};

enum class ShipState {
    Alive,
    Exploded,
    Fallen,
    OutOfFuel,
    OutOfOxygen,
};

enum class GameplayEvent {
    ShipBumpedWall,
    ShipExploded,
    ShipBounced,
    ShipRefilled,
};

struct Ship {
    double x_position;
    double y_position;
    double z_position;
    double slide_amount;
    int16_t sliding_accel;
    double x_movement_base;
    double y_velocity;
    double z_velocity;
    double fuel_remaining;
    double oxygen_remaining;
    int16_t offset_at_which_not_inside_tile;
    bool is_on_ground;
    bool is_going_up;
    bool has_run_jump_o_master;
    double jump_o_master_velocity_delta;
    bool jump_o_master_in_use;
    double jumped_from_y_position;
    ShipState state;

    Ship();

    // Advances one fixed step. `expected` is the DOS "expected" scratch ship,
    // carried across frames exactly as the reference does. Returns the events
    // raised this step, in order.
    std::vector<GameplayEvent> update(const Level& level, Ship& expected,
                                      ControllerState controls);

private:
    void sanitize_parameters();
    bool is_different_height(const Ship& other) const;
    TouchEffect get_touch_effect(const LevelCell& cell) const;
    void apply_touch_effect(TouchEffect effect,
                            std::vector<GameplayEvent>& events);
    void update_y_velocity(const Ship& expected, const Level& level,
                           std::vector<GameplayEvent>& events);
    void update_z_velocity(bool can_control, int8_t accel_input);
    void update_x_velocity(bool can_control, int8_t turn_input,
                           bool is_on_sliding_tile, bool is_above_nothing);
    void update_jump(bool can_control, bool is_above_nothing, bool jump_input,
                     const Level& level);
    void update_jump_o_master(ControllerState controls, const Level& level);
    void update_gravity(double gravity_acceleration);
    void attempt_motion(bool on_decel_pad);
    void move_to(const Ship& dest, const Level& level);
    void handle_bumps(Ship& expected, const Level& level,
                      std::vector<GameplayEvent>& events);
    void handle_collision(const Ship& expected,
                          std::vector<GameplayEvent>& events);
    void handle_slide_collision(Ship& expected);
    void handle_bounce(const Ship& expected, const Level& level);
    void handle_oxygen_and_fuel(const Level& level);
    void handle_fall_below_ground();
    void interp(const Ship& dest, double percent);
    void run_jump_o_master(ControllerState controls, const Level& level);
    bool will_land_on_tile(ControllerState controls, Ship ship,
                           const Level& level) const;
    void clamp_global_z_velocity();
    double clamp_z_velocity(double z_velocity) const;
};

struct GameSnapshot {
    double x_position;
    double y_position;
    double z_position;
    double z_velocity;
    ShipState craft_state;
    double oxygen_percent;
    double fuel_percent;
    bool jump_o_master_in_use;
    double jump_o_master_velocity_delta;
};

struct GameplayFrameResult {
    std::size_t frame_index;
    ControllerState controls;
    GameSnapshot snapshot;
    std::vector<GameplayEvent> events;
    bool did_win;
    std::size_t road_row_index;
};

// How long the game keeps simulating after the ship dies, so the death plays out
// on screen (EXE @0x2200, re/NOTES.md Update 9). A crash runs the explosion
// counter ds:0x4578 for 42 ticks (14 sprites x 3 ticks each) and then ends
// immediately; every other death instead dwells on ds:0x4566 until 108 ticks,
// which is what lets a ship that fell off the road drop out of view.
constexpr std::size_t EXPLOSION_DEATH_TICKS = 42;
constexpr std::size_t OTHER_DEATH_TICKS = 108;
// Falling off the road: the EXE dwells the full OTHER_DEATH_TICKS (108 ticks = 3.0s
// at the original's 36Hz tick -- PIT divisor 6628 -> 180Hz, ISR ticks 2 per 10
// interrupts). Play-testing against the original says the fall was clearly shorter
// than that, and by ~this point the ship has dropped out of sight anyway, so this is
// a deliberate, tunable divergence. Raise to OTHER_DEATH_TICKS for the literal EXE
// behaviour.
constexpr std::size_t FALL_DEATH_TICKS = 45;

struct GameplaySession {
    Level level;
    Ship ship;
    Ship expected_ship;
    bool did_win = false;
    // `pub(crate)` in the reference design; public here so the host/tests can seed state.
    ControllerState last_controls = ControllerState::neutral();
    std::optional<std::size_t> death_frame_index;
    std::size_t frame_index_ = 0;

    explicit GameplaySession(Level level);

    std::size_t frame_index() const { return frame_index_; }
    // True once the ship is dead AND its death animation has run its course, i.e.
    // when the original would stop simulating and report the outcome.
    bool death_animation_finished() const;
    GameplayFrameResult run_frame(ControllerState controls);
    GameplayFrameResult run_demo_frame(const DemoRecording& demo);
};

// the reference design returns Option<&DemoInput>; a nullable pointer models it here.
const DemoInput* sample_demo_input_for_ship(const DemoRecording& demo,
                                            const Ship& ship);
ControllerState controller_state_from_demo_input(const DemoInput* input);

} // namespace skyroads::core
