/*
	Copyright 2019 Mitch Lustig

	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */

#include "conf_general.h"

#include "ch.h" // ChibiOS
#include "hal.h" // ChibiOS HAL
#include "mc_interface.h" // Motor control functions
#include "hw.h" // Pin mapping on this hardware
#include "timeout.h" // To reset the timeout
#include "commands.h"
#include "imu/imu.h"
#include "imu/ahrs.h"
#include "utils.h"
#include "datatypes.h"
#include "comm_can.h"


#include <math.h>

// Can
#define MAX_CAN_AGE 0.1

// Data type
typedef enum {
	STARTUP = 0,
	RUNNING,
	FAULT,
	DEAD
} BalanceState;

typedef enum {
	CENTERING = 0,
	TILTBACK
} SetpointAdjustmentType;

typedef enum {
	OFF = 0,
	HALF,
	ON
} SwitchState;

// Balance thread
static THD_FUNCTION(balance_thread, arg);
static THD_WORKING_AREA(balance_thread_wa, 2048); // 2kb stack for this thread

static volatile balance_config balance_conf;
static volatile imu_config imu_conf;
static thread_t *app_thread;

// Values used in loop
static BalanceState state;
static float pitch_angle, roll_angle;
static float gyro[3];
static float duty_cycle, abs_duty_cycle;
static float erpm, abs_erpm, avg_erpm;
static float proportional, integral, derivative;
static float last_proportional;
static float pid_value;
static float yaw_proportional, yaw_integral, yaw_derivative, yaw_last_proportional, yaw_pid_value, yaw_setpoint;
static float setpoint, setpoint_target, setpoint_target_interpolated;
static SetpointAdjustmentType setpointAdjustmentType;
static float startup_step_size, tiltback_step_size;
static systime_t current_time, last_time, diff_time;
static systime_t startup_start_time, startup_diff_time;
static systime_t dead_start_time;
static systime_t fault_start_time;
static float adc1, adc2;
static SwitchState switch_state;

// Values read to pass in app data to GUI
static float motor_current;
static float motor_position;

void app_balance_configure(balance_config *conf, imu_config *conf2) {
	balance_conf = *conf;
	imu_conf = *conf2;
}

void app_balance_start(void) {

	// Reset all Values
	state = STARTUP;
	pitch_angle = 0;
	roll_angle = 0;
	gyro[0] = 0;
	gyro[1] = 0;
	gyro[2] = 0;
	duty_cycle = 0;
	abs_duty_cycle = 0;
	erpm = 0;
	abs_erpm = 0;
	avg_erpm = 0;
	adc1 = 0;
	adc2 = 0;
	switch_state = OFF;
	proportional = 0;
	integral = 0;
	derivative = 0;
	last_proportional = 0;
	pid_value = 0;
	yaw_proportional = 0;
	yaw_integral = 0;
	yaw_derivative = 0;
	yaw_last_proportional = 0;
	yaw_pid_value = 0;
	yaw_setpoint = 0;
	setpoint = 0;
	setpoint_target = 0;
	setpoint_target_interpolated = 0;
	setpointAdjustmentType = CENTERING;
	startup_step_size = 0;
	tiltback_step_size = 0;
	current_time = 0;
	last_time = 0;
	diff_time = 0;
	startup_start_time = 0;
	startup_diff_time = 0;

	// Start the balance thread
	app_thread = chThdCreateStatic(balance_thread_wa, sizeof(balance_thread_wa), NORMALPRIO, balance_thread, NULL);
}

float app_balance_get_pid_output(void) {
	return pid_value;
}
float app_balance_get_pitch_angle(void) {
	return pitch_angle;
}
float app_balance_get_roll_angle(void) {
	return roll_angle;
}
uint32_t app_balance_get_diff_time(void) {
	return ST2US(diff_time);
}
float app_balance_get_motor_current(void) {
	return motor_current;
}
float app_balance_get_motor_position(void) {
	return motor_position;
}
uint16_t app_balance_get_state(void) {
	return state;
}
uint16_t app_balance_get_switch_state(void) {
	return switch_state;
}
float app_balance_get_adc1(void) {
	return adc1;
}
float app_balance_get_adc2(void) {
	return adc2;
}

float get_setpoint_adjustment_step_size(void){
	switch(setpointAdjustmentType){
		case (CENTERING):
			return startup_step_size;
		case (TILTBACK):
			return tiltback_step_size;
	}
	return 0;
}

float apply_deadzone(float error){
	if(balance_conf.deadzone == 0){
		return error;
	}

	if(error < balance_conf.deadzone && error > -balance_conf.deadzone){
		return 0;
	} else if(error > balance_conf.deadzone){
		return error - balance_conf.deadzone;
	} else {
		return error + balance_conf.deadzone;
	}
}

void brake(void){
	// Reset the timeout
	timeout_reset();
	// Set current
	mc_interface_set_brake_current(balance_conf.brake_current);
	if(balance_conf.multi_esc){
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg *msg = comm_can_get_status_msg_index(i);
			if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
				comm_can_set_current_brake(msg->id, balance_conf.brake_current);
			}
		}
	}
}

void set_current(float current, float yaw_current){
	// Reset the timeout
	timeout_reset();
	// Set current
	if(balance_conf.multi_esc){
		mc_interface_set_current(current + yaw_current);
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg *msg = comm_can_get_status_msg_index(i);

			if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
				comm_can_set_current(msg->id, current - yaw_current);// Assume 2 motors, i don't know how to steer 3 anyways
			}
		}
	} else {
		mc_interface_set_current(current);
	}
}

void app_balance_stop(void) {
	if(app_thread != NULL){
		chThdTerminate(app_thread);
		chThdWait(app_thread);
	}
	set_current(0, 0);
}

static THD_FUNCTION(balance_thread, arg) {
	(void)arg;
	chRegSetThreadName("APP_BALANCE");

	// Do one off config
	startup_step_size = balance_conf.startup_speed / balance_conf.hertz;
	tiltback_step_size = balance_conf.tiltback_speed / balance_conf.hertz;

	state = STARTUP;

	while (!chThdShouldTerminateX()) {
		// Update times
		current_time = chVTGetSystemTimeX();
		if(last_time == 0){
		  last_time = current_time;
		}
		diff_time = current_time - last_time;
		last_time = current_time;

		// Read values for GUI
		motor_current = mc_interface_get_tot_current_directional_filtered();
		motor_position = mc_interface_get_pid_pos_now();

		// Get the values we want
		pitch_angle = imu_get_pitch() * 180.0f / M_PI;
		roll_angle = imu_get_roll() * 180.0f / M_PI;
		imu_get_gyro(gyro);
		duty_cycle = mc_interface_get_duty_cycle_now();
		abs_duty_cycle = fabsf(duty_cycle);
		erpm = mc_interface_get_rpm();
		abs_erpm = fabsf(erpm);
		if(balance_conf.multi_esc){
			avg_erpm = erpm;
			for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
				can_status_msg *msg = comm_can_get_status_msg_index(i);
				if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
					avg_erpm += msg->rpm;
				}
			}
			avg_erpm = avg_erpm/2;// Assume 2 motors, i don't know how to steer 3 anyways
		}
		adc1 = (((float)ADC_Value[ADC_IND_EXT])/4095) * V_REG;
#ifdef ADC_IND_EXT2
		adc2 = (((float)ADC_Value[ADC_IND_EXT2])/4095) * V_REG;
#else
		adc2 = 0.0;
#endif

		// Calculate switch state from ADC values
		if(balance_conf.adc1 == 0 && balance_conf.adc2 == 0){ // No Switch
			switch_state = ON;
		}else if(balance_conf.adc2 == 0){ // Single switch on ADC1
			if(adc1 > balance_conf.adc1){
				switch_state = ON;
			} else {
				switch_state = OFF;
			}
		}else if(balance_conf.adc1 == 0){ // Single switch on ADC2
			if(adc2 > balance_conf.adc2){
				switch_state = ON;
			} else {
				switch_state = OFF;
			}
		}else{ // Double switch
			if(adc1 > balance_conf.adc1 && adc2 > balance_conf.adc2){
				switch_state = ON;
			}else if(adc1 > balance_conf.adc1 || adc2 > balance_conf.adc2){
				switch_state = HALF;
			}else{
				switch_state = OFF;
			}
		}


		// State based logic
		switch(state){
			case (STARTUP):
				while(!imu_startup_done()){
					// Disable output
					brake();
					// Wait
					chThdSleepMilliseconds(50);
				}
				state = FAULT;
				startup_start_time = 0;
				startup_diff_time = 0;
				break;
			case (RUNNING):
				// Check for overspeed
				if(abs_duty_cycle > balance_conf.overspeed_duty){
					if(ST2MS(current_time - dead_start_time) > balance_conf.overspeed_delay){
						state = DEAD;
					}
				} else {
					dead_start_time = current_time;
				}

				// Check for fault
				if(
					fabsf(pitch_angle) > balance_conf.pitch_fault || // Balnce axis tip over
					fabsf(roll_angle) > balance_conf.roll_fault || // Cross axis tip over
					switch_state == OFF || // Switch fully open
					(switch_state == HALF && abs_erpm < balance_conf.adc_half_fault_erpm) // Switch partially open and stopped
						){
					if(ST2MS(current_time - fault_start_time) > balance_conf.fault_delay){
						state = FAULT;
					}
				} else {
					fault_start_time = current_time;
				}

				// Over speed tilt back safety
				if(setpointAdjustmentType == CENTERING && setpoint_target_interpolated != setpoint_target){
					// Ignore tiltback during centering sequence
				}else if(abs_duty_cycle > balance_conf.tiltback_duty ||
						(abs_duty_cycle > 0.05 && GET_INPUT_VOLTAGE() > balance_conf.tiltback_high_voltage) ||
						(abs_duty_cycle > 0.05 && GET_INPUT_VOLTAGE() < balance_conf.tiltback_low_voltage)){
					if(duty_cycle > 0){
						setpoint_target = balance_conf.tiltback_angle;
					} else {
						setpoint_target = -balance_conf.tiltback_angle;
					}
					setpointAdjustmentType = TILTBACK;
				}else if(abs_duty_cycle > 0.03){
					// Nose angle adjustment
					if(duty_cycle > 0){
						setpoint_target = balance_conf.tiltback_constant;
					} else {
						setpoint_target = -balance_conf.tiltback_constant;
					}
					setpointAdjustmentType = TILTBACK;
				}else{
					setpointAdjustmentType = TILTBACK;
					setpoint_target = 0;
				}

				// Adjust setpoint
				if(setpoint_target_interpolated != setpoint_target){
					// If we are less than one step size away, go all the way
					if(fabsf(setpoint_target - setpoint_target_interpolated) < get_setpoint_adjustment_step_size()){
						setpoint_target_interpolated = setpoint_target;
					}else if (setpoint_target - setpoint_target_interpolated > 0){
						setpoint_target_interpolated += get_setpoint_adjustment_step_size();
					}else{
						setpoint_target_interpolated -= get_setpoint_adjustment_step_size();
					}
				}

				// Apply setpoint filtering
				if(setpointAdjustmentType == CENTERING){
					// Ignore filtering during centering
					setpoint = setpoint_target_interpolated;
				}else{
					setpoint = (setpoint * (1-balance_conf.setpoint_pitch_filter)) + (pitch_angle * balance_conf.setpoint_pitch_filter);
					setpoint = (setpoint * (1-balance_conf.setpoint_target_filter)) + (setpoint_target_interpolated * balance_conf.setpoint_target_filter);
					if(setpoint > balance_conf.setpoint_clamp){
						setpoint = balance_conf.setpoint_clamp;
					}else if (setpoint < -balance_conf.setpoint_clamp){
						setpoint = -balance_conf.setpoint_clamp;
					}
				}

				// Do PID maths
				proportional = setpoint - pitch_angle;
				// Apply deadzone
				proportional = apply_deadzone(proportional);
				// Resume real PID maths
				integral = integral + proportional;
				derivative = proportional - last_proportional;

				pid_value = (balance_conf.kp * proportional) + (balance_conf.ki * integral) + (balance_conf.kd * derivative);

				last_proportional = proportional;

				// Apply current boost
				if(pid_value > 0){
					pid_value += balance_conf.current_boost;
				}else if(pid_value < 0){
					pid_value -= balance_conf.current_boost;
				}

				if(balance_conf.multi_esc){
					// Calculate setpoint
					if(abs_duty_cycle < .02){
						yaw_setpoint = 0;
					} else if(avg_erpm < 0){
						yaw_setpoint = (-balance_conf.roll_steer_kp * roll_angle) + (balance_conf.roll_steer_erpm_kp * roll_angle * avg_erpm);
					} else{
						yaw_setpoint = (balance_conf.roll_steer_kp * roll_angle) + (balance_conf.roll_steer_erpm_kp * roll_angle * avg_erpm);
					}
					// Do PID maths
					yaw_proportional = yaw_setpoint - gyro[2];
					yaw_integral = yaw_integral + yaw_proportional;
					yaw_derivative = yaw_proportional - yaw_last_proportional;

					yaw_pid_value = (balance_conf.yaw_kp * yaw_proportional) + (balance_conf.yaw_ki * yaw_integral) + (balance_conf.yaw_kd * yaw_derivative);

					if(yaw_pid_value > balance_conf.yaw_current_clamp){
						yaw_pid_value = balance_conf.yaw_current_clamp;
					}else if(yaw_pid_value < -balance_conf.yaw_current_clamp){
						yaw_pid_value = -balance_conf.yaw_current_clamp;
					}

					yaw_last_proportional = yaw_proportional;
				}

				// Output to motor
				set_current(pid_value, yaw_pid_value);
				break;
			case (FAULT):
				// Check for valid startup position and switch state
				if(fabsf(pitch_angle) < balance_conf.startup_pitch_tolerance && fabsf(roll_angle) < balance_conf.startup_roll_tolerance && switch_state == ON){
					// Clear accumulated values.
					integral = 0;
					last_proportional = 0;
					yaw_integral = 0;
					yaw_last_proportional = 0;
					// Set values for startup
					setpoint = pitch_angle;
					setpoint_target = 0;
					setpoint_target_interpolated = pitch_angle;
					setpointAdjustmentType = CENTERING;
					state = RUNNING;
					break;
				}
				// Disable output
				brake();
				break;
			case (DEAD):
				// Disable output
				brake();
				break;
		}

		// Delay between loops
		chThdSleepMicroseconds((int)((1000.0 / balance_conf.hertz) * 1000.0));
	}

	// Disable output
	brake();
}
