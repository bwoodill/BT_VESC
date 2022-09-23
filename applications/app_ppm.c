/*
	Copyright 2016 - 2019 Benjamin Vedder	benjamin@vedder.se

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

#include "app.h"

#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"
#include "servo_dec.h"
#include "mc_interface.h"
#include "timeout.h"
#include "utils.h"
#include "comm_can.h"
#include <math.h>

// Only available if servo output is not active
#if !SERVO_OUT_ENABLE

// Settings
#define MAX_CAN_AGE						0.1
#define MIN_PULSES_WITHOUT_POWER		50

// Threads
static THD_FUNCTION(ppm_thread, arg);
static THD_WORKING_AREA(ppm_thread_wa, 1536);
static thread_t *ppm_tp;
static volatile bool ppm_rx = false;

// Private functions
static void servodec_func(void);

// Private variables
static volatile bool is_running = false;
static volatile bool stop_now = true;
static volatile ppm_config config;
static volatile int pulses_without_power = 0;
static float input_val = 0.0;
static volatile float direction_hyst = 0;

// Private functions
#endif

void app_ppm_configure(ppm_config *conf) {
#if !SERVO_OUT_ENABLE
	config = *conf;
	pulses_without_power = 0;

	if (is_running) {
		servodec_set_pulse_options(config.pulse_start, config.pulse_end, config.median_filter);
	}

	direction_hyst = config.max_erpm_for_dir * 0.20;
#else
	(void)conf;
#endif
}

void app_ppm_start(void) {
#if !SERVO_OUT_ENABLE
	stop_now = false;
	chThdCreateStatic(ppm_thread_wa, sizeof(ppm_thread_wa), NORMALPRIO, ppm_thread, NULL);
#endif
}

void app_ppm_stop(void) {
#if !SERVO_OUT_ENABLE
	stop_now = true;

	if (is_running) {
		chEvtSignalI(ppm_tp, (eventmask_t) 1);
		servodec_stop();
	}

	while(is_running) {
		chThdSleepMilliseconds(1);
	}
#endif
}

float app_ppm_get_decoded_level(void) {
#if !SERVO_OUT_ENABLE
	return input_val;
#else
	return 0.0;
#endif
}

#if !SERVO_OUT_ENABLE
static void servodec_func(void) {
	ppm_rx = true;
	chSysLockFromISR();
	chEvtSignalI(ppm_tp, (eventmask_t) 1);
	chSysUnlockFromISR();
}

static THD_FUNCTION(ppm_thread, arg) {
	(void)arg;

	chRegSetThreadName("APP_PPM");
	ppm_tp = chThdGetSelfX();

	servodec_set_pulse_options(config.pulse_start, config.pulse_end, config.median_filter);
	servodec_init(servodec_func);
	is_running = true;

	for(;;) {
		chEvtWaitAnyTimeout((eventmask_t)1, MS2ST(2));

		if (stop_now) {
			is_running = false;
			return;
		}

		if (ppm_rx) {
			ppm_rx = false;
			timeout_reset();
		}

		const volatile mc_configuration *mcconf = mc_interface_get_configuration();
		const float rpm_now = mc_interface_get_rpm();
		float servo_val = servodec_get_servo(0);
		float servo_ms = utils_map(servo_val, -1.0, 1.0, config.pulse_start, config.pulse_end);

		switch (config.ctrl_type) {
		case PPM_CTRL_TYPE_CURRENT_NOREV:
		case PPM_CTRL_TYPE_DUTY_NOREV:
		case PPM_CTRL_TYPE_PID_NOREV:
			input_val = servo_val;
			servo_val += 1.0;
			servo_val /= 2.0;
			break;

		default:
			// Mapping with respect to center pulsewidth
			if (servo_ms < config.pulse_center) {
				servo_val = utils_map(servo_ms, config.pulse_start,
						config.pulse_center, -1.0, 0.0);
			} else {
				servo_val = utils_map(servo_ms, config.pulse_center,
						config.pulse_end, 0.0, 1.0);
			}
			input_val = servo_val;
			break;
		}

		// All pins and buttons are still decoded for debugging, even
		// when output is disabled.
		if (app_is_output_disabled()) {
			continue;
		}

		if (timeout_has_timeout() || servodec_get_time_since_update() > timeout_get_timeout_msec() ||
				mc_interface_get_fault() != FAULT_CODE_NONE) {
			pulses_without_power = 0;
			continue;
		}

		// Apply deadband
		utils_deadband(&servo_val, config.hyst, 1.0);

		// Apply throttle curve
		servo_val = utils_throttle_curve(servo_val, config.throttle_exp, config.throttle_exp_brake, config.throttle_exp_mode);

		// Apply ramping
		static systime_t last_time = 0;
		static float servo_val_ramp = 0.0;
		float ramp_time = fabsf(servo_val) > fabsf(servo_val_ramp) ? config.ramp_time_pos : config.ramp_time_neg;

		// TODO: Remember what this was about?
//		if (fabsf(servo_val) > 0.001) {
//			ramp_time = fminf(config.ramp_time_pos, config.ramp_time_neg);
//		}

		const float dt = (float)ST2MS(chVTTimeElapsedSinceX(last_time)) / 1000.0;
		last_time = chVTGetSystemTimeX();

		if (ramp_time > 0.01) {
			const float ramp_step = dt / ramp_time;
			utils_step_towards(&servo_val_ramp, servo_val, ramp_step);
			servo_val = servo_val_ramp;
		}

		float current = 0;
		bool current_mode = false;
		bool current_mode_brake = false;
		bool send_current = false;
		bool send_duty = false;
		static bool force_brake = true;
		static int8_t did_idle_once = 0;
		float rpm_local = mc_interface_get_rpm();
		float rpm_lowest = rpm_local;

		switch (config.ctrl_type) {
		case PPM_CTRL_TYPE_CURRENT_BRAKE_REV_HYST:
			current_mode = true;

			// Hysteresis 20 % of actual RPM
			if (force_brake) {
				if (rpm_local < config.max_erpm_for_dir - direction_hyst) { // for 2500 it's 2000
					force_brake = false;
					did_idle_once = 0;
				}
			} else {
				if (rpm_local > config.max_erpm_for_dir + direction_hyst) { // for 2500 it's 3000
					force_brake = true;
					did_idle_once = 0;
				}
			}

			if (servo_val >= 0.0) {
				if (servo_val == 0.0) {
					// if there was a idle in between then allow going backwards
					if (did_idle_once == 1 && !force_brake) {
						did_idle_once = 2;
					}
				} else{
					// accelerated forward or fast enough at least
					if (rpm_local > -config.max_erpm_for_dir){ // for 2500 it's -2500
						did_idle_once = 0;
					}
				}

				current = servo_val * mcconf->lo_current_motor_max_now;
			} else {
				// too fast
				if (force_brake){
					current_mode_brake = true;
				} else{
					// not too fast backwards
					if (rpm_local > -config.max_erpm_for_dir) { // for 2500 it's -2500
						// first time that we brake and we are not too fast
						if (did_idle_once != 2) {
							did_idle_once = 1;
							current_mode_brake = true;
						}
					// too fast backwards
					} else {
						// if brake was active already
						if (did_idle_once == 1) {
							current_mode_brake = true;
						} else {
							// it's ok to go backwards now braking would be strange now
							did_idle_once = 2;
						}
					}
				}

				if (current_mode_brake) {
					// braking
					current = fabsf(servo_val * mcconf->lo_current_motor_min_now);
				} else {
					// reverse acceleration
					current = servo_val * fabsf(mcconf->lo_current_motor_min_now);
				}
			}

			if (fabsf(servo_val) < 0.001) {
				pulses_without_power++;
			}

			break;
		case PPM_CTRL_TYPE_CURRENT:
		case PPM_CTRL_TYPE_CURRENT_NOREV:
			current_mode = true;
			if ((servo_val >= 0.0 && rpm_now > 0.0) || (servo_val < 0.0 && rpm_now < 0.0)) {
				current = servo_val * mcconf->lo_current_motor_max_now;
			} else {
				current = servo_val * fabsf(mcconf->lo_current_motor_min_now);
			}

			if (fabsf(servo_val) < 0.001) {
				pulses_without_power++;
			}
			break;

		case PPM_CTRL_TYPE_CURRENT_NOREV_BRAKE:
		case PPM_CTRL_TYPE_CURRENT_SMART_REV:
			current_mode = true;
			current_mode_brake = servo_val < 0.0;

			if (servo_val >= 0.0 && rpm_now > 0.0) {
				current = servo_val * mcconf->lo_current_motor_max_now;
			} else {
				current = fabsf(servo_val * mcconf->lo_current_motor_min_now);
			}

			if (fabsf(servo_val) < 0.001) {
				pulses_without_power++;
			}
			break;

		case PPM_CTRL_TYPE_DUTY:
		case PPM_CTRL_TYPE_DUTY_NOREV:
			if (fabsf(servo_val) < 0.001) {
				pulses_without_power++;
			}

			if (!(pulses_without_power < MIN_PULSES_WITHOUT_POWER && config.safe_start)) {
				mc_interface_set_duty(utils_map(servo_val, -1.0, 1.0, -mcconf->l_max_duty, mcconf->l_max_duty));
				send_duty = true;
			}
			break;

		case PPM_CTRL_TYPE_PID:
		case PPM_CTRL_TYPE_PID_NOREV:
			if (fabsf(servo_val) < 0.001) {
				pulses_without_power++;
			}

			if (!(pulses_without_power < MIN_PULSES_WITHOUT_POWER && config.safe_start)) {
				mc_interface_set_pid_speed(servo_val * config.pid_max_erpm);
				send_current = true;
			}
			break;

		default:
			continue;
		}

		if (pulses_without_power < MIN_PULSES_WITHOUT_POWER && config.safe_start) {
			static int pulses_without_power_before = 0;
			if (pulses_without_power == pulses_without_power_before) {
				pulses_without_power = 0;
			}
			pulses_without_power_before = pulses_without_power;
			mc_interface_set_brake_current(timeout_get_brake_current());
			continue;
		}

		const float duty_now = mc_interface_get_duty_cycle_now();
		float current_highest_abs = fabsf(mc_interface_get_tot_current_directional_filtered());
		float duty_highest_abs = fabsf(duty_now);

		if (config.multi_esc) {
			for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
				can_status_msg *msg = comm_can_get_status_msg_index(i);

				if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
					if (fabsf(msg->rpm) < fabsf(rpm_lowest)) {
						rpm_lowest = msg->rpm;
					}

					if (fabsf(msg->current) > current_highest_abs) {
						current_highest_abs = fabsf(msg->current);
					}

					if (fabsf(msg->duty) > duty_highest_abs) {
						duty_highest_abs = fabsf(msg->duty);
					}
				}
			}
		}

		if (config.ctrl_type == PPM_CTRL_TYPE_CURRENT_SMART_REV) {
			bool duty_control = false;
			static bool was_duty_control = false;
			static float duty_rev = 0.0;

			if (servo_val < -0.92 && duty_highest_abs < (mcconf->l_min_duty * 1.5) &&
					current_highest_abs < (mcconf->l_current_max * mcconf->l_current_max_scale * 0.7)) {
				duty_control = true;
			}

			if (duty_control || (was_duty_control && servo_val < -0.1)) {
				was_duty_control = true;

				float goal = config.smart_rev_max_duty * -servo_val;
				utils_step_towards(&duty_rev, -goal,
						config.smart_rev_max_duty * dt / config.smart_rev_ramp_time);

				mc_interface_set_duty(duty_rev);

				// Send the same duty cycle to the other controllers
				if (config.multi_esc) {
					for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
						can_status_msg *msg = comm_can_get_status_msg_index(i);

						if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
							comm_can_set_duty(msg->id, duty_rev);
						}
					}
				}

				current_mode = false;
			} else {
				duty_rev = duty_now;
				was_duty_control = false;
			}
		}

		if ((send_current || send_duty) && config.multi_esc) {
			float current_filtered = mc_interface_get_tot_current_directional_filtered();
			float duty = mc_interface_get_duty_cycle_now();

			for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
				can_status_msg *msg = comm_can_get_status_msg_index(i);

				if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
					if (send_current) {
						comm_can_set_current(msg->id, current_filtered);
					} else if (send_duty) {
						comm_can_set_duty(msg->id, duty);
					}
				}
			}
		}

		if (current_mode) {
			if (current_mode_brake) {
				mc_interface_set_brake_current(current);

				// Send brake command to all ESCs seen recently on the CAN bus
				for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
					can_status_msg *msg = comm_can_get_status_msg_index(i);

					if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
						comm_can_set_current_brake(msg->id, current);
					}
				}
			} else {
				float current_out = current;
				bool is_reverse = false;
				if (current_out < 0.0) {
					is_reverse = true;
					current_out = -current_out;
					current = -current;
					rpm_local = -rpm_local;
					rpm_lowest = -rpm_lowest;
				}

				// Traction control
				if (config.multi_esc) {
					for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
						can_status_msg *msg = comm_can_get_status_msg_index(i);

						if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
							if (config.tc) {
								float rpm_tmp = msg->rpm;
								if (is_reverse) {
									rpm_tmp = -rpm_tmp;
								}

								float diff = rpm_tmp - rpm_lowest;
								current_out = utils_map(diff, 0.0, config.tc_max_diff, current, 0.0);
								if (current_out < mcconf->cc_min_current) {
									current_out = 0.0;
								}
							}

							if (is_reverse) {
								comm_can_set_current(msg->id, -current_out);
							} else {
								comm_can_set_current(msg->id, current_out);
							}
						}
					}

					if (config.tc) {
						float diff = rpm_local - rpm_lowest;
						current_out = utils_map(diff, 0.0, config.tc_max_diff, current, 0.0);
						if (current_out < mcconf->cc_min_current) {
							current_out = 0.0;
						}
					}
				}

				if (is_reverse) {
					mc_interface_set_current(-current_out);
				} else {
					mc_interface_set_current(current_out);
				}
			}
		}

	}
}
#endif
