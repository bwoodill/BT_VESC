/*
	Copyright 2016 - 2020 Benjamin Vedder	benjamin@vedder.se

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

#include <string.h>
#include <math.h>
#include "comm_can.h"
#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"
#include "datatypes.h"
#include "buffer.h"
#include "mc_interface.h"
#include "timeout.h"
#include "commands.h"
#include "app.h"
#include "crc.h"
#include "packet.h"
#include "hw.h"
#include "canard_driver.h"
#include "encoder.h"
#include "utils.h"
#include "mempools.h"
#include "shutdown.h"

// Settings
#define RX_FRAMES_SIZE	100
#define RX_BUFFER_SIZE	PACKET_MAX_PL_LEN

#if CAN_ENABLE
// Threads
static THD_WORKING_AREA(cancom_read_thread_wa, 512);
static THD_WORKING_AREA(cancom_process_thread_wa, 4096);
static THD_WORKING_AREA(cancom_status_thread_wa, 512);
static THD_FUNCTION(cancom_read_thread, arg);
static THD_FUNCTION(cancom_status_thread, arg);
static THD_FUNCTION(cancom_process_thread, arg);

#ifdef HW_HAS_DUAL_MOTORS
static THD_FUNCTION(cancom_status_internal_thread, arg);
static THD_WORKING_AREA(cancom_status_internal_thread_wa, 512);
#endif

static mutex_t can_mtx;
static mutex_t can_rx_mtx;
static uint8_t rx_buffer[RX_BUFFER_SIZE];
static unsigned int rx_buffer_last_id;
static CANRxFrame rx_frames[RX_FRAMES_SIZE];
static int rx_frame_read;
static int rx_frame_write;
static thread_t *process_tp = 0;
static thread_t *ping_tp = 0;
#endif

// Variables
static can_status_msg stat_msgs[CAN_STATUS_MSGS_TO_STORE];
static can_status_msg_2 stat_msgs_2[CAN_STATUS_MSGS_TO_STORE];
static can_status_msg_3 stat_msgs_3[CAN_STATUS_MSGS_TO_STORE];
static can_status_msg_4 stat_msgs_4[CAN_STATUS_MSGS_TO_STORE];
static can_status_msg_5 stat_msgs_5[CAN_STATUS_MSGS_TO_STORE];
static unsigned int detect_all_foc_res_index = 0;
static int8_t detect_all_foc_res[50];

/*
 * 500KBaud, automatic wakeup, automatic recover
 * from abort mode.
 * See section 22.7.7 on the STM32 reference manual.
 */
static CANConfig cancfg = {
		CAN_MCR_ABOM | CAN_MCR_AWUM | CAN_MCR_TXFP,
		CAN_BTR_SJW(3) | CAN_BTR_TS2(2) |
		CAN_BTR_TS1(9) | CAN_BTR_BRP(5)
};

// Private functions
static void set_timing(int brp, int ts1, int ts2);
#if CAN_ENABLE
static void send_packet_wrapper(unsigned char *data, unsigned int len);
static void decode_msg(uint32_t eid, uint8_t *data8, int len, bool is_replaced);
static void send_status1(uint8_t id, bool replace);
static void send_status2(uint8_t id, bool replace);
static void send_status3(uint8_t id, bool replace);
static void send_status4(uint8_t id, bool replace);
static void send_status5(uint8_t id, bool replace);
#endif

// Function pointers
static void(*sid_callback)(uint32_t id, uint8_t *data, uint8_t len) = 0;
static void(*eid_callback)(uint32_t id, uint8_t *data, uint8_t len) = 0;

void comm_can_init(void) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		stat_msgs[i].id = -1;
		stat_msgs_2[i].id = -1;
		stat_msgs_3[i].id = -1;
		stat_msgs_4[i].id = -1;
	}

#if CAN_ENABLE
	rx_frame_read = 0;
	rx_frame_write = 0;

	chMtxObjectInit(&can_mtx);
	chMtxObjectInit(&can_rx_mtx);

	palSetPadMode(HW_CANRX_PORT, HW_CANRX_PIN,
			PAL_MODE_ALTERNATE(HW_CAN_GPIO_AF) |
			PAL_STM32_OTYPE_PUSHPULL |
			PAL_STM32_OSPEED_MID1);
	palSetPadMode(HW_CANTX_PORT, HW_CANTX_PIN,
			PAL_MODE_ALTERNATE(HW_CAN_GPIO_AF) |
			PAL_STM32_OTYPE_PUSHPULL |
			PAL_STM32_OSPEED_MID1);

	canStart(&HW_CAN_DEV, &cancfg);

	canard_driver_init();

	chThdCreateStatic(cancom_read_thread_wa, sizeof(cancom_read_thread_wa), NORMALPRIO + 1,
			cancom_read_thread, NULL);
	chThdCreateStatic(cancom_status_thread_wa, sizeof(cancom_status_thread_wa), NORMALPRIO,
			cancom_status_thread, NULL);
	chThdCreateStatic(cancom_process_thread_wa, sizeof(cancom_process_thread_wa), NORMALPRIO,
			cancom_process_thread, NULL);
#ifdef HW_HAS_DUAL_MOTORS
	chThdCreateStatic(cancom_status_internal_thread_wa, sizeof(cancom_status_internal_thread_wa),
			NORMALPRIO, cancom_status_internal_thread, NULL);
#endif

#endif
}

void comm_can_set_baud(CAN_BAUD baud) {
	switch (baud) {
	case CAN_BAUD_125K:	set_timing(15, 14, 4); break;
	case CAN_BAUD_250K:	set_timing(7, 14, 4); break;
	case CAN_BAUD_500K:	set_timing(5, 9, 2); break;
	case CAN_BAUD_1M:	set_timing(2, 9, 2); break;
	case CAN_BAUD_10K:	set_timing(299, 10, 1); break;
	case CAN_BAUD_20K:	set_timing(149, 10, 1); break;
	case CAN_BAUD_50K:	set_timing(59, 10, 1); break;
	case CAN_BAUD_75K:	set_timing(39, 10, 1); break;
	default: break;
	}
}

/**
 * Transmit CAN packet with extended ID.
 *
 * @param id
 * EID
 *
 * @param data
 * Data
 *
 * @param len
 * Length of data, max 8 bytes.
 *
 * @param replace
 * Process packets for motor2 directly instead of sending them. Unused
 * on single motor hardware.
 */
void comm_can_transmit_eid_replace(uint32_t id, const uint8_t *data, uint8_t len, bool replace) {
	if (len > 8) {
		len = 8;
	}

#if CAN_ENABLE
#ifdef HW_HAS_DUAL_MOTORS
	if (app_get_configuration()->can_mode == CAN_MODE_VESC) {
		if (replace && ((id & 0xFF) == utils_second_motor_id() ||
				(id & 0xFF) == app_get_configuration()->controller_id)) {
			uint8_t data_tmp[10];
			memcpy(data_tmp, data, len);
			decode_msg(id, data_tmp, len, true);
			return;
		}
	}
#else
	(void)replace;
#endif

	CANTxFrame txmsg;
	txmsg.IDE = CAN_IDE_EXT;
	txmsg.EID = id;
	txmsg.RTR = CAN_RTR_DATA;
	txmsg.DLC = len;
	memcpy(txmsg.data8, data, len);

	chMtxLock(&can_mtx);
	canTransmit(&HW_CAN_DEV, CAN_ANY_MAILBOX, &txmsg, MS2ST(5));
	chMtxUnlock(&can_mtx);
#else
	(void)id;
	(void)data;
	(void)len;
	(void)replace;
#endif
}

void comm_can_transmit_eid(uint32_t id, const uint8_t *data, uint8_t len) {
	comm_can_transmit_eid_replace(id, data, len, true);
}

void comm_can_transmit_sid(uint32_t id, uint8_t *data, uint8_t len) {
	if (len > 8) {
		len = 8;
	}

#if CAN_ENABLE
	CANTxFrame txmsg;
	txmsg.IDE = CAN_IDE_STD;
	txmsg.SID = id;
	txmsg.RTR = CAN_RTR_DATA;
	txmsg.DLC = len;
	memcpy(txmsg.data8, data, len);

	chMtxLock(&can_mtx);
	canTransmit(&HW_CAN_DEV, CAN_ANY_MAILBOX, &txmsg, MS2ST(5));
	chMtxUnlock(&can_mtx);
#else
	(void)id;
	(void)data;
	(void)len;
#endif
}

/**
 * Set function to be called when standard CAN frames are received.
 *
 * @param p_func
 * Pointer to the function.
 */
void comm_can_set_sid_rx_callback(void (*p_func)(uint32_t id, uint8_t *data, uint8_t len)) {
	sid_callback = p_func;
}

/**
 * Set function to be called when extended CAN frames are received. Will only be called when
 * the CAN mode is CAN_MODE_COMM_BRIDGE.
 *
 * @param p_func
 * Pointer to the function.
 */
void comm_can_set_eid_rx_callback(void (*p_func)(uint32_t id, uint8_t *data, uint8_t len)) {
	eid_callback = p_func;
}

/**
 * Send a buffer up to RX_BUFFER_SIZE bytes as fragments. If the buffer is 6 bytes or less
 * it will be sent in a single CAN frame, otherwise it will be split into
 * several frames.
 *
 * @param controller_id
 * The controller id to send to.
 *
 * @param data
 * The payload.
 *
 * @param len
 * The payload length.
 *
 * @param send
 * 0: Packet goes to commands_process_packet of receiver
 * 1: Packet goes to commands_send_packet of receiver
 * 2: Packet goes to commands_process and send function is set to null
 *    so that no reply is sent back.
 */
void comm_can_send_buffer(uint8_t controller_id, uint8_t *data, unsigned int len, uint8_t send) {
	uint8_t send_buffer[8];

	if (len <= 6) {
		uint32_t ind = 0;
		send_buffer[ind++] = app_get_configuration()->controller_id;
		send_buffer[ind++] = send;
		memcpy(send_buffer + ind, data, len);
		ind += len;
		comm_can_transmit_eid(controller_id |
				((uint32_t)CAN_PACKET_PROCESS_SHORT_BUFFER << 8), send_buffer, ind);
	} else {
		unsigned int end_a = 0;
		for (unsigned int i = 0;i < len;i += 7) {
			if (i > 255) {
				break;
			}

			end_a = i + 7;

			uint8_t send_len = 7;
			send_buffer[0] = i;

			if ((i + 7) <= len) {
				memcpy(send_buffer + 1, data + i, send_len);
			} else {
				send_len = len - i;
				memcpy(send_buffer + 1, data + i, send_len);
			}

			comm_can_transmit_eid(controller_id |
					((uint32_t)CAN_PACKET_FILL_RX_BUFFER << 8), send_buffer, send_len + 1);
		}

		for (unsigned int i = end_a;i < len;i += 6) {
			uint8_t send_len = 6;
			send_buffer[0] = i >> 8;
			send_buffer[1] = i & 0xFF;

			if ((i + 6) <= len) {
				memcpy(send_buffer + 2, data + i, send_len);
			} else {
				send_len = len - i;
				memcpy(send_buffer + 2, data + i, send_len);
			}

			comm_can_transmit_eid(controller_id |
					((uint32_t)CAN_PACKET_FILL_RX_BUFFER_LONG << 8), send_buffer, send_len + 2);
		}

		uint32_t ind = 0;
		send_buffer[ind++] = app_get_configuration()->controller_id;
		send_buffer[ind++] = send;
		send_buffer[ind++] = len >> 8;
		send_buffer[ind++] = len & 0xFF;
		unsigned short crc = crc16(data, len);
		send_buffer[ind++] = (uint8_t)(crc >> 8);
		send_buffer[ind++] = (uint8_t)(crc & 0xFF);

		comm_can_transmit_eid(controller_id |
				((uint32_t)CAN_PACKET_PROCESS_RX_BUFFER << 8), send_buffer, ind++);
	}
}

void comm_can_set_duty(uint8_t controller_id, float duty) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_int32(buffer, (int32_t)(duty * 100000.0), &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_DUTY << 8), buffer, send_index);
}

void comm_can_set_current(uint8_t controller_id, float current) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_int32(buffer, (int32_t)(current * 1000.0), &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_CURRENT << 8), buffer, send_index);
}

void comm_can_set_current_brake(uint8_t controller_id, float current) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_int32(buffer, (int32_t)(current * 1000.0), &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_CURRENT_BRAKE << 8), buffer, send_index);
}

void comm_can_set_rpm(uint8_t controller_id, float rpm) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_int32(buffer, (int32_t)rpm, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_RPM << 8), buffer, send_index);
}

void comm_can_set_pos(uint8_t controller_id, float pos) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_int32(buffer, (int32_t)(pos * 1000000.0), &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_POS << 8), buffer, send_index);
}

/**
 * Set current relative to the minimum and maximum current limits.
 *
 * @param controller_id
 * The ID of the VESC to set the current on.
 *
 * @param current_rel
 * The relative current value, range [-1.0 1.0]
 */
void comm_can_set_current_rel(uint8_t controller_id, float current_rel) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_float32(buffer, current_rel, 1e5, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_CURRENT_REL << 8), buffer, send_index);
}

/**
 * Set brake current relative to the minimum current limit.
 *
 * @param controller_id
 * The ID of the VESC to set the current on.
 *
 * @param current_rel
 * The relative current value, range [0.0 1.0]
 */
void comm_can_set_current_brake_rel(uint8_t controller_id, float current_rel) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_float32(buffer, current_rel, 1e5, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_CURRENT_BRAKE_REL << 8), buffer, send_index);
}

/**
 * Set handbrake current.
 *
 * @param controller_id
 * The ID of the VESC to set the handbrake current on.
 *
 * @param current_rel
 * The handbrake current value
 */
void comm_can_set_handbrake(uint8_t controller_id, float current) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_float32(buffer, current, 1e3, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_CURRENT_HANDBRAKE << 8), buffer, send_index);
}

/**
 * Set handbrake current relative to the minimum current limit.
 *
 * @param controller_id
 * The ID of the VESC to set the handbrake current on.
 *
 * @param current_rel
 * The relative handbrake current value, range [0.0 1.0]
 */
void comm_can_set_handbrake_rel(uint8_t controller_id, float current_rel) {
	int32_t send_index = 0;
	uint8_t buffer[4];
	buffer_append_float32(buffer, current_rel, 1e5, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_SET_CURRENT_HANDBRAKE_REL << 8), buffer, send_index);
}

/**
 * Check if a VESC on the CAN-bus responds.
 *
 * @param controller_id
 * The ID of the VESC.
 *
 * @return
 * True for success, false otherwise.
 */
bool comm_can_ping(uint8_t controller_id) {
#if CAN_ENABLE
	if (app_get_configuration()->can_mode != CAN_MODE_VESC) {
		return false;
	}

#ifdef HW_HAS_DUAL_MOTORS
	if (controller_id == app_get_configuration()->controller_id) {
		return false;
	}
#endif

	ping_tp = chThdGetSelfX();
	chEvtGetAndClearEvents(ALL_EVENTS);

	uint8_t buffer[1];
	buffer[0] = app_get_configuration()->controller_id;
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_PING << 8), buffer, 1);

	int ret = chEvtWaitAnyTimeout(1 << 29, MS2ST(10));
	ping_tp = 0;
	return ret != 0;
#else
	(void)controller_id;
	return 0;
#endif
}

/**
 * Detect and apply FOC settings.
 *
 * @param controller_id
 * The ID of the VESC.
 *
 * @param activate_status_msgs
 * Activate CAN status messages on the target VESC on success.
 *
 * @param max_power_loss
 * Maximum accepted power losses.
 */
void comm_can_detect_apply_all_foc(uint8_t controller_id, bool activate_status_msgs, float max_power_loss) {
	int32_t send_index = 0;
	uint8_t buffer[6];
	buffer[send_index++] = app_get_configuration()->controller_id;
	buffer[send_index++] = activate_status_msgs;
	buffer_append_float32(buffer, max_power_loss, 1e3, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)CAN_PACKET_DETECT_APPLY_ALL_FOC << 8), buffer, send_index);
}

/**
 * Update current limits on VESC on CAN-bus.
 *
 * @param controller_id
 * ID of the VESC.
 *
 * @param store
 * Store parameters in emulated EEPROM (FLASH).
 *
 * @param min
 * Minimum current (negative value).
 *
 * @param max
 * Maximum current.
 */
void comm_can_conf_current_limits(uint8_t controller_id,
		bool store, float min, float max) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	buffer_append_float32(buffer, min, 1e3, &send_index);
	buffer_append_float32(buffer, max, 1e3, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)(store ? CAN_PACKET_CONF_STORE_CURRENT_LIMITS :
					CAN_PACKET_CONF_CURRENT_LIMITS) << 8), buffer, send_index);
}

/**
 * Update input current limits on VESC on CAN-bus.
 *
 * @param controller_id
 * ID of the VESC.
 *
 * @param store
 * Store parameters in emulated EEPROM (FLASH).
 *
 * @param min
 * Minimum current (negative value).
 *
 * @param max
 * Maximum current.
 */
void comm_can_conf_current_limits_in(uint8_t controller_id,
		bool store, float min, float max) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	buffer_append_float32(buffer, min, 1e3, &send_index);
	buffer_append_float32(buffer, max, 1e3, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)(store ? CAN_PACKET_CONF_STORE_CURRENT_LIMITS_IN :
					CAN_PACKET_CONF_CURRENT_LIMITS_IN) << 8), buffer, send_index);
}

/**
 * Update FOC ERPM settings on VESC on CAN-bus.
 *
 * @param controller_id
 * ID of the VESC.
 *
 * @param store
 * Store parameters in emulated EEPROM (FLASH).
 *
 * @param foc_openloop_rpm
 * Run in openloop below this ERPM in sensorless mode.
 *
 * @param foc_sl_erpm
 * Use sensors below this ERPM in sensored mode.
 */
void comm_can_conf_foc_erpms(uint8_t controller_id,
		bool store, float foc_openloop_rpm, float foc_sl_erpm) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	buffer_append_float32(buffer, foc_openloop_rpm, 1e3, &send_index);
	buffer_append_float32(buffer, foc_sl_erpm, 1e3, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)(store ? CAN_PACKET_CONF_STORE_FOC_ERPMS :
					CAN_PACKET_CONF_FOC_ERPMS) << 8), buffer, send_index);
}

int comm_can_detect_all_foc_res(unsigned int index) {
	if (index < detect_all_foc_res_index) {
		return detect_all_foc_res[detect_all_foc_res_index];
	} else {
		return -999;
	}
}

int comm_can_detect_all_foc_res_size(void) {
	return detect_all_foc_res_index;
}

void comm_can_detect_all_foc_res_clear(void) {
	detect_all_foc_res_index = 0;
}

void comm_can_conf_battery_cut(uint8_t controller_id,
		bool store, float start, float end) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	buffer_append_float32(buffer, start, 1e3, &send_index);
	buffer_append_float32(buffer, end, 1e3, &send_index);
	comm_can_transmit_eid(controller_id |
			((uint32_t)(store ? CAN_PACKET_CONF_STORE_BATTERY_CUT :
					CAN_PACKET_CONF_BATTERY_CUT) << 8), buffer, send_index);
}

void comm_can_shutdown(uint8_t controller_id) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	comm_can_transmit_eid(controller_id |
			((uint32_t)(CAN_PACKET_SHUTDOWN) << 8), buffer, send_index);
}

/**
 * Get status message by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg *comm_can_get_status_msg_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs[index];
	} else {
		return 0;
	}
}

/**
 * Get status message by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg *comm_can_get_status_msg_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs[i].id == id) {
			return &stat_msgs[i];
		}
	}

	return 0;
}

/**
 * Get status message 2 by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg_2 *comm_can_get_status_msg_2_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs_2[index];
	} else {
		return 0;
	}
}

/**
 * Get status message 2 by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg_2 *comm_can_get_status_msg_2_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs_2[i].id == id) {
			return &stat_msgs_2[i];
		}
	}

	return 0;
}

/**
 * Get status message 3 by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg_3 *comm_can_get_status_msg_3_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs_3[index];
	} else {
		return 0;
	}
}

/**
 * Get status message 3 by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg_3 *comm_can_get_status_msg_3_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs_3[i].id == id) {
			return &stat_msgs_3[i];
		}
	}

	return 0;
}

/**
 * Get status message 4 by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg_4 *comm_can_get_status_msg_4_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs_4[index];
	} else {
		return 0;
	}
}

/**
 * Get status message 4 by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg_4 *comm_can_get_status_msg_4_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs_4[i].id == id) {
			return &stat_msgs_4[i];
		}
	}

	return 0;
}

/**
 * Get status message 5 by index.
 *
 * @param index
 * Index in the array
 *
 * @return
 * The message or 0 for an invalid index.
 */
can_status_msg_5 *comm_can_get_status_msg_5_index(int index) {
	if (index < CAN_STATUS_MSGS_TO_STORE) {
		return &stat_msgs_5[index];
	} else {
		return 0;
	}
}

/**
 * Get status message 5 by id.
 *
 * @param id
 * Id of the controller that sent the status message.
 *
 * @return
 * The message or 0 for an invalid id.
 */
can_status_msg_5 *comm_can_get_status_msg_5_id(int id) {
	for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
		if (stat_msgs_5[i].id == id) {
			return &stat_msgs_5[i];
		}
	}

	return 0;
}

CANRxFrame *comm_can_get_rx_frame(void) {
#if CAN_ENABLE
	chMtxLock(&can_rx_mtx);
	if (rx_frame_read != rx_frame_write) {
		CANRxFrame *res = &rx_frames[rx_frame_read++];

		if (rx_frame_read == RX_FRAMES_SIZE) {
			rx_frame_read = 0;
		}

		chMtxUnlock(&can_rx_mtx);
		return res;
	} else {
		chMtxUnlock(&can_rx_mtx);
		return 0;
	}
#else
	return 0;
#endif
}

#if CAN_ENABLE
static THD_FUNCTION(cancom_read_thread, arg) {
	(void)arg;
	chRegSetThreadName("CAN read");

	event_listener_t el;
	CANRxFrame rxmsg;

	chEvtRegister(&HW_CAN_DEV.rxfull_event, &el, 0);

	while(!chThdShouldTerminateX()) {
		// Feed watchdog
		timeout_feed_WDT(THREAD_CANBUS);
        
		if (chEvtWaitAnyTimeout(ALL_EVENTS, MS2ST(10)) == 0) {
			continue;
		}

		msg_t result = canReceive(&HW_CAN_DEV, CAN_ANY_MAILBOX, &rxmsg, TIME_IMMEDIATE);

		while (result == MSG_OK) {
			chMtxLock(&can_rx_mtx);
			rx_frames[rx_frame_write++] = rxmsg;
			if (rx_frame_write == RX_FRAMES_SIZE) {
				rx_frame_write = 0;
			}
			chMtxUnlock(&can_rx_mtx);

			chEvtSignal(process_tp, (eventmask_t) 1);

			result = canReceive(&HW_CAN_DEV, CAN_ANY_MAILBOX, &rxmsg, TIME_IMMEDIATE);
		}
	}

	chEvtUnregister(&HW_CAN_DEV.rxfull_event, &el);
}

static THD_FUNCTION(cancom_process_thread, arg) {
	(void)arg;

	chRegSetThreadName("CAN process");
	process_tp = chThdGetSelfX();

	for(;;) {
		chEvtWaitAny((eventmask_t)1);

		if (app_get_configuration()->can_mode == CAN_MODE_UAVCAN) {
			continue;
		} else if (app_get_configuration()->can_mode == CAN_MODE_COMM_BRIDGE) {
			CANRxFrame *rxmsg_tmp;
			while ((rxmsg_tmp = comm_can_get_rx_frame()) != 0) {
				CANRxFrame rxmsg = *rxmsg_tmp;
				commands_fwd_can_frame(rxmsg.DLC, rxmsg.data8,
						rxmsg.IDE == CAN_IDE_EXT ? rxmsg.EID : rxmsg.SID,
						rxmsg.IDE == CAN_IDE_EXT);

				if (rxmsg.IDE == CAN_IDE_STD) {
					if (sid_callback) {
						sid_callback(rxmsg.SID, rxmsg.data8, rxmsg.DLC);
					}
				} else {
					if (eid_callback) {
						eid_callback(rxmsg.EID, rxmsg.data8, rxmsg.DLC);
					}
				}
			}
			continue;
		}

		CANRxFrame *rxmsg_tmp;
		while ((rxmsg_tmp = comm_can_get_rx_frame()) != 0) {
			CANRxFrame rxmsg = *rxmsg_tmp;

			if (rxmsg.IDE == CAN_IDE_EXT) {
				decode_msg(rxmsg.EID, rxmsg.data8, rxmsg.DLC, false);
			} else {
				if (sid_callback) {
					sid_callback(rxmsg.SID, rxmsg.data8, rxmsg.DLC);
				}
			}
		}
	}
}

#ifdef HW_HAS_DUAL_MOTORS
/*
 * This thread sends all status messages and uses the internal decoer. That
 * way the second motor always show up on the CAN-bus.
 */
static THD_FUNCTION(cancom_status_internal_thread, arg) {
	(void)arg;
	chRegSetThreadName("CAN stat loc");

	mc_interface_select_motor_thread(2);

	for (;;) {
		send_status1(utils_second_motor_id(), true);
		send_status2(utils_second_motor_id(), true);
		send_status3(utils_second_motor_id(), true);
		send_status4(utils_second_motor_id(), true);
		send_status5(utils_second_motor_id(), true);
		chThdSleepMilliseconds(2);
	}
}
#endif

static THD_FUNCTION(cancom_status_thread, arg) {
	(void)arg;
	chRegSetThreadName("CAN status");

	for(;;) {
		const app_configuration *conf = app_get_configuration();

		if (conf->can_mode == CAN_MODE_VESC) {
			if (conf->send_can_status == CAN_STATUS_1 ||
					conf->send_can_status == CAN_STATUS_1_2 ||
					conf->send_can_status == CAN_STATUS_1_2_3 ||
					conf->send_can_status == CAN_STATUS_1_2_3_4 ||
					conf->send_can_status == CAN_STATUS_1_2_3_4_5) {
				mc_interface_select_motor_thread(1);
				send_status1(conf->controller_id, false);
#ifdef HW_HAS_DUAL_MOTORS
				mc_interface_select_motor_thread(2);
				send_status1(utils_second_motor_id(), false);
#endif
			}

			if (conf->send_can_status == CAN_STATUS_1_2 ||
					conf->send_can_status == CAN_STATUS_1_2_3||
					conf->send_can_status == CAN_STATUS_1_2_3_4 ||
					conf->send_can_status == CAN_STATUS_1_2_3_4_5) {
				mc_interface_select_motor_thread(1);
				send_status2(conf->controller_id, false);
#ifdef HW_HAS_DUAL_MOTORS
				mc_interface_select_motor_thread(2);
				send_status2(utils_second_motor_id(), false);
#endif
			}

			if (conf->send_can_status == CAN_STATUS_1_2_3 ||
					conf->send_can_status == CAN_STATUS_1_2_3_4 ||
					conf->send_can_status == CAN_STATUS_1_2_3_4_5) {
				mc_interface_select_motor_thread(1);
				send_status3(conf->controller_id, false);
#ifdef HW_HAS_DUAL_MOTORS
				mc_interface_select_motor_thread(2);
				send_status3(utils_second_motor_id(), false);
#endif
			}

			if (conf->send_can_status == CAN_STATUS_1_2_3_4 ||
					conf->send_can_status == CAN_STATUS_1_2_3_4_5) {
				mc_interface_select_motor_thread(1);
				send_status4(conf->controller_id, false);
#ifdef HW_HAS_DUAL_MOTORS
				mc_interface_select_motor_thread(2);
				send_status4(utils_second_motor_id(), false);
#endif
			}

			if (conf->send_can_status == CAN_STATUS_1_2_3_4_5) {
				mc_interface_select_motor_thread(1);
				send_status5(conf->controller_id, false);
#ifdef HW_HAS_DUAL_MOTORS
				mc_interface_select_motor_thread(2);
				send_status5(utils_second_motor_id(), false);
#endif
			}
		}

		systime_t sleep_time = CH_CFG_ST_FREQUENCY / conf->send_can_status_rate_hz;
		if (sleep_time == 0) {
			sleep_time = 1;
		}

		chThdSleep(sleep_time);
	}
}

static void send_packet_wrapper(unsigned char *data, unsigned int len) {
	comm_can_send_buffer(rx_buffer_last_id, data, len, 1);
}

static void decode_msg(uint32_t eid, uint8_t *data8, int len, bool is_replaced) {
	int32_t ind = 0;
	unsigned int rxbuf_len;
	unsigned int rxbuf_ind;
	uint8_t crc_low;
	uint8_t crc_high;
	uint8_t commands_send;

	uint8_t id = eid & 0xFF;
	CAN_PACKET_ID cmd = eid >> 8;

	int id1 = app_get_configuration()->controller_id;

#ifdef HW_HAS_DUAL_MOTORS
	int motor_last = mc_interface_get_motor_thread();
	int id2 = utils_second_motor_id();
	if (id == id2) {
		mc_interface_select_motor_thread(2);
	} else {
		mc_interface_select_motor_thread(1);
	}
#else
	int id2 = id1;
#endif

	if (id == 255 || id == id1 || id == id2) {
		switch (cmd) {
		case CAN_PACKET_SET_DUTY:
			ind = 0;
			mc_interface_set_duty(buffer_get_float32(data8, 1e5, &ind));
			timeout_reset();
			break;

		case CAN_PACKET_SET_CURRENT:
			ind = 0;
			mc_interface_set_current(buffer_get_float32(data8, 1e3, &ind));
			timeout_reset();
			break;

		case CAN_PACKET_SET_CURRENT_BRAKE:
			ind = 0;
			mc_interface_set_brake_current(buffer_get_float32(data8, 1e3, &ind));
			timeout_reset();
			break;

		case CAN_PACKET_SET_RPM:
			ind = 0;
			mc_interface_set_pid_speed(buffer_get_float32(data8, 1e0, &ind));
			timeout_reset();
			break;

		case CAN_PACKET_SET_POS:
			ind = 0;
			mc_interface_set_pid_pos(buffer_get_float32(data8, 1e6, &ind));
			timeout_reset();
			break;

		case CAN_PACKET_FILL_RX_BUFFER:
			memcpy(rx_buffer + data8[0], data8 + 1, len - 1);
			break;

		case CAN_PACKET_FILL_RX_BUFFER_LONG:
			rxbuf_ind = (unsigned int)data8[0] << 8;
			rxbuf_ind |= data8[1];
			if (rxbuf_ind < RX_BUFFER_SIZE) {
				memcpy(rx_buffer + rxbuf_ind, data8 + 2, len - 2);
			}
			break;

		case CAN_PACKET_PROCESS_RX_BUFFER:
			ind = 0;
			rx_buffer_last_id = data8[ind++];
			commands_send = data8[ind++];
			rxbuf_len = (unsigned int)data8[ind++] << 8;
			rxbuf_len |= (unsigned int)data8[ind++];

			if (rxbuf_len > RX_BUFFER_SIZE) {
				break;
			}

			crc_high = data8[ind++];
			crc_low = data8[ind++];

			if (crc16(rx_buffer, rxbuf_len)
					== ((unsigned short) crc_high << 8
							| (unsigned short) crc_low)) {

				if (is_replaced) {
					if (rx_buffer[0] == COMM_JUMP_TO_BOOTLOADER ||
							rx_buffer[0] == COMM_ERASE_NEW_APP ||
							rx_buffer[0] == COMM_WRITE_NEW_APP_DATA ||
							rx_buffer[0] == COMM_WRITE_NEW_APP_DATA_LZO ||
							rx_buffer[0] == COMM_ERASE_BOOTLOADER) {
						break;
					}
				}

				switch (commands_send) {
				case 0:
					commands_process_packet(rx_buffer, rxbuf_len, send_packet_wrapper);
					break;
				case 1:
					commands_send_packet(rx_buffer, rxbuf_len);
					break;
				case 2:
					commands_process_packet(rx_buffer, rxbuf_len, 0);
					break;
				default:
					break;
				}
			}
			break;

		case CAN_PACKET_PROCESS_SHORT_BUFFER:
			ind = 0;
			rx_buffer_last_id = data8[ind++];
			commands_send = data8[ind++];

			if (is_replaced) {
				if (data8[ind] == COMM_JUMP_TO_BOOTLOADER ||
						data8[ind] == COMM_ERASE_NEW_APP ||
						data8[ind] == COMM_WRITE_NEW_APP_DATA ||
						data8[ind] == COMM_WRITE_NEW_APP_DATA_LZO ||
						data8[ind] == COMM_ERASE_BOOTLOADER) {
					break;
				}
			}

			switch (commands_send) {
			case 0:
				commands_process_packet(data8 + ind, len - ind, send_packet_wrapper);
				break;
			case 1:
				commands_send_packet(data8 + ind, len - ind);
				break;
			case 2:
				commands_process_packet(data8 + ind, len - ind, 0);
				break;
			default:
				break;
			}
			break;

			case CAN_PACKET_SET_CURRENT_REL:
				ind = 0;
				mc_interface_set_current_rel(buffer_get_float32(data8, 1e5, &ind));
				timeout_reset();
				break;

			case CAN_PACKET_SET_CURRENT_BRAKE_REL:
				ind = 0;
				mc_interface_set_brake_current_rel(buffer_get_float32(data8, 1e5, &ind));
				timeout_reset();
				break;

			case CAN_PACKET_SET_CURRENT_HANDBRAKE:
				ind = 0;
				mc_interface_set_handbrake(buffer_get_float32(data8, 1e3, &ind));
				timeout_reset();
				break;

			case CAN_PACKET_SET_CURRENT_HANDBRAKE_REL:
				ind = 0;
				mc_interface_set_handbrake_rel(buffer_get_float32(data8, 1e5, &ind));
				timeout_reset();
				break;

			case CAN_PACKET_PING: {
				uint8_t buffer[1];
				buffer[0] = app_get_configuration()->controller_id;
				comm_can_transmit_eid(data8[0] |
						((uint32_t)CAN_PACKET_PONG << 8), buffer, 1);
			} break;

			case CAN_PACKET_PONG:
				// data8[0]; // Sender ID
				if (ping_tp) {
					chEvtSignal(ping_tp, 1 << 29);
				}
				break;

			case CAN_PACKET_DETECT_APPLY_ALL_FOC: {
				if (is_replaced) {
					break;
				}

				ind = 1;
				bool activate_status = data8[ind++];
				float max_power_loss = buffer_get_float32(data8, 1e3, &ind);
				int res = conf_general_detect_apply_all_foc(max_power_loss, true, false);
				if (res >= 0 && activate_status) {
					app_configuration *appconf = mempools_alloc_appconf();
					*appconf = *app_get_configuration();

					if (appconf->send_can_status != CAN_STATUS_1_2_3_4) {
						appconf->send_can_status = CAN_STATUS_1_2_3_4;
						conf_general_store_app_configuration(appconf);
						app_set_configuration(appconf);
					}

					mempools_free_appconf(appconf);
				}

				int8_t buffer[1];
				buffer[0] = res;
				comm_can_transmit_eid(data8[0] |
						((uint32_t)CAN_PACKET_DETECT_APPLY_ALL_FOC_RES << 8), (uint8_t*)buffer, 1);
			} break;

			case CAN_PACKET_DETECT_APPLY_ALL_FOC_RES: {
				if (is_replaced) {
					break;
				}

				detect_all_foc_res[detect_all_foc_res_index++] = (int8_t)data8[0];
				detect_all_foc_res_index %= sizeof(detect_all_foc_res);
			} break;

			case CAN_PACKET_CONF_CURRENT_LIMITS:
			case CAN_PACKET_CONF_STORE_CURRENT_LIMITS: {
				ind = 0;
				float min = buffer_get_float32(data8, 1e3, &ind);
				float max = buffer_get_float32(data8, 1e3, &ind);

				mc_configuration *mcconf = mempools_alloc_mcconf();
				*mcconf = *mc_interface_get_configuration();

				if (mcconf->l_current_min != min || mcconf->l_current_max != max) {
					mcconf->l_current_min = min;
					mcconf->l_current_max = max;

					if (cmd == CAN_PACKET_CONF_STORE_CURRENT_LIMITS) {
						conf_general_store_mc_configuration(mcconf,
								mc_interface_get_motor_thread() == 2);
					}

					mc_interface_set_configuration(mcconf);
				}

				mempools_free_mcconf(mcconf);
			} break;

			case CAN_PACKET_CONF_CURRENT_LIMITS_IN:
			case CAN_PACKET_CONF_STORE_CURRENT_LIMITS_IN: {
				ind = 0;
				float min = buffer_get_float32(data8, 1e3, &ind);
				float max = buffer_get_float32(data8, 1e3, &ind);

				mc_configuration *mcconf = mempools_alloc_mcconf();
				*mcconf = *mc_interface_get_configuration();

				if (mcconf->l_in_current_min != min || mcconf->l_in_current_max != max) {
					mcconf->l_in_current_min = min;
					mcconf->l_in_current_max = max;

					if (cmd == CAN_PACKET_CONF_STORE_CURRENT_LIMITS_IN) {
						conf_general_store_mc_configuration(mcconf,
								mc_interface_get_motor_thread() == 2);
					}

					mc_interface_set_configuration(mcconf);
				}

				mempools_free_mcconf(mcconf);
			} break;

			case CAN_PACKET_CONF_FOC_ERPMS:
			case CAN_PACKET_CONF_STORE_FOC_ERPMS: {
				ind = 0;
				float foc_openloop_rpm = buffer_get_float32(data8, 1e3, &ind);
				float foc_sl_erpm = buffer_get_float32(data8, 1e3, &ind);

				mc_configuration *mcconf = mempools_alloc_mcconf();
				*mcconf = *mc_interface_get_configuration();

				if (mcconf->foc_openloop_rpm != foc_openloop_rpm ||
						mcconf->foc_sl_erpm != foc_sl_erpm) {
					mcconf->foc_openloop_rpm = foc_openloop_rpm;
					mcconf->foc_sl_erpm = foc_sl_erpm;

					if (cmd == CAN_PACKET_CONF_STORE_FOC_ERPMS) {
						conf_general_store_mc_configuration(mcconf,
								mc_interface_get_motor_thread() == 2);
					}

					mc_interface_set_configuration(mcconf);
				}

				mempools_free_mcconf(mcconf);
			} break;

			case CAN_PACKET_POLL_TS5700N8501_STATUS: {
				comm_can_transmit_eid(app_get_configuration()->controller_id |
						((uint32_t)CAN_PACKET_POLL_TS5700N8501_STATUS << 8),
						encoder_ts5700n8501_get_raw_status(), 8);
			} break;

			case CAN_PACKET_CONF_BATTERY_CUT:
			case CAN_PACKET_CONF_STORE_BATTERY_CUT: {
				ind = 0;
				float start = buffer_get_float32(data8, 1e3, &ind);
				float end = buffer_get_float32(data8, 1e3, &ind);

				mc_configuration *mcconf = mempools_alloc_mcconf();
				*mcconf = *mc_interface_get_configuration();

				if (mcconf->l_battery_cut_start != start || mcconf->l_battery_cut_end != end) {
					mcconf->l_battery_cut_start = start;
					mcconf->l_battery_cut_end = end;

					if (cmd == CAN_PACKET_CONF_STORE_BATTERY_CUT) {
						conf_general_store_mc_configuration(mcconf,
								mc_interface_get_motor_thread() == 2);
					}

					mc_interface_set_configuration(mcconf);
				}

				mempools_free_mcconf(mcconf);
			} break;

			case CAN_PACKET_SHUTDOWN: {
#ifdef HW_SHUTDOWN_HOLD_ON
				SHUTDOWN_SET_SAMPLING_DISABLED(true);
				mc_interface_lock();
				DISABLE_GATE();
				HW_SHUTDOWN_HOLD_OFF();
				chThdSleepMilliseconds(5000);
				HW_SHUTDOWN_HOLD_ON();
				ENABLE_GATE();
				mc_interface_unlock();
				SHUTDOWN_SET_SAMPLING_DISABLED(false);
#endif
			} break;

			default:
				break;
		}
	}

	switch (cmd) {
	case CAN_PACKET_STATUS:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg *stat_tmp = &stat_msgs[i];
			if (stat_tmp->id == id || stat_tmp->id == -1) {
				ind = 0;
				stat_tmp->id = id;
				stat_tmp->rx_time = chVTGetSystemTime();
				stat_tmp->rpm = (float)buffer_get_int32(data8, &ind);
				stat_tmp->current = (float)buffer_get_int16(data8, &ind) / 10.0;
				stat_tmp->duty = (float)buffer_get_int16(data8, &ind) / 1000.0;
				break;
			}
		}
		break;

	case CAN_PACKET_STATUS_2:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg_2 *stat_tmp_2 = &stat_msgs_2[i];
			if (stat_tmp_2->id == id || stat_tmp_2->id == -1) {
				ind = 0;
				stat_tmp_2->id = id;
				stat_tmp_2->rx_time = chVTGetSystemTime();
				stat_tmp_2->amp_hours = (float)buffer_get_int32(data8, &ind) / 1e4;
				stat_tmp_2->amp_hours_charged = (float)buffer_get_int32(data8, &ind) / 1e4;
				break;
			}
		}
		break;

	case CAN_PACKET_STATUS_3:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg_3 *stat_tmp_3 = &stat_msgs_3[i];
			if (stat_tmp_3->id == id || stat_tmp_3->id == -1) {
				ind = 0;
				stat_tmp_3->id = id;
				stat_tmp_3->rx_time = chVTGetSystemTime();
				stat_tmp_3->watt_hours = (float)buffer_get_int32(data8, &ind) / 1e4;
				stat_tmp_3->watt_hours_charged = (float)buffer_get_int32(data8, &ind) / 1e4;
				break;
			}
		}
		break;

	case CAN_PACKET_STATUS_4:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg_4 *stat_tmp_4 = &stat_msgs_4[i];
			if (stat_tmp_4->id == id || stat_tmp_4->id == -1) {
				ind = 0;
				stat_tmp_4->id = id;
				stat_tmp_4->rx_time = chVTGetSystemTime();
				stat_tmp_4->temp_fet = (float)buffer_get_int16(data8, &ind) / 10.0;
				stat_tmp_4->temp_motor = (float)buffer_get_int16(data8, &ind) / 10.0;
				stat_tmp_4->current_in = (float)buffer_get_int16(data8, &ind) / 10.0;
				stat_tmp_4->pid_pos_now = (float)buffer_get_int16(data8, &ind) / 50.0;
				break;
			}
		}
		break;

	case CAN_PACKET_STATUS_5:
		for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
			can_status_msg_5 *stat_tmp_5 = &stat_msgs_5[i];
			if (stat_tmp_5->id == id || stat_tmp_5->id == -1) {
				ind = 0;
				stat_tmp_5->id = id;
				stat_tmp_5->rx_time = chVTGetSystemTime();
				stat_tmp_5->tacho_value = buffer_get_int32(data8, &ind);
				stat_tmp_5->v_in = (float)buffer_get_int16(data8, &ind) / 1e1;
				break;
			}
		}
		break;

	default:
		break;
	}

#ifdef HW_HAS_DUAL_MOTORS
	mc_interface_select_motor_thread(motor_last);
#endif
}

static void send_status1(uint8_t id, bool replace) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	buffer_append_int32(buffer, (int32_t)mc_interface_get_rpm(), &send_index);
	buffer_append_int16(buffer, (int16_t)(mc_interface_get_tot_current_filtered() * 1e1), &send_index);
	buffer_append_int16(buffer, (int16_t)(mc_interface_get_duty_cycle_now() * 1e3), &send_index);
	comm_can_transmit_eid_replace(id | ((uint32_t)CAN_PACKET_STATUS << 8),
			buffer, send_index, replace);
}

static void send_status2(uint8_t id, bool replace) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	buffer_append_int32(buffer, (int32_t)(mc_interface_get_amp_hours(false) * 1e4), &send_index);
	buffer_append_int32(buffer, (int32_t)(mc_interface_get_amp_hours_charged(false) * 1e4), &send_index);
	comm_can_transmit_eid_replace(id | ((uint32_t)CAN_PACKET_STATUS_2 << 8),
			buffer, send_index, replace);
}

static void send_status3(uint8_t id, bool replace) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	buffer_append_int32(buffer, (int32_t)(mc_interface_get_watt_hours(false) * 1e4), &send_index);
	buffer_append_int32(buffer, (int32_t)(mc_interface_get_watt_hours_charged(false) * 1e4), &send_index);
	comm_can_transmit_eid_replace(id | ((uint32_t)CAN_PACKET_STATUS_3 << 8),
			buffer, send_index, replace);
}

static void send_status4(uint8_t id, bool replace) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	buffer_append_int16(buffer, (int16_t)(mc_interface_temp_fet_filtered() * 1e1), &send_index);
	buffer_append_int16(buffer, (int16_t)(mc_interface_temp_motor_filtered() * 1e1), &send_index);
	buffer_append_int16(buffer, (int16_t)(mc_interface_get_tot_current_in_filtered() * 1e1), &send_index);
	buffer_append_int16(buffer, (int16_t)(mc_interface_get_pid_pos_now() * 50.0), &send_index);
	comm_can_transmit_eid_replace(id | ((uint32_t)CAN_PACKET_STATUS_4 << 8),
			buffer, send_index, replace);
}

static void send_status5(uint8_t id, bool replace) {
	int32_t send_index = 0;
	uint8_t buffer[8];
	buffer_append_int32(buffer, mc_interface_get_tachometer_value(false), &send_index);
	buffer_append_int16(buffer, (int16_t)(GET_INPUT_VOLTAGE() * 1e1), &send_index);
	buffer_append_int16(buffer, 0, &send_index); // Reserved for now
	comm_can_transmit_eid_replace(id | ((uint32_t)CAN_PACKET_STATUS_5 << 8),
			buffer, send_index, replace);
}
#endif

/**
 * Set the CAN timing. The CAN is clocked at 42 MHz, and the baud rate can be
 * calculated with
 *
 * 42000000 / ((brp + 1) * (ts1 + ts2 + 3))
 *
 * ts1 should be larger than ts2 in general to take the sample after the
 * signal had time to stabilize.
 *
 * @param brp
 * Prescaler.
 *
 * @param ts1
 * TS1.
 *
 * @param ts2
 * TS2.
 */
static void set_timing(int brp, int ts1, int ts2) {
	brp &= 0b1111111111;
	ts1 &= 0b1111;
	ts2 &= 0b111;

	cancfg.btr = CAN_BTR_SJW(3) | CAN_BTR_TS2(ts2) |
		CAN_BTR_TS1(ts1) | CAN_BTR_BRP(brp);

	canStop(&HW_CAN_DEV);
	canStart(&HW_CAN_DEV, &cancfg);
}
