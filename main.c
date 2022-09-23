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

#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "mc_interface.h"
#include "mcpwm.h"
#include "mcpwm_foc.h"
#include "ledpwm.h"
#include "comm_usb.h"
#include "ledpwm.h"
#include "terminal.h"
#include "hw.h"
#include "app.h"
#include "packet.h"
#include "commands.h"
#include "timeout.h"
#include "comm_can.h"
#include "ws2811.h"
#include "led_external.h"
#include "encoder.h"
#include "servo_simple.h"
#include "utils.h"
#include "nrf_driver.h"
#include "rfhelp.h"
#include "spi_sw.h"
#include "timer.h"
#include "imu.h"
#include "flash_helper.h"
#if HAS_BLACKMAGIC
#include "bm_if.h"
#endif
#include "shutdown.h"
#include "mempools.h"

/*
 * HW resources used:
 *
 * TIM1: mcpwm
 * TIM2: mcpwm_foc
 * TIM5: timer
 * TIM8: mcpwm
 * TIM3: servo_dec/Encoder (HW_R2)/servo_simple
 * TIM4: WS2811/WS2812 LEDs/Encoder (other HW)
 *
 * DMA/stream	Device		Function
 * 1, 2			I2C1		Nunchuk, temp on rev 4.5
 * 1, 7			I2C1		Nunchuk, temp on rev 4.5
 * 2, 4			ADC			mcpwm
 * 1, 0			TIM4		WS2811/WS2812 LEDs CH1 (Ch 1)
 * 1, 3			TIM4		WS2811/WS2812 LEDs CH2 (Ch 2)
 *
 */

// Private variables
static THD_WORKING_AREA(periodic_thread_wa, 1024);
static THD_WORKING_AREA(timer_thread_wa, 128);
static THD_WORKING_AREA(flash_integrity_check_thread_wa, 256);

static THD_FUNCTION(flash_integrity_check_thread, arg) {
	(void)arg;

	chRegSetThreadName("Flash check");
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_CRC, ENABLE);

	for(;;) {
		if (flash_helper_verify_flash_memory_chunk() == FAULT_CODE_FLASH_CORRUPTION) {
			NVIC_SystemReset();
		}

		chThdSleepMilliseconds(6);
	}
}

static THD_FUNCTION(periodic_thread, arg) {
	(void)arg;

	chRegSetThreadName("Main periodic");

	for(;;) {
		mc_state state1 = mc_interface_get_state();
		mc_interface_select_motor_thread(2);
		mc_state state2 = mc_interface_get_state();
		mc_interface_select_motor_thread(1);
		if ((state1 == MC_STATE_RUNNING) || (state2 == MC_STATE_RUNNING)) {
			ledpwm_set_intensity(LED_GREEN, 1.0);
		} else {
			ledpwm_set_intensity(LED_GREEN, 0.2);
		}

		mc_fault_code fault = mc_interface_get_fault();
		mc_interface_select_motor_thread(2);
		mc_fault_code fault2 = mc_interface_get_fault();
		mc_interface_select_motor_thread(1);
		if (fault != FAULT_CODE_NONE || fault2 != FAULT_CODE_NONE) {
			for (int i = 0;i < (int)fault;i++) {
				ledpwm_set_intensity(LED_RED, 1.0);
				chThdSleepMilliseconds(250);
				ledpwm_set_intensity(LED_RED, 0.0);
				chThdSleepMilliseconds(250);
			}

			chThdSleepMilliseconds(500);

			for (int i = 0;i < (int)fault2;i++) {
				ledpwm_set_intensity(LED_RED, 1.0);
				chThdSleepMilliseconds(250);
				ledpwm_set_intensity(LED_RED, 0.0);
				chThdSleepMilliseconds(250);
			}

			chThdSleepMilliseconds(500);
		} else {
			ledpwm_set_intensity(LED_RED, 0.0);
		}

		if (mc_interface_get_state() == MC_STATE_DETECTING) {
			commands_send_rotor_pos(mcpwm_get_detect_pos());
		}

		disp_pos_mode display_mode = commands_get_disp_pos_mode();

		switch (display_mode) {
		case DISP_POS_MODE_ENCODER:
			commands_send_rotor_pos(encoder_read_deg());
			break;

		case DISP_POS_MODE_PID_POS:
			commands_send_rotor_pos(mc_interface_get_pid_pos_now());
			break;

		case DISP_POS_MODE_PID_POS_ERROR:
			commands_send_rotor_pos(utils_angle_difference(mc_interface_get_pid_pos_set(), mc_interface_get_pid_pos_now()));
			break;

		default:
			break;
		}

		if (mc_interface_get_configuration()->motor_type == MOTOR_TYPE_FOC) {
			switch (display_mode) {
			case DISP_POS_MODE_OBSERVER:
				commands_send_rotor_pos(mcpwm_foc_get_phase_observer());
				break;

			case DISP_POS_MODE_ENCODER_OBSERVER_ERROR:
				commands_send_rotor_pos(utils_angle_difference(mcpwm_foc_get_phase_observer(), mcpwm_foc_get_phase_encoder()));
				break;

			default:
				break;
			}
		}

		chThdSleepMilliseconds(10);
	}
}

static THD_FUNCTION(timer_thread, arg) {
	(void)arg;

	chRegSetThreadName("msec_timer");

	for(;;) {
		packet_timerfunc();
		timeout_feed_WDT(THREAD_TIMER);
		chThdSleepMilliseconds(1);
	}
}

// When assertions enabled halve PWM frequency. The control loop ISR runs 40% slower
void assert_failed(uint8_t* file, uint32_t line) {
	commands_printf("Wrong parameters value: file %s on line %d\r\n", file, line);
	mc_interface_release_motor();
	while(1) {
		chThdSleepMilliseconds(1);
	}
}

int main(void) {
	halInit();
	chSysInit();

	// Initialize the enable pins here and disable them
	// to avoid excessive current draw at boot because of
	// floating pins.
#ifdef HW_HAS_DRV8313
	INIT_BR();
#endif

	HW_EARLY_INIT();

#ifdef BOOT_OK_GPIO
	palSetPadMode(BOOT_OK_GPIO, BOOT_OK_PIN, PAL_MODE_OUTPUT_PUSHPULL);
	palClearPad(BOOT_OK_GPIO, BOOT_OK_PIN);
#endif

	chThdSleepMilliseconds(100);

	hw_init_gpio();
	LED_RED_OFF();
	LED_GREEN_OFF();

	timer_init();
	conf_general_init();

	if( flash_helper_verify_flash_memory() == FAULT_CODE_FLASH_CORRUPTION )	{
		// Loop here, it is not safe to run any code
		while (1) {
			chThdSleepMilliseconds(100);
			LED_RED_ON();
			chThdSleepMilliseconds(75);
			LED_RED_OFF();
		}
	}

	ledpwm_init();
	mc_interface_init();

	commands_init();

#if COMM_USE_USB
	comm_usb_init();
#endif

#if CAN_ENABLE
	comm_can_init();
#endif

	app_configuration *appconf = mempools_alloc_appconf();
	conf_general_read_app_configuration(appconf);
	app_set_configuration(appconf);
	app_uartcomm_start_permanent();

#ifdef HW_HAS_PERMANENT_NRF
	conf_general_permanent_nrf_found = nrf_driver_init();
	if (conf_general_permanent_nrf_found) {
		rfhelp_restart();
	} else {
		nrf_driver_stop();
		// Set the nrf SPI pins to the general SPI interface so that
		// an external NRF can be used with the NRF app.
		spi_sw_change_pins(
				HW_SPI_PORT_NSS, HW_SPI_PIN_NSS,
				HW_SPI_PORT_SCK, HW_SPI_PIN_SCK,
				HW_SPI_PORT_MOSI, HW_SPI_PIN_MOSI,
				HW_SPI_PORT_MISO, HW_SPI_PIN_MISO);
		HW_PERMANENT_NRF_FAILED_HOOK();
	}
#endif

#if WS2811_ENABLE
	ws2811_init();
#if !WS2811_TEST
	led_external_init();
#endif
#endif

#if SERVO_OUT_ENABLE
	servo_simple_init();
#endif

	// Threads
	chThdCreateStatic(periodic_thread_wa, sizeof(periodic_thread_wa), NORMALPRIO, periodic_thread, NULL);
	chThdCreateStatic(timer_thread_wa, sizeof(timer_thread_wa), NORMALPRIO, timer_thread, NULL);
	chThdCreateStatic(flash_integrity_check_thread_wa, sizeof(flash_integrity_check_thread_wa), LOWPRIO, flash_integrity_check_thread, NULL);

#if WS2811_TEST
	unsigned int color_ind = 0;
	const int num = 4;
	const uint32_t colors[] = {COLOR_RED, COLOR_GOLD, COLOR_GRAY, COLOR_MAGENTA, COLOR_BLUE};
	const int brightness_set = 100;

	for (;;) {
		chThdSleepMilliseconds(1000);

		for (int i = 0;i < brightness_set;i++) {
			ws2811_set_brightness(i);
			chThdSleepMilliseconds(10);
		}

		chThdSleepMilliseconds(1000);

		for(int i = -num;i <= WS2811_LED_NUM;i++) {
			ws2811_set_led_color(i - 1, COLOR_BLACK);
			ws2811_set_led_color(i + num, colors[color_ind]);

			ws2811_set_led_color(0, COLOR_RED);
			ws2811_set_led_color(WS2811_LED_NUM - 1, COLOR_GREEN);

			chThdSleepMilliseconds(50);
		}

		for (int i = 0;i < brightness_set;i++) {
			ws2811_set_brightness(brightness_set - i);
			chThdSleepMilliseconds(10);
		}

		color_ind++;
		if (color_ind >= sizeof(colors) / sizeof(uint32_t)) {
			color_ind = 0;
		}

		static int asd = 0;
		asd++;
		if (asd >= 3) {
			asd = 0;

			for (unsigned int i = 0;i < sizeof(colors) / sizeof(uint32_t);i++) {
				ws2811_set_all(colors[i]);

				for (int i = 0;i < brightness_set;i++) {
					ws2811_set_brightness(i);
					chThdSleepMilliseconds(2);
				}

				chThdSleepMilliseconds(100);

				for (int i = 0;i < brightness_set;i++) {
					ws2811_set_brightness(brightness_set - i);
					chThdSleepMilliseconds(2);
				}
			}
		}
	}
#endif

	timeout_init();
	timeout_configure(appconf->timeout_msec, appconf->timeout_brake_current);
	imu_init(&appconf->imu_conf);

	mempools_free_appconf(appconf);

#if HAS_BLACKMAGIC
	bm_init();
#endif

#ifdef HW_SHUTDOWN_HOLD_ON
	shutdown_init();
#endif

#ifdef BOOT_OK_GPIO
	chThdSleepMilliseconds(500);
	palSetPad(BOOT_OK_GPIO, BOOT_OK_PIN);
#endif

	for(;;) {
		chThdSleepMilliseconds(10);
	}
}
