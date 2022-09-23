/*
	Copyright 2017 Benjamin Vedder	benjamin@vedder.se
	Copyright 2019 Marcos Chaparro	mchaparro@powerdesigns.ca

	For support, please contact www.powerdesigns.ca

	This file is part of the VESC firmware.

	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */

#include "hw.h"
#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"
#include "stm32f4xx_rcc.h"
#include "utils.h"
#include "terminal.h"
#include "commands.h"
#include "mc_interface.h"
#include "stdio.h"
#include <math.h>
#include "minilzo.h"

#include "hw_axiom_fpga_bitstream.c"    //this file ONLY contains the fpga binary blob

// Defines
#define SPI_SW_MISO_GPIO			HW_SPI_PORT_MISO
#define SPI_SW_MISO_PIN				HW_SPI_PIN_MISO
#define SPI_SW_MOSI_GPIO			HW_SPI_PORT_MOSI
#define SPI_SW_MOSI_PIN				HW_SPI_PIN_MOSI
#define SPI_SW_SCK_GPIO				HW_SPI_PORT_SCK
#define SPI_SW_SCK_PIN				HW_SPI_PIN_SCK
#define SPI_SW_FPGA_CS_GPIO			GPIOB
#define SPI_SW_FPGA_CS_PIN			7

#define AXIOM_FPGA_CLK_PORT			GPIOC
#define AXIOM_FPGA_CLK_PIN			9
#define AXIOM_FPGA_RESET_PORT		GPIOB

#ifdef HW_PALTA_REV_B
#define AXIOM_FPGA_RESET_PIN		5
#else
#define AXIOM_FPGA_RESET_PIN		4
#endif

#define EEPROM_ADDR_CURRENT_GAIN	0

#define BITSTREAM_CHUNK_SIZE		2000
#define BITSTREAM_SIZE				104090		//ice40up5k
//#define BITSTREAM_SIZE				71338		//ice40LP1K

// Variables
static volatile bool i2c_running = false;
static volatile float current_sensor_gain = 0.0;
//extern unsigned char FPGA_bitstream[BITSTREAM_SIZE];

// I2C configuration
static const I2CConfig i2cfg = {
		OPMODE_I2C,
		100000,
		STD_DUTY_CYCLE
};

// Private functions
static void terminal_cmd_reset_oc(int argc, const char **argv);
static void terminal_cmd_store_current_sensor_gain(int argc, const char **argv);
static void terminal_cmd_read_current_sensor_gain(int argc, const char **argv);
static void spi_transfer(uint8_t *in_buf, const uint8_t *out_buf, int length);
static void spi_begin(void);
static void spi_end(void);
static void spi_delay(void);
void hw_axiom_init_FPGA_CLK(void);
void hw_axiom_setup_dac(void);
void hw_axiom_configure_brownout(uint8_t);
void hw_axiom_configure_VDD_undervoltage(void);
float hw_axiom_read_current_sensor_gain(void);
inline float hw_axiom_get_current_sensor_gain(void);

void hw_init_gpio(void) {

	// Set Brown out to keep mcu under reset until VDD reaches 2.7V
	hw_axiom_configure_brownout(OB_BOR_LEVEL3);

	// Configure Programmable voltage detector to interrupt the cpu
	// when VDD is below 2.9V.
	hw_axiom_configure_VDD_undervoltage();

	// GPIO clock enable
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);

	// LEDs
	palSetPadMode(GPIOB, 2, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);

#ifdef HW_PALTA_REV_B
	palSetPadMode(GPIOB, 1, PAL_MODE_OUTPUT_PUSHPULL |	PAL_STM32_OSPEED_HIGHEST);
#else
	palSetPadMode(GPIOB, 11, PAL_MODE_OUTPUT_PUSHPULL |	PAL_STM32_OSPEED_HIGHEST);
#endif
	// ENABLE_GATE
	palSetPadMode(GPIOC, 14,
			PAL_MODE_OUTPUT_PUSHPULL |
			PAL_STM32_OSPEED_HIGHEST);

	ENABLE_GATE();

	// FPGA SPI port
	palSetPadMode(SPI_SW_MISO_GPIO, SPI_SW_MISO_PIN, PAL_MODE_INPUT);
	palSetPadMode(SPI_SW_SCK_GPIO, SPI_SW_SCK_PIN, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
	palSetPadMode(SPI_SW_FPGA_CS_GPIO, SPI_SW_FPGA_CS_PIN, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
	palSetPadMode(SPI_SW_MOSI_GPIO, SPI_SW_MOSI_PIN, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);

	// Set FPGA SS to '0' to make it start in slave mode
	palClearPad(SPI_SW_FPGA_CS_GPIO, SPI_SW_FPGA_CS_PIN);

	// FPGA RESET
	palSetPadMode(AXIOM_FPGA_RESET_PORT, AXIOM_FPGA_RESET_PIN, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
	palClearPad(AXIOM_FPGA_RESET_PORT, AXIOM_FPGA_RESET_PIN);
	chThdSleep(1);
	palSetPad(AXIOM_FPGA_RESET_PORT, AXIOM_FPGA_RESET_PIN);
    
	//output a 12MHz clock on MCO2
	hw_axiom_init_FPGA_CLK();

	// GPIOA Configuration: Channel 1 to 3 as alternate function push-pull
	palSetPadMode(GPIOA, 8, PAL_MODE_ALTERNATE(GPIO_AF_TIM1) |
			PAL_STM32_OSPEED_HIGHEST |
			PAL_STM32_PUDR_FLOATING);
	palSetPadMode(GPIOA, 9, PAL_MODE_ALTERNATE(GPIO_AF_TIM1) |
			PAL_STM32_OSPEED_HIGHEST |
			PAL_STM32_PUDR_FLOATING);
	palSetPadMode(GPIOA, 10, PAL_MODE_ALTERNATE(GPIO_AF_TIM1) |
			PAL_STM32_OSPEED_HIGHEST |
			PAL_STM32_PUDR_FLOATING);

	palSetPadMode(GPIOB, 13, PAL_MODE_ALTERNATE(GPIO_AF_TIM1) |
			PAL_STM32_OSPEED_HIGHEST |
			PAL_STM32_PUDR_FLOATING);
	palSetPadMode(GPIOB, 14, PAL_MODE_ALTERNATE(GPIO_AF_TIM1) |
			PAL_STM32_OSPEED_HIGHEST |
			PAL_STM32_PUDR_FLOATING);
	palSetPadMode(GPIOB, 15, PAL_MODE_ALTERNATE(GPIO_AF_TIM1) |
			PAL_STM32_OSPEED_HIGHEST |
			PAL_STM32_PUDR_FLOATING);

	// Hall sensors
	palSetPadMode(HW_HALL_ENC_GPIO1, HW_HALL_ENC_PIN1, PAL_MODE_INPUT_PULLUP);
	palSetPadMode(HW_HALL_ENC_GPIO2, HW_HALL_ENC_PIN2, PAL_MODE_INPUT_PULLUP);
	palSetPadMode(HW_HALL_ENC_GPIO3, HW_HALL_ENC_PIN3, PAL_MODE_INPUT_PULLUP);

	// Fault pin
	palSetPadMode(GPIOB, 12, PAL_MODE_INPUT_PULLUP);

	// ADC Pins
	palSetPadMode(GPIOA, 0, PAL_MODE_INPUT_ANALOG);
	palSetPadMode(GPIOA, 1, PAL_MODE_INPUT_ANALOG);
	palSetPadMode(GPIOA, 2, PAL_MODE_INPUT_ANALOG);
	palSetPadMode(GPIOA, 3, PAL_MODE_INPUT_ANALOG);

#ifdef HW_AXIOM_USE_DAC
	hw_axiom_setup_dac();
#else
	palSetPadMode(GPIOA, 4, PAL_MODE_INPUT_ANALOG);		//Temperature bridge A
	palSetPadMode(GPIOA, 6, PAL_MODE_INPUT_ANALOG);		//Temperature bridge B
#endif
	palSetPadMode(GPIOA, 5, PAL_MODE_INPUT_ANALOG);		//Temperature bridge C

	palSetPadMode(GPIOB, 0, PAL_MODE_INPUT_ANALOG);		//Accel 2
#ifndef HW_PALTA_REV_B
	palSetPadMode(GPIOB, 1, PAL_MODE_INPUT_ANALOG);		//Gate driver supply voltage
#endif

	palSetPadMode(GPIOC, 0, PAL_MODE_INPUT_ANALOG);
	palSetPadMode(GPIOC, 1, PAL_MODE_INPUT_ANALOG);
	palSetPadMode(GPIOC, 2, PAL_MODE_INPUT_ANALOG);
	palSetPadMode(GPIOC, 3, PAL_MODE_INPUT_ANALOG);
	palSetPadMode(GPIOC, 4, PAL_MODE_INPUT_ANALOG);		//Motor temp
	palSetPadMode(GPIOC, 5, PAL_MODE_INPUT_ANALOG);		//Accel 1

	// Register terminal callbacks
	terminal_register_command_callback(
			"axiom_clear_faults",
			"Reset latched FPGA faults.",
			0,
			terminal_cmd_reset_oc);

	terminal_register_command_callback(
			"axiom_store_current_sensor_gain",
			"Store new current sensor gain.",
			0,
			terminal_cmd_store_current_sensor_gain);

	terminal_register_command_callback(
			"axiom_read_current_sensor_gain",
			"Read current sensor gain.",
			0,
			terminal_cmd_read_current_sensor_gain);
    
    // Send bitstream over SPI to configure FPGA
	hw_axiom_configure_FPGA();

	current_sensor_gain = hw_axiom_read_current_sensor_gain();
}

void hw_setup_adc_channels(void) {
	// ADC1 regular channels
	ADC_RegularChannelConfig(ADC1, ADC_Channel_0,  1, ADC_SampleTime_15Cycles);	// 0	SENS1
	ADC_RegularChannelConfig(ADC1, ADC_Channel_10, 2, ADC_SampleTime_15Cycles);	// 3	CURR1
	ADC_RegularChannelConfig(ADC1, ADC_Channel_8,  3, ADC_SampleTime_15Cycles); // 6	Throttle2
	ADC_RegularChannelConfig(ADC1, ADC_Channel_14, 4, ADC_SampleTime_15Cycles); // 9	TEMP_MOTOR
	ADC_RegularChannelConfig(ADC1, ADC_Channel_9,  5, ADC_SampleTime_15Cycles);	// 12	V_GATE_DRIVER
	ADC_RegularChannelConfig(ADC1, ADC_Channel_5,  6, ADC_SampleTime_15Cycles);	// 15	IGBT_TEMP3

	// ADC2 regular channels
	ADC_RegularChannelConfig(ADC2, ADC_Channel_1,  1, ADC_SampleTime_15Cycles);	// 1	SENS2
	ADC_RegularChannelConfig(ADC2, ADC_Channel_11, 2, ADC_SampleTime_15Cycles);	// 4	CURR2
	ADC_RegularChannelConfig(ADC2, ADC_Channel_6,  3, ADC_SampleTime_15Cycles);	// 7	IGBT_TEMP2
	ADC_RegularChannelConfig(ADC2, ADC_Channel_15, 4, ADC_SampleTime_15Cycles);	// 10	Throttle1
	ADC_RegularChannelConfig(ADC2, ADC_Channel_4,  5, ADC_SampleTime_15Cycles);	// 13	IGBT_TEMP1
	ADC_RegularChannelConfig(ADC2, ADC_Channel_Vrefint,  6, ADC_SampleTime_15Cycles);// 16	VREFINT

	// ADC3 regular channels
	ADC_RegularChannelConfig(ADC3, ADC_Channel_2,  1, ADC_SampleTime_15Cycles);	// 2	SENS3
	ADC_RegularChannelConfig(ADC3, ADC_Channel_12, 2, ADC_SampleTime_15Cycles);	// 5	CURR3
	ADC_RegularChannelConfig(ADC3, ADC_Channel_3,  3, ADC_SampleTime_15Cycles); // 8	PCB_TEMP
	ADC_RegularChannelConfig(ADC3, ADC_Channel_13, 4, ADC_SampleTime_15Cycles);	// 11	VBUS
	ADC_RegularChannelConfig(ADC3, ADC_Channel_3,  5, ADC_SampleTime_15Cycles);	// 14	UNUSED
	ADC_RegularChannelConfig(ADC3, ADC_Channel_Vrefint,  6, ADC_SampleTime_15Cycles);// 17

	// Injected channels
	ADC_InjectedChannelConfig(ADC1, ADC_Channel_10, 1, ADC_SampleTime_15Cycles);
	ADC_InjectedChannelConfig(ADC2, ADC_Channel_11, 1, ADC_SampleTime_15Cycles);
	ADC_InjectedChannelConfig(ADC3, ADC_Channel_12, 1, ADC_SampleTime_15Cycles);
	ADC_InjectedChannelConfig(ADC1, ADC_Channel_10, 2, ADC_SampleTime_15Cycles);
	ADC_InjectedChannelConfig(ADC2, ADC_Channel_11, 2, ADC_SampleTime_15Cycles);
	ADC_InjectedChannelConfig(ADC3, ADC_Channel_12, 2, ADC_SampleTime_15Cycles);
	ADC_InjectedChannelConfig(ADC1, ADC_Channel_10, 3, ADC_SampleTime_15Cycles);
	ADC_InjectedChannelConfig(ADC2, ADC_Channel_11, 3, ADC_SampleTime_15Cycles);
	ADC_InjectedChannelConfig(ADC3, ADC_Channel_12, 3, ADC_SampleTime_15Cycles);
}

void hw_axiom_setup_dac(void) {
	// GPIOA clock enable
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);

	// DAC Periph clock enable
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_DAC, ENABLE);

	// DAC channel 1 & 2 (DAC_OUT1 = PA.4)(DAC_OUT2 = PA.5) configuration
	palSetPadMode(GPIOA, 4, PAL_MODE_INPUT_ANALOG);
	palSetPadMode(GPIOA, 5, PAL_MODE_INPUT_ANALOG);

	// Enable both DAC channels with output buffer disabled to achieve rail-to-rail output
	DAC->CR |= DAC_CR_EN1 | DAC_CR_BOFF1 | DAC_CR_EN2 | DAC_CR_BOFF2;

	// Set DAC channels at 1.65V
	hw_axiom_DAC1_setdata(0x800);
	hw_axiom_DAC2_setdata(0x800);
}

void hw_axiom_DAC1_setdata(uint16_t data) {
	DAC->DHR12R1 = data;
}

void hw_axiom_DAC2_setdata(uint16_t data) {
	DAC->DHR12R2 = data;
}

void hw_axiom_configure_brownout(uint8_t BOR_level) {
    /* Get BOR Option Bytes */
    if((FLASH_OB_GetBOR() & 0x0C) != BOR_level)
    {
      /* Unlocks the option bytes block access */
      FLASH_OB_Unlock();

      /* Select the desired V(BOR) Level -------------------------------------*/
      FLASH_OB_BORConfig(BOR_level);

      /* Launch the option byte loading */
      FLASH_OB_Launch();

      /* Locks the option bytes block access */
      FLASH_OB_Lock();
    }
}

void hw_axiom_configure_VDD_undervoltage(void) {

	// partially configured in mcuconf.h -> STM32_PVD_ENABLE and STM32_PLS

	// Connect EXTI Line to pin
	EXTI_InitTypeDef   EXTI_InitStructure;

	// Configure EXTI Line
	EXTI_InitStructure.EXTI_Line = EXTI_Line16;		//Connected to Programmable Voltage Detector
	EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
	EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising;
	EXTI_InitStructure.EXTI_LineCmd = ENABLE;
	EXTI_Init(&EXTI_InitStructure);

	// Enable and set EXTI Line Interrupt to the highest priority
	nvicEnableVector(PVD_IRQn, 0);
}

void hw_start_i2c(void) {
	i2cAcquireBus(&HW_I2C_DEV);

	if (!i2c_running) {
		palSetPadMode(HW_I2C_SCL_PORT, HW_I2C_SCL_PIN,
				PAL_MODE_ALTERNATE(HW_I2C_GPIO_AF) |
				PAL_STM32_OTYPE_OPENDRAIN |
				PAL_STM32_OSPEED_MID1 |
				PAL_STM32_PUDR_PULLUP);
		palSetPadMode(HW_I2C_SDA_PORT, HW_I2C_SDA_PIN,
				PAL_MODE_ALTERNATE(HW_I2C_GPIO_AF) |
				PAL_STM32_OTYPE_OPENDRAIN |
				PAL_STM32_OSPEED_MID1 |
				PAL_STM32_PUDR_PULLUP);

		i2cStart(&HW_I2C_DEV, &i2cfg);
		i2c_running = true;
	}

	i2cReleaseBus(&HW_I2C_DEV);
}

void hw_stop_i2c(void) {
	i2cAcquireBus(&HW_I2C_DEV);

	if (i2c_running) {
		palSetPadMode(HW_I2C_SCL_PORT, HW_I2C_SCL_PIN, PAL_MODE_INPUT);
		palSetPadMode(HW_I2C_SDA_PORT, HW_I2C_SDA_PIN, PAL_MODE_INPUT);

		i2cStop(&HW_I2C_DEV);
		i2c_running = false;

	}

	i2cReleaseBus(&HW_I2C_DEV);
}

/**
 * Try to restore the i2c bus
 */
void hw_try_restore_i2c(void) {
	if (i2c_running) {
		i2cAcquireBus(&HW_I2C_DEV);

		palSetPadMode(HW_I2C_SCL_PORT, HW_I2C_SCL_PIN,
				PAL_STM32_OTYPE_OPENDRAIN |
				PAL_STM32_OSPEED_MID1 |
				PAL_STM32_PUDR_PULLUP);

		palSetPadMode(HW_I2C_SDA_PORT, HW_I2C_SDA_PIN,
				PAL_STM32_OTYPE_OPENDRAIN |
				PAL_STM32_OSPEED_MID1 |
				PAL_STM32_PUDR_PULLUP);

		palSetPad(HW_I2C_SCL_PORT, HW_I2C_SCL_PIN);
		palSetPad(HW_I2C_SDA_PORT, HW_I2C_SDA_PIN);

		chThdSleep(1);

		for(int i = 0;i < 16;i++) {
			palClearPad(HW_I2C_SCL_PORT, HW_I2C_SCL_PIN);
			chThdSleep(1);
			palSetPad(HW_I2C_SCL_PORT, HW_I2C_SCL_PIN);
			chThdSleep(1);
		}

		// Generate start then stop condition
		palClearPad(HW_I2C_SDA_PORT, HW_I2C_SDA_PIN);
		chThdSleep(1);
		palClearPad(HW_I2C_SCL_PORT, HW_I2C_SCL_PIN);
		chThdSleep(1);
		palSetPad(HW_I2C_SCL_PORT, HW_I2C_SCL_PIN);
		chThdSleep(1);
		palSetPad(HW_I2C_SDA_PORT, HW_I2C_SDA_PIN);

		palSetPadMode(HW_I2C_SCL_PORT, HW_I2C_SCL_PIN,
				PAL_MODE_ALTERNATE(HW_I2C_GPIO_AF) |
				PAL_STM32_OTYPE_OPENDRAIN |
				PAL_STM32_OSPEED_MID1 |
				PAL_STM32_PUDR_PULLUP);

		palSetPadMode(HW_I2C_SDA_PORT, HW_I2C_SDA_PIN,
				PAL_MODE_ALTERNATE(HW_I2C_GPIO_AF) |
				PAL_STM32_OTYPE_OPENDRAIN |
				PAL_STM32_OSPEED_MID1 |
				PAL_STM32_PUDR_PULLUP);

		HW_I2C_DEV.state = I2C_STOP;
		i2cStart(&HW_I2C_DEV, &i2cfg);

		i2cReleaseBus(&HW_I2C_DEV);
	}
}

static void terminal_cmd_reset_oc(int argc, const char **argv) {
	(void)argc;
	(void)argv;

	hw_axiom_configure_FPGA();
	commands_printf("Axiom FPGA fault latch reset done!");
	commands_printf(" ");
}

// Software SPI for FPGA control
static void spi_transfer(uint8_t *in_buf, const uint8_t *out_buf, int length) {
	for (int i = 0;i < length;i++) {
		uint8_t send = out_buf ? out_buf[i] : 0xFF;
		uint8_t recieve = 0;

		for (int bit = 0;bit < 8;bit++) {
			palWritePad(HW_SPI_PORT_MOSI, HW_SPI_PIN_MOSI, send >> 7);
			send <<= 1;

			spi_delay();
			palSetPad(SPI_SW_SCK_GPIO, SPI_SW_SCK_PIN);
			spi_delay();

/*
			int r1, r2, r3;
			r1 = palReadPad(SPI_SW_MISO_GPIO, SPI_SW_MISO_PIN);
			__NOP();
			r2 = palReadPad(SPI_SW_MISO_GPIO, SPI_SW_MISO_PIN);
			__NOP();
			r3 = palReadPad(SPI_SW_MISO_GPIO, SPI_SW_MISO_PIN);

			recieve <<= 1;
			if (utils_middle_of_3_int(r1, r2, r3)) {
				recieve |= 1;
			}
*/

			palClearPad(SPI_SW_SCK_GPIO, SPI_SW_SCK_PIN);
			spi_delay();
		}

		if (in_buf) {
			in_buf[i] = recieve;
		}
	}
}

void hw_axiom_init_FPGA_CLK(void) {
	/* Configure PLLI2S prescalers */
	/* PLLI2S_VCO : VCO_192M */
	/* SAI_CLK(first level) = PLLI2S_VCO/PLLI2SQ = 192/4 = 48 Mhz */
	RCC->PLLI2SCFGR = (192 << 6) | (4 << 28);
	/* Enable PLLI2S Clock */
	RCC_PLLI2SCmd(ENABLE);

	/* Wait till PLLI2S is ready */
	while(RCC_GetFlagStatus(RCC_FLAG_PLLI2SRDY) == RESET)
	{
	}

	/* Configure MCO2 pin(PC9) in alternate function */
	palSetPadMode(AXIOM_FPGA_CLK_PORT, AXIOM_FPGA_CLK_PIN, PAL_MODE_ALTERNATE(GPIO_AF_MCO) |
				PAL_STM32_OTYPE_PUSHPULL |
				PAL_STM32_OSPEED_HIGHEST |
				PAL_STM32_PUDR_PULLUP);

	// HSE clock selected to output on MCO2 pin(PA8) 48MHz/4 = 12MHz
	RCC_MCO2Config(RCC_MCO2Source_PLLI2SCLK, RCC_MCO2Div_4);
}

char hw_axiom_configure_FPGA(void) {
	// use CCM SRAM for this 2kB decompressor buffer
	__attribute__((section(".ram4"))) static uint8_t __LZO_MMODEL outputBuffer[BITSTREAM_CHUNK_SIZE] = {0};

    int r;
    uint32_t index = 0;
    const int16_t chunks = BITSTREAM_SIZE / BITSTREAM_CHUNK_SIZE + 1;
    lzo_uint decompressed_len;
    lzo_uint decompressed_bitstream_size = 0;

	r = lzo_init(); // Initialize decompressor

	spi_begin();
	palSetPad(SPI_SW_SCK_GPIO, SPI_SW_SCK_PIN);
	palClearPad(AXIOM_FPGA_RESET_PORT, AXIOM_FPGA_RESET_PIN);
	chThdSleep(10);
	palSetPad(AXIOM_FPGA_RESET_PORT, AXIOM_FPGA_RESET_PIN);
	chThdSleep(20);

    for (int i = 0; i < chunks; i++) {
        uint16_t compressed_chunk_size = (uint16_t)FPGA_bitstream[index++] << 8;
    	compressed_chunk_size |= (uint8_t)FPGA_bitstream[index++];

        if( i == (chunks - 1) ) {
        	decompressed_len = BITSTREAM_SIZE % BITSTREAM_CHUNK_SIZE;
        }
        else {
        	decompressed_len =  BITSTREAM_CHUNK_SIZE;
        }

		r = lzo1x_decompress_safe(FPGA_bitstream + index,compressed_chunk_size, outputBuffer, &decompressed_len,NULL);
		decompressed_bitstream_size += decompressed_len;
		index += compressed_chunk_size;

		if (r != LZO_E_OK) {
			break;
		}

		spi_transfer(0, outputBuffer, decompressed_len);
    }

	//include 49 extra spi clock cycles, dummy bytes
	uint8_t dummy = 0;
	spi_transfer(0, &dummy, 7);
	spi_end();

	// CDONE LED should be set by now
	if( (r != LZO_E_OK) || (decompressed_bitstream_size != BITSTREAM_SIZE) )
		commands_printf("Error decompressing FPGA image.\n");

	return 0;
}

static void spi_begin(void) {
	palClearPad(SPI_SW_FPGA_CS_GPIO, SPI_SW_FPGA_CS_PIN);
}

static void spi_end(void) {
	palSetPad(SPI_SW_FPGA_CS_GPIO, SPI_SW_FPGA_CS_PIN);
}

static void spi_delay(void) {
	__NOP();
	__NOP();
	__NOP();
	__NOP();

	__NOP();
	__NOP();
	__NOP();
	__NOP();
}

static void terminal_cmd_store_current_sensor_gain(int argc, const char **argv) {
	(void)argc;
	(void)argv;

	eeprom_var current_gain;
	if( argc == 2 ) {
		sscanf(argv[1], "%f", &(current_gain.as_float));

		// Store data in eeprom
		conf_general_store_eeprom_var_hw(&current_gain, EEPROM_ADDR_CURRENT_GAIN);

		//read back written data
		current_sensor_gain = hw_axiom_read_current_sensor_gain();

		if(current_sensor_gain == current_gain.as_float) {
			commands_printf("Axiom current sensor sensor gain set as %.8f", (double)current_sensor_gain);
		}
		else {
			current_sensor_gain = 0.0;
			commands_printf("Error storing EEPROM data.");
		}
	}
	else {
		commands_printf("1 argument required. For example: axiom_store_current_sensor_gain 0.003761");
		commands_printf(" ");
	}
	commands_printf(" ");
	return;
}

static void terminal_cmd_read_current_sensor_gain(int argc, const char **argv) {
	(void)argc;
	(void)argv;

	//read back written data
	current_sensor_gain = hw_axiom_read_current_sensor_gain();

	commands_printf("Axiom current sensor sensor gain is set as %.8f", (double)current_sensor_gain);
	commands_printf(" ");
	return;
}

float hw_axiom_read_current_sensor_gain() {
	eeprom_var current_gain;

	conf_general_read_eeprom_var_hw(&current_gain, EEPROM_ADDR_CURRENT_GAIN);

	if( (current_gain.as_float <= 0) || (current_gain.as_float >= 1) )
		current_gain.as_float = DEFAULT_CURRENT_AMP_GAIN;
	return current_gain.as_float;
}

inline float hw_axiom_get_current_sensor_gain() {
	return current_sensor_gain;
}

float hw_axiom_get_highest_IGBT_temp() {
	float t1 = NTC_TEMP_MOS1();
	float t2 = NTC_TEMP_MOS2();
	float t3 = NTC_TEMP_MOS3();
	float res = 0.0;

	if (t1 > t2 && t1 > t3) {
		res = t1;
	} else if (t2 > t1 && t2 > t3) {
		res = t2;
	} else {
		res = t3;
	}

	return res;
}
