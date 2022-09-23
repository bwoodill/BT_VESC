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

#include "flash_helper.h"
#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"
#include "utils.h"
#include "mc_interface.h"
#include "timeout.h"
#include "hw.h"
#include "crc.h"
#include <string.h>

/*
 * Defines
 */
#define FLASH_SECTORS							12
#define BOOTLOADER_BASE							11
#define APP_BASE								0
#define NEW_APP_BASE							8
#define NEW_APP_SECTORS							3
#define APP_MAX_SIZE							(393216 - 8) // Note that the bootloader needs 8 extra bytes

// Base address of the Flash sectors
#define ADDR_FLASH_SECTOR_0    					((uint32_t)0x08000000) // Base @ of Sector 0, 16 Kbytes
#define ADDR_FLASH_SECTOR_1    					((uint32_t)0x08004000) // Base @ of Sector 1, 16 Kbytes
#define ADDR_FLASH_SECTOR_2    					((uint32_t)0x08008000) // Base @ of Sector 2, 16 Kbytes
#define ADDR_FLASH_SECTOR_3						((uint32_t)0x0800C000) // Base @ of Sector 3, 16 Kbytes
#define ADDR_FLASH_SECTOR_4    					((uint32_t)0x08010000) // Base @ of Sector 4, 64 Kbytes
#define ADDR_FLASH_SECTOR_5    					((uint32_t)0x08020000) // Base @ of Sector 5, 128 Kbytes
#define ADDR_FLASH_SECTOR_6     				((uint32_t)0x08040000) // Base @ of Sector 6, 128 Kbytes
#define ADDR_FLASH_SECTOR_7     				((uint32_t)0x08060000) // Base @ of Sector 7, 128 Kbytes
#define ADDR_FLASH_SECTOR_8     				((uint32_t)0x08080000) // Base @ of Sector 8, 128 Kbytes
#define ADDR_FLASH_SECTOR_9 				    ((uint32_t)0x080A0000) // Base @ of Sector 9, 128 Kbytes
#define ADDR_FLASH_SECTOR_10				    ((uint32_t)0x080C0000) // Base @ of Sector 10, 128 Kbytes
#define ADDR_FLASH_SECTOR_11				    ((uint32_t)0x080E0000) // Base @ of Sector 11, 128 Kbytes

#define VECTOR_TABLE_ADDRESS					((uint32_t*)ADDR_FLASH_SECTOR_0)
#define VECTOR_TABLE_SIZE						((uint32_t)(ADDR_FLASH_SECTOR_1 - ADDR_FLASH_SECTOR_0))
#define EEPROM_EMULATION_SIZE					((uint32_t)(ADDR_FLASH_SECTOR_4 - ADDR_FLASH_SECTOR_2))

#define APP_START_ADDRESS						((uint32_t*)(ADDR_FLASH_SECTOR_3))
#define APP_SIZE								((uint32_t)(APP_MAX_SIZE - VECTOR_TABLE_SIZE - EEPROM_EMULATION_SIZE))

#define	APP_CRC_WAS_CALCULATED_FLAG				((uint32_t)0x00000000)
#define	APP_CRC_WAS_CALCULATED_FLAG_ADDRESS		((uint32_t*)(ADDR_FLASH_SECTOR_0 + APP_MAX_SIZE - 8))
#define APP_CRC_ADDRESS							((uint32_t*)(ADDR_FLASH_SECTOR_0 + APP_MAX_SIZE - 4))

typedef struct {
	uint32_t crc_flag;
	uint32_t crc;
}crc_info_t;

//Make sure the app image has the CRC bits set to '1' to later write the flag and CRC.
const crc_info_t __attribute__((section (".crcinfo"))) crc_info = {0xFFFFFFFF, 0xFFFFFFFF};

// Private constants
static const uint32_t flash_addr[FLASH_SECTORS] = {
		ADDR_FLASH_SECTOR_0,
		ADDR_FLASH_SECTOR_1,
		ADDR_FLASH_SECTOR_2,
		ADDR_FLASH_SECTOR_3,
		ADDR_FLASH_SECTOR_4,
		ADDR_FLASH_SECTOR_5,
		ADDR_FLASH_SECTOR_6,
		ADDR_FLASH_SECTOR_7,
		ADDR_FLASH_SECTOR_8,
		ADDR_FLASH_SECTOR_9,
		ADDR_FLASH_SECTOR_10,
		ADDR_FLASH_SECTOR_11
};
static const uint16_t flash_sector[FLASH_SECTORS] = {
		FLASH_Sector_0,
		FLASH_Sector_1,
		FLASH_Sector_2,
		FLASH_Sector_3,
		FLASH_Sector_4,
		FLASH_Sector_5,
		FLASH_Sector_6,
		FLASH_Sector_7,
		FLASH_Sector_8,
		FLASH_Sector_9,
		FLASH_Sector_10,
		FLASH_Sector_11
};

uint16_t flash_helper_erase_new_app(uint32_t new_app_size) {
	FLASH_Unlock();
	FLASH_ClearFlag(FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
			FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

	new_app_size += flash_addr[NEW_APP_BASE];

	mc_interface_unlock();
	mc_interface_release_motor();
	utils_sys_lock_cnt();
	timeout_configure_IWDT_slowest();

	for (int i = 0;i < NEW_APP_SECTORS;i++) {
		if (new_app_size > flash_addr[NEW_APP_BASE + i]) {
			uint16_t res = FLASH_EraseSector(flash_sector[NEW_APP_BASE + i], VoltageRange_3);
			if (res != FLASH_COMPLETE) {
				FLASH_Lock();
				timeout_configure_IWDT();
				utils_sys_unlock_cnt();
				return res;
			}
		} else {
			break;
		}
	}

	FLASH_Lock();
	timeout_configure_IWDT();
	utils_sys_unlock_cnt();

	return FLASH_COMPLETE;
}

uint16_t flash_helper_erase_bootloader(void) {
	FLASH_Unlock();
	FLASH_ClearFlag(FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
			FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

	mc_interface_unlock();
	mc_interface_release_motor();
	utils_sys_lock_cnt();
	timeout_configure_IWDT_slowest();

	uint16_t res = FLASH_EraseSector(flash_sector[BOOTLOADER_BASE], VoltageRange_3);
	if (res != FLASH_COMPLETE) {
		FLASH_Lock();
		return res;
	}

	FLASH_Lock();
	timeout_configure_IWDT();
	utils_sys_unlock_cnt();

	return FLASH_COMPLETE;
}

uint16_t flash_helper_write_new_app_data(uint32_t offset, uint8_t *data, uint32_t len) {
	FLASH_Unlock();
	FLASH_ClearFlag(FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
			FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

	mc_interface_unlock();
	mc_interface_release_motor();
	utils_sys_lock_cnt();
	timeout_configure_IWDT_slowest();

	for (uint32_t i = 0;i < len;i++) {
		uint16_t res = FLASH_ProgramByte(flash_addr[NEW_APP_BASE] + offset + i, data[i]);
		if (res != FLASH_COMPLETE) {
			FLASH_Lock();
			return res;
		}
	}
	FLASH_Lock();

	timeout_configure_IWDT();

	utils_sys_unlock_cnt();

	return FLASH_COMPLETE;
}

/**
 * Stop the system and jump to the bootloader.
 */
void flash_helper_jump_to_bootloader(void) {
	typedef void (*pFunction)(void);

	mc_interface_unlock();
	mc_interface_release_motor();
	usbDisconnectBus(&USBD1);
	usbStop(&USBD1);

	sdStop(&HW_UART_DEV);
	palSetPadMode(HW_UART_TX_PORT, HW_UART_TX_PIN, PAL_MODE_INPUT);
	palSetPadMode(HW_UART_RX_PORT, HW_UART_RX_PIN, PAL_MODE_INPUT);

	// Disable watchdog
	timeout_configure_IWDT_slowest();

	chSysDisable();

	pFunction jump_to_bootloader;

	// Variable that will be loaded with the start address of the application
	volatile uint32_t* jump_address;
	const volatile uint32_t* bootloader_address = (volatile uint32_t*)0x080E0000;

	// Get jump address from application vector table
	jump_address = (volatile uint32_t*) bootloader_address[1];

	// Load this address into function pointer
	jump_to_bootloader = (pFunction) jump_address;

	// Clear pending interrupts
	SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk;

	// Disable all interrupts
	for(int i = 0;i < 8;i++) {
		NVIC->ICER[i] = NVIC->IABR[i];
	}

	// Set stack pointer
	__set_MSP((uint32_t) (bootloader_address[0]));

	// Jump to the bootloader
	jump_to_bootloader();
}

uint8_t* flash_helper_get_sector_address(uint32_t fsector) {
	uint8_t *res = 0;

	for (int i = 0;i < FLASH_SECTORS;i++) {
		if (flash_sector[i] == fsector) {
			res = (uint8_t *)flash_addr[i];
			break;
		}
	}

	return res;
}

/**
  * @brief  Compute the CRC of the application code to verify its integrity
  * @retval FAULT_CODE_NONE or FAULT_CODE_FLASH_CORRUPTION
  */
uint32_t flash_helper_verify_flash_memory(void) {
	uint32_t crc;
	// Look for a flag indicating that the CRC was previously computed.
	// If it is blank (0xFFFFFFFF), calculate and store the CRC.
	if(APP_CRC_WAS_CALCULATED_FLAG_ADDRESS[0] == APP_CRC_WAS_CALCULATED_FLAG) {
		RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_CRC, ENABLE);
		crc32_reset();

		// compute vector table (sector 0)
		crc32(VECTOR_TABLE_ADDRESS, (VECTOR_TABLE_SIZE) / 4);

		// skip emulated EEPROM (sector 1 and 2)

		// compute application code
		crc = crc32(APP_START_ADDRESS, (APP_SIZE) / 4);

		RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_CRC, DISABLE);

		// A CRC over the full image should return zero.
		return (crc == 0) ? FAULT_CODE_NONE : FAULT_CODE_FLASH_CORRUPTION;
	} else {
		FLASH_Unlock();
		FLASH_ClearFlag(FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
				FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

		// Write the flag to indicate CRC has been computed.
		uint16_t res = FLASH_ProgramWord((uint32_t)APP_CRC_WAS_CALCULATED_FLAG_ADDRESS, APP_CRC_WAS_CALCULATED_FLAG);
		if (res != FLASH_COMPLETE) {
			FLASH_Lock();
			return FAULT_CODE_FLASH_CORRUPTION;
		}

		// Compute flash crc including the new flag
		RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_CRC, ENABLE);
		crc32_reset();

		// compute vector table (sector 0)
		crc32(VECTOR_TABLE_ADDRESS, (VECTOR_TABLE_SIZE) / 4);

		// skip emulated EEPROM (sector 1 and 2)

		// compute application code
		crc = crc32(APP_START_ADDRESS, (APP_SIZE - 4) / 4);

		RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_CRC, DISABLE);

		//Store CRC
		res = FLASH_ProgramWord((uint32_t)APP_CRC_ADDRESS, crc);
		if (res != FLASH_COMPLETE) {
			FLASH_Lock();
			return FAULT_CODE_FLASH_CORRUPTION;
		}
		FLASH_Lock();

		// reboot
		NVIC_SystemReset();
		return FAULT_CODE_NONE;
	}
}

uint32_t flash_helper_verify_flash_memory_chunk(void) {
	static uint32_t index = 0;
	uint32_t chunk_size = 1024;
	uint32_t res = FAULT_CODE_NONE;
	uint32_t crc = 0;
	uint32_t tot_bytes = VECTOR_TABLE_SIZE + APP_SIZE;

	// Make sure RCC_AHB1Periph_CRC is enabled
	if (index == 0) {
		crc32_reset();
	}

	if ((index + chunk_size) >= tot_bytes) {
		chunk_size = tot_bytes - index;
	}

	if (index < VECTOR_TABLE_SIZE) {
		crc32(VECTOR_TABLE_ADDRESS + index / 4, chunk_size / 4);
	} else {
		crc = crc32(APP_START_ADDRESS + (index - VECTOR_TABLE_SIZE) / 4, chunk_size / 4);
	}

	index += chunk_size;
	if (index >= tot_bytes) {
		index = 0;
		if (crc != 0) {
			res = FAULT_CODE_FLASH_CORRUPTION;
		}
	}

	return res;
}
