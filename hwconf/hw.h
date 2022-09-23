/*
	Copyright 2012 - 2020 Benjamin Vedder	benjamin@vedder.se

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

#ifndef HW_H_
#define HW_H_

#include "conf_general.h"
#include "stm32f4xx_conf.h"

#include HW_HEADER

#ifndef HW_NAME
#error "No hardware name set"
#endif

// Possible HW properties.

/*
 * #define HW_HAS_DRV8301
 *
 * Use the DRV8301 driver
 *
 * Requires defining SPI pins for the driver:
 *
 * #define DRV8301_MOSI_GPIO	GPIOC
 * #define DRV8301_MOSI_PIN		12
 * #define DRV8301_MISO_GPIO	GPIOC
 * #define DRV8301_MISO_PIN		11
 * #define DRV8301_SCK_GPIO		GPIOC
 * #define DRV8301_SCK_PIN		10
 * #define DRV8301_CS_GPIO		GPIOC
 * #define DRV8301_CS_PIN		9
 *
 * Requires running
 * drv8301_init();
 * at the end of void hw_init_gpio(void)
 */

/*
 * #define HW_HAS_DRV8313
 *
 * Use the DRV8313 driver. Changes how the
 * output is switched off and runs an early init hook.
 */

/*
 * #define HW_HAS_DRV8305
 *
 * Use the DRV8305 gate driver. Note that support
 * for this driver is incomplete.
 *
 * Requires defining SPI pins for the driver:
 *
 * #define DRV8305_MOSI_GPIO		GPIOC
 * #define DRV8305_MOSI_PIN			12
 * #define DRV8305_MISO_GPIO		GPIOC
 * #define DRV8305_MISO_PIN			11
 * #define DRV8305_SCK_GPIO			GPIOC
 * #define DRV8305_SCK_PIN			10
 * #define DRV8305_CS_GPIO			GPIOD
 * #define DRV8305_CS_PIN			8
 *
 * Requires running
 * drv8305_init();
 * at the end of void hw_init_gpio(void)
 */

/*
 * #define HW_HAS_DRV8320S
 *
 * Use the DRV8320S driver.
 *
 * Requires defining SPI pins for the driver:
 *
 * #define DRV8320S_MOSI_GPIO		GPIOC
 * #define DRV8320S_MOSI_PIN		12
 * #define DRV8320S_MISO_GPIO		GPIOC
 * #define DRV8320S_MISO_PIN		11
 * #define DRV8320S_SCK_GPIO		GPIOC
 * #define DRV8320S_SCK_PIN			10
 * #define DRV8320S_CS_GPIO			GPIOC
 * #define DRV8320S_CS_PIN			9
 *
 * Requires running
 * drv8320s_init();
 * at the end of void hw_init_gpio(void)
 */

/*
 * #define HW_HAS_DRV8323S
 *
 * Use the DRV8323S driver.
 *
 * Requires defining SPI pins for the driver:
 *
 * #define DRV8323S_MOSI_GPIO		GPIOC
 * #define DRV8323S_MOSI_PIN		12
 * #define DRV8323S_MISO_GPIO		GPIOB
 * #define DRV8323S_MISO_PIN		4
 * #define DRV8323S_SCK_GPIO		GPIOB
 * #define DRV8323S_SCK_PIN			3
 * #define DRV8323S_CS_GPIO			GPIOC
 * #define DRV8323S_CS_PIN			9
 *
 * Requires running
 * drv8323s_init();
 * at the end of void hw_init_gpio(void)
 */

/*
 * #define HW_USE_INTERNAL_RC
 *
 * Use the internal RC clock instead of an external crystal.
 */

/*
 * define HW_HAS_3_SHUNTS
 *
 * The hardware has 3 current sensors.
 */

/*
 * #define HW_HAS_PHASE_SHUNTS
 *
 * The current sensors are in line with the motor phases,
 * which allows sampling in V0 and V7, and some extra filtering.
 */

/*
 * #define HW_HAS_NO_CAN
 *
 * The hardware is missing CAN-bus.
 */

/*
 * Define these to enable MPU9150 or MPU9250 support
 * on these pins.
 *
 * #define MPU9X50_SDA_GPIO		GPIOB
 * #define MPU9X50_SDA_PIN		7
 * #define MPU9X50_SCL_GPIO		GPIOB
 * #define MPU9X50_SCL_PIN		6
 *
 * This will flip the axes of the IMU around the horizontal plane
 * #define MPU9x50_FLIP
 */

/*
 * #define HW_HAS_PERMANENT_NRF
 *
 * The hardware has a permanently mounted NRF24. Also requires defining its pins:
 * #define NRF_PORT_CSN			GPIOB
 * #define NRF_PIN_CSN			12
 * #define NRF_PORT_SCK			GPIOB
 * #define NRF_PIN_SCK			4
 * #define NRF_PORT_MOSI		GPIOB
 * #define NRF_PIN_MOSI			3
 * #define NRF_PORT_MISO		GPIOD
 * #define NRF_PIN_MISO			2
 *
 * If the NRF is not detected during initialization, the pins will be remapped
 * to the COMM port if the NRF app is used. A hook to run if initialization fails
 * can also be defined:
 *
 * #define HW_PERMANENT_NRF_FAILED_HOOK()
 */

// Default macros in case there is no hardware support or no need to change them.

#ifndef ENABLE_GATE
#define ENABLE_GATE()
#endif
#ifndef DISABLE_GATE
#define DISABLE_GATE()
#endif
#ifndef DCCAL_ON
#define DCCAL_ON()
#endif
#ifndef DCCAL_OFF
#define DCCAL_OFF()
#endif
#ifndef IS_DRV_FAULT
#define IS_DRV_FAULT()			0
#endif
#ifndef IS_DRV_FAULT_2
#define IS_DRV_FAULT_2()		IS_DRV_FAULT()
#endif

// Double samples in beginning and end for positive current measurement.
// Useful when the shunt sense traces have noise that causes offset.
#ifndef CURR1_DOUBLE_SAMPLE
#define CURR1_DOUBLE_SAMPLE		0
#endif
#ifndef CURR2_DOUBLE_SAMPLE
#define CURR2_DOUBLE_SAMPLE		0
#endif
#ifndef CURR3_DOUBLE_SAMPLE
#define CURR3_DOUBLE_SAMPLE		0
#endif

#ifndef AUX_ON
#define AUX_ON()
#endif
#ifndef AUX_OFF
#define AUX_OFF()
#endif

#ifndef PHASE_FILTER_ON
#define PHASE_FILTER_ON()
#endif
#ifndef PHASE_FILTER_OFF
#define PHASE_FILTER_OFF()
#endif

#ifndef CURRENT_FILTER_ON
#define CURRENT_FILTER_ON()
#endif
#ifndef CURRENT_FILTER_OFF
#define CURRENT_FILTER_OFF()
#endif

// VCC net voltage
#ifndef V_REG
#define V_REG				3.3
#endif

// Individual MOSFET temperature sensors. Override if available.
#ifndef NTC_TEMP_MOS1
#define NTC_TEMP_MOS1()		0.0
#endif
#ifndef NTC_TEMP_MOS2
#define NTC_TEMP_MOS2()		0.0
#endif
#ifndef NTC_TEMP_MOS3
#define NTC_TEMP_MOS3()		0.0
#endif

// Sin/Cos Encoder Signals. Override if available
#ifndef ENCODER_SIN_VOLTS
#define ENCODER_SIN_VOLTS()		0.0
#endif
#ifndef ENCODER_COS_VOLTS
#define ENCODER_COS_VOLTS()		0.0
#endif

// Current ADC macros. Override them for custom current measurement functions.
#ifndef GET_CURRENT1
#ifdef INVERTED_SHUNT_POLARITY
#define GET_CURRENT1()		(4095 - ADC_Value[ADC_IND_CURR1])
#else
#define GET_CURRENT1()		ADC_Value[ADC_IND_CURR1]
#endif
#endif
#ifndef GET_CURRENT2
#ifdef INVERTED_SHUNT_POLARITY
#define GET_CURRENT2()		(4095 - ADC_Value[ADC_IND_CURR2])
#else
#define GET_CURRENT2()		ADC_Value[ADC_IND_CURR2]
#endif
#endif
#ifndef GET_CURRENT3
#ifdef INVERTED_SHUNT_POLARITY
#define GET_CURRENT3()		(4095 - ADC_Value[ADC_IND_CURR3])
#else
#define GET_CURRENT3()		ADC_Value[ADC_IND_CURR3]
#endif
#endif

#ifndef GET_CURRENT1_M2
#ifdef INVERTED_SHUNT_POLARITY
#define GET_CURRENT1_M2()	(4095 - ADC_Value[ADC_IND_CURR4])
#else
#define GET_CURRENT1_M2()	ADC_Value[ADC_IND_CURR4]
#endif
#endif
#ifndef GET_CURRENT2_M2
#ifdef INVERTED_SHUNT_POLARITY
#define GET_CURRENT2_M2()	(4095 - ADC_Value[ADC_IND_CURR5])
#else
#define GET_CURRENT2_M2()	ADC_Value[ADC_IND_CURR5]
#endif
#endif
#ifndef GET_CURRENT3_M2
#ifdef INVERTED_SHUNT_POLARITY
#define GET_CURRENT3_M2()	(4095 - ADC_Value[ADC_IND_CURR6])
#else
#define GET_CURRENT3_M2()	ADC_Value[ADC_IND_CURR6]
#endif
#endif

#ifndef HW_MAX_CURRENT_OFFSET
#define HW_MAX_CURRENT_OFFSET 				620
#endif
#ifndef MCCONF_MAX_CURRENT_UNBALANCE
#define MCCONF_MAX_CURRENT_UNBALANCE		(FAC_CURRENT * 512)
#endif
#ifndef MCCONF_MAX_CURRENT_UNBALANCE_RATE
#define MCCONF_MAX_CURRENT_UNBALANCE_RATE	0.3
#endif

// ADC Channels
#ifndef ADC_IND_EXT3
#define ADC_IND_EXT3 			ADC_IND_EXT
#endif
#ifndef ADC_IND_EXT2
#define ADC_IND_EXT2 			ADC_IND_EXT
#endif

// NRF SW SPI (default to spi header pins)
#ifndef NRF_PORT_CSN
#define NRF_PORT_CSN			HW_SPI_PORT_NSS
#endif
#ifndef NRF_PIN_CSN
#define NRF_PIN_CSN				HW_SPI_PIN_NSS
#endif
#ifndef NRF_PORT_SCK
#define NRF_PORT_SCK			HW_SPI_PORT_SCK
#endif
#ifndef NRF_PIN_SCK
#define NRF_PIN_SCK				HW_SPI_PIN_SCK
#endif
#ifndef NRF_PORT_MOSI
#define NRF_PORT_MOSI			HW_SPI_PORT_MOSI
#endif
#ifndef NRF_PIN_MOSI
#define NRF_PIN_MOSI			HW_SPI_PIN_MOSI
#endif
#ifndef NRF_PORT_MISO
#define NRF_PORT_MISO			HW_SPI_PORT_MISO
#endif
#ifndef NRF_PIN_MISO
#define NRF_PIN_MISO			HW_SPI_PIN_MISO
#endif

// CAN device and port (default CAN1)
#ifndef HW_CANRX_PORT
#define HW_CANRX_PORT			GPIOB
#endif
#ifndef HW_CANRX_PIN
#define HW_CANRX_PIN			8
#endif
#ifndef HW_CANTX_PORT
#define HW_CANTX_PORT			GPIOB
#endif
#ifndef HW_CANTX_PIN
#define HW_CANTX_PIN			9
#endif
#ifndef HW_CAN_GPIO_AF
#define HW_CAN_GPIO_AF			GPIO_AF_CAN1
#endif
#ifndef HW_CAN_DEV
#define HW_CAN_DEV				CAND1
#endif

// Hook to call when trying to initialize the permanent NRF failed. Can be
// used to e.g. reconfigure pins.
#ifndef HW_PERMANENT_NRF_FAILED_HOOK
#define HW_PERMANENT_NRF_FAILED_HOOK()
#endif

#ifndef HW_EARLY_INIT
#define HW_EARLY_INIT()
#endif

// Default ID
#ifndef HW_DEFAULT_ID
#define HW_DEFAULT_ID			(APPCONF_CONTROLLER_ID >= 0 ? APPCONF_CONTROLLER_ID : hw_id_from_uuid())
#endif

#ifndef HW_LIM_CURRENT
#define HW_LIM_CURRENT			-100.0, 100.0
#endif
#ifndef HW_LIM_CURRENT_ABS
#define HW_LIM_CURRENT_ABS		0.0, 140.0
#endif

#ifndef HW_LIM_FOC_CTRL_LOOP_FREQ
#define HW_LIM_FOC_CTRL_LOOP_FREQ	3000.0, 30000.0
#endif

#ifndef HW_FOC_CURRENT_FILTER_LIM
#define HW_FOC_CURRENT_FILTER_LIM	0.05, 1.0
#endif

#ifndef COMM_USE_USB
#define COMM_USE_USB				1
#endif

#ifndef PTC_TEMP_MOTOR
#if defined(NTC_RES_MOTOR) && defined(ADC_IND_TEMP_MOTOR)
#define PTC_TEMP_MOTOR(res, con, tbase)			(((NTC_RES_MOTOR(ADC_Value[ADC_IND_TEMP_MOTOR]) - res) / res) * 100 / con + tbase)
#define PTC_TEMP_MOTOR_2(res, con, tbase)			(((NTC_RES_MOTOR(ADC_Value[ADC_IND_TEMP_MOTOR_2]) - res) / res) * 100 / con + tbase)
#else
#define PTC_TEMP_MOTOR(res, con, tbase)			0.0
#define PTC_TEMP_MOTOR_2(res, con, tbase)			0.0
#endif
#endif

// Default second motor defines
#ifndef READ_HALL1_2
#define READ_HALL1_2()			READ_HALL1()
#endif
#ifndef READ_HALL2_2
#define READ_HALL2_2()			READ_HALL2()
#endif
#ifndef READ_HALL3_2
#define READ_HALL3_2()			READ_HALL3()
#endif
#ifndef ADC_IND_TEMP_MOS_M2
#define ADC_IND_TEMP_MOS_M2		ADC_IND_TEMP_MOS
#endif
#ifndef NTC_TEMP_MOTOR_2
#define NTC_TEMP_MOTOR_2(beta)	NTC_TEMP_MOTOR(beta)
#endif
#ifndef ADC_IND_TEMP_MOTOR_2
#define ADC_IND_TEMP_MOTOR_2	ADC_IND_TEMP_MOTOR
#endif
#ifndef  MOTOR_TEMP_LPF
#define MOTOR_TEMP_LPF 			0.01
#endif
#ifndef HW_ADC_CHANNELS_EXTRA
#define HW_ADC_CHANNELS_EXTRA	0
#endif
#ifndef ADC_V_L4
#define ADC_V_L4				ADC_V_L1
#endif
#ifndef ADC_V_L5
#define ADC_V_L5				ADC_V_L2
#endif
#ifndef ADC_V_L6
#define ADC_V_L6				ADC_V_L3
#endif

#ifdef HW_HAS_DRV8323S
#ifndef DRV8323S_CUSTOM_SETTINGS
#define DRV8323S_CUSTOM_SETTINGS()
#endif
#endif

// Functions
void hw_init_gpio(void);
void hw_setup_adc_channels(void);
void hw_start_i2c(void);
void hw_stop_i2c(void);
void hw_try_restore_i2c(void);
uint8_t hw_id_from_uuid(void);

#endif /* HW_H_ */
