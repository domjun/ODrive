
#include "odrive_main.h"
#include <algorithm>

Controller::Controller(Config_t& config) :
    config_(config)
{}

void Controller::reset() {
    pos_setpoint_ = 0.0f;
    vel_setpoint_ = 0.0f;
    vel_integrator_current_ = 0.0f;
    current_setpoint_ = 0.0f;
}

void Controller::set_error(Error_t error) {
    error_ |= error;
    axis_->error_ |= Axis::ERROR_CONTROLLER_FAILED;
}

//--------------------------------
// Command Handling
//--------------------------------

void Controller::set_pos_setpoint(float pos_setpoint, float vel_feed_forward, float current_feed_forward) {
    pos_setpoint_ = pos_setpoint;
    vel_setpoint_ = vel_feed_forward;
    current_setpoint_ = current_feed_forward;
    config_.control_mode = CTRL_MODE_POSITION_CONTROL;
#ifdef DEBUG_PRINT
    printf("POSITION_CONTROL %6.0f %3.3f %3.3f\n", pos_setpoint, vel_setpoint_, current_setpoint_);
#endif
}

void Controller::set_vel_setpoint(float vel_setpoint, float current_feed_forward) {
    vel_setpoint_ = vel_setpoint;
    current_setpoint_ = current_feed_forward;
    config_.control_mode = CTRL_MODE_VELOCITY_CONTROL;
#ifdef DEBUG_PRINT
    printf("VELOCITY_CONTROL %3.3f %3.3f\n", vel_setpoint_, motor->current_setpoint_);
#endif
}

void Controller::set_current_setpoint(float current_setpoint) {
    current_setpoint_ = current_setpoint;
    config_.control_mode = CTRL_MODE_CURRENT_CONTROL;
#ifdef DEBUG_PRINT
    printf("CURRENT_CONTROL %3.3f\n", current_setpoint_);
#endif
}

void Controller::move_to_pos(float goal_point) {
    axis_->trap_.planTrapezoidal(goal_point, pos_setpoint_, vel_setpoint_,
                                 axis_->trap_.config_.vel_limit,
                                 axis_->trap_.config_.accel_limit,
                                 axis_->trap_.config_.decel_limit);
    traj_start_loop_count_ = axis_->loop_counter_;
    config_.control_mode = CTRL_MODE_TRAJECTORY_CONTROL;
    goal_point_ = goal_point;
}

void Controller::move_incremental(float displacement, bool from_goal_point = true){
    if(from_goal_point){
        move_to_pos(goal_point_ + displacement);
    } else{
        move_to_pos(pos_setpoint_ + displacement);
    }
}

bool Controller::update(float pos_estimate, float vel_estimate, float* current_setpoint_output) {

    // Trajectory control
    if (config_.control_mode == CTRL_MODE_TRAJECTORY_CONTROL) {
        // Note: uint32_t loop count delta is OK across overflow
        // Beware of negative deltas, as they will not be well behaved due to uint!
        float t = (axis_->loop_counter_ - traj_start_loop_count_) * current_meas_period;
        if (t > axis_->trap_.Tf_) {
            // Drop into position control mode when done to avoid problems on loop counter delta overflow
            config_.control_mode = CTRL_MODE_POSITION_CONTROL;
            // pos_setpoint already set by trajectory
            vel_setpoint_ = 0.0f;
            current_setpoint_ = 0.0f;
        } else {
            TrapezoidalTrajectory::Step_t traj_step = axis_->trap_.eval(t);
            pos_setpoint_ = traj_step.Y;
            vel_setpoint_ = traj_step.Yd;
            current_setpoint_ = traj_step.Ydd * axis_->trap_.config_.A_per_css;
        }
    }

    // Ramp rate limited velocity setpoint
    if (config_.control_mode == CTRL_MODE_VELOCITY_CONTROL && config_.vel_ramp_enable) {
        float max_step_size = current_meas_period * config_.vel_ramp_rate;
        float full_step = vel_ramp_target_ - vel_setpoint_;
        float step = clamp_bidirf(full_step, max_step_size);
        vel_setpoint_ += step;
    }

    // Position control
    // TODO Decide if we want to use encoder or pll position here
    float vel_des = vel_setpoint_;
    if (config_.control_mode >= CTRL_MODE_POSITION_CONTROL) {
        float pos_err;
        if (config_.setpoints_in_cpr) {
            // TODO this breaks the semantics that estimates come in on the arguments.
            // It's probably better to call a get_estimate that will arbitrate (enc vs sensorless) instead.
            float cpr = (float)(axis_->encoder_.config_.cpr);
            // Keep pos setpoint from drifting
            pos_setpoint_ = fmodf_pos(pos_setpoint_, cpr);
            // Circular delta
            pos_err = pos_setpoint_ - axis_->encoder_.pos_cpr_;
            pos_err = wrap_pm(pos_err, 0.5f * cpr);
        } else {
            pos_err = pos_setpoint_ - pos_estimate;
        }
        vel_des += config_.pos_gain * pos_err;
    }

    // Velocity limiting
    float vel_lim = config_.vel_limit;
    if (vel_des > vel_lim) vel_des = vel_lim;
    if (vel_des < -vel_lim) vel_des = -vel_lim;

    // Check for overspeed fault (done in this module (controller) for cohesion with vel_lim)
    if (config_.vel_limit_tolerance > 0.0f) { // 0.0f to disable
        if (fabsf(vel_estimate) > config_.vel_limit_tolerance * vel_lim) {
            set_error(ERROR_OVERSPEED);
            return false;
        }
    }

    // TODO: Change to controller working in torque units
    // Torque per amp gain scheduling (ACIM)
    float vel_gain = config_.vel_gain;
    float vel_integrator_gain = config_.vel_integrator_gain;
    if (axis_->motor_.config_.motor_type == Motor::MOTOR_TYPE_ACIM) {
        float effective_flux = axis_->motor_.current_control_.acim_rotor_flux;
        float minflux = axis_->motor_.config_.acim_gain_min_flux;
        if (fabsf(effective_flux) < minflux)
            effective_flux = std::copysignf(minflux, effective_flux);
        vel_gain /= effective_flux;
        vel_integrator_gain /= effective_flux;
        // TODO: also scale the integral value which is also changing units.
        // (or again just do control in torque units)
    }

    // Current feed forward path
    float Iq = current_setpoint_;

    // Anticogging
    // Hard counts version
    // 0 to 1 maps to full rotation
    float pos_ratio = (float)axis_->encoder_.count_in_cpr_ / (float)axis_->encoder_.config_.cpr;
    // Interpolated/filtered version
    // 0 to 1 maps to full rotation
    // float pos_ratio = axis_->encoder_.pos_cpr_ / (float)axis_->encoder_.config_.cpr
    float idxf = pos_ratio * config_.cogmap_size;
    size_t idx = (size_t)idxf;
    size_t idx1 = (idx + 1) % config_.cogmap_size;
    float frac = idxf - (float)idx;
    // linear interpolation
    cogmap_current_ = (1.0f - frac) * config_.cogmap[idx] + frac * config_.cogmap[idx1];
    Iq += cogmap_current_;


    // Velocity control
    float v_err = vel_des - vel_estimate;
    if (config_.control_mode >= CTRL_MODE_VELOCITY_CONTROL) {
        // Proportional feedback
        Iq += vel_gain * v_err;

        // Anticogging integral and linear broadcast
        float cogmap_corr_rate = config_.cogmap_integrator_gain * v_err;
        float cogmap_correction =  cogmap_corr_rate * current_meas_period;
        config_.cogmap[idx] += (1.0f - frac) * cogmap_correction;
        config_.cogmap[idx1] += frac * cogmap_correction;
        config_.cogmap[idx] = clamp_bidirf(config_.cogmap[idx], config_.cogmap_max_current);
        config_.cogmap[idx1] = clamp_bidirf(config_.cogmap[idx1], config_.cogmap_max_current);
        // RMS correction for reporting
        cogmap_correction_pwr_ += 0.001f * (cogmap_corr_rate*cogmap_corr_rate - cogmap_correction_pwr_);
    }

    // Velocity integral action before limiting
    Iq += vel_integrator_current_;

    // Current limiting
    // TODO: Change to controller working in torque units
    // and get the torque limits from a function of the motor
    bool limited = false;
    float Ilim = axis_->motor_.effective_current_lim();
    if (Iq > Ilim) {
        limited = true;
        Iq = Ilim;
    }
    if (Iq < -Ilim) {
        limited = true;
        Iq = -Ilim;
    }

    // Velocity integrator (behaviour dependent on limiting)
    if (config_.control_mode < CTRL_MODE_VELOCITY_CONTROL) {
        // reset integral if not in use
        vel_integrator_current_ = 0.0f;
    } else {
        if (limited) {
            // TODO make decayfactor configurable
            vel_integrator_current_ *= 0.99f;
        } else {
            vel_integrator_current_ += (vel_integrator_gain * current_meas_period) * v_err;
        }
    }

    if (current_setpoint_output) *current_setpoint_output = Iq;
    return true;
}
