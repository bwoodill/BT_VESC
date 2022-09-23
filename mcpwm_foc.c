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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mcpwm_foc.h"
#include "mc_interface.h"
#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"
#include "digital_filter.h"
#include "utils.h"
#include "ledpwm.h"
#include "terminal.h"
#include "encoder.h"
#include "commands.h"
#include "timeout.h"
#include "timer.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "virtual_motor.h"
#include "digital_filter.h"

// Private types
typedef struct {
	float id_target;
	float iq_target;
	float max_duty;
	float duty_now;
	float phase;
	float i_alpha;
	float i_beta;
	float i_abs;
	float i_abs_filter;
	float i_bus;
	float v_bus;
	float v_alpha;
	float v_beta;
	float mod_d;
	float mod_q;
	float id;
	float iq;
	float id_filter;
	float iq_filter;
	float vd;
	float vq;
	float vd_int;
	float vq_int;
	float speed_rad_s;
	uint32_t svm_sector;
} motor_state_t;

typedef struct {
	int sample_num;
	float avg_current_tot;
	float avg_voltage_tot;
} mc_sample_t;

typedef struct {
	void(*fft_bin0_func)(float*, float*, float*);
	void(*fft_bin1_func)(float*, float*, float*);
	void(*fft_bin2_func)(float*, float*, float*);

	int samples;
	int table_fact;
	float buffer[32];
	float buffer_current[32];
	bool ready;
	int ind;
	bool is_samp_n;
	float prev_sample;
	float angle;
	int est_done_cnt;
	float observer_zero_time;
	int flip_cnt;
} hfi_state_t;

typedef struct {
	volatile mc_configuration *m_conf;
	mc_state m_state;
	mc_control_mode m_control_mode;
	motor_state_t m_motor_state;
	int m_curr_unbalance;
	bool m_phase_override;
	float m_phase_now_override;
	float m_duty_cycle_set;
	float m_id_set;
	float m_iq_set;
	float m_openloop_speed;
	float m_openloop_phase;
	bool m_output_on;
	float m_pos_pid_set;
	float m_speed_pid_set_rpm;
	float m_phase_now_observer;
	float m_phase_now_observer_override;
	bool m_phase_observer_override;
	float m_phase_now_encoder;
	float m_phase_now_encoder_no_index;
	float m_observer_x1;
	float m_observer_x2;
	float m_pll_phase;
	float m_pll_speed;
	mc_sample_t m_samples;
	int m_tachometer;
	int m_tachometer_abs;
	float m_pos_pid_now;
	float m_gamma_now;
	bool m_using_encoder;
	float m_speed_est_fast;
	float m_speed_est_faster;
	int m_curr_samples;
	int m_curr_sum[3];
	int m_curr_ofs[3];
	int m_duty1_next, m_duty2_next, m_duty3_next;
	bool m_duty_next_set;
	hfi_state_t m_hfi;
	int m_hfi_plot_en;
	float m_hfi_plot_sample;

	float m_phase_before;
	float m_duty_filtered;
	bool m_was_full_brake;
	bool m_was_control_duty;
	float m_duty_i_term;
	float m_openloop_angle;
	float m_x1_prev;
	float m_x2_prev;
	float m_phase_before_speed_est;
	int m_tacho_step_last;
	float m_pid_div_angle_last;
	float m_min_rpm_hyst_timer;
	float m_min_rpm_timer;
	bool m_cc_was_hfi;
	float m_pos_i_term;
	float m_pos_prev_error;
	float m_pos_dt_int;
	float m_pos_d_filter;
	float m_speed_i_term;
	float m_speed_prev_error;
	float m_speed_d_filter;
	int m_ang_hall_int_prev;
	bool m_using_hall;
	float m_ang_hall;
	float m_hall_dt_diff_last;
	float m_hall_dt_diff_now;
} motor_all_state_t;

// Private variables
static volatile bool m_dccal_done = false;
static volatile float m_last_adc_isr_duration;
static volatile bool m_init_done = false;
static volatile motor_all_state_t m_motor_1;
#ifdef HW_HAS_DUAL_MOTORS
static volatile motor_all_state_t m_motor_2;
#endif
static volatile int m_isr_motor = 0;

// Private functions
static void do_dc_cal(void);
void observer_update(float v_alpha, float v_beta, float i_alpha, float i_beta,
					 float dt, volatile float *x1, volatile float *x2, volatile float *phase, volatile motor_all_state_t *motor);
static void pll_run(float phase, float dt, volatile float *phase_var,
					volatile float *speed_var, volatile mc_configuration *conf);
static void control_current(volatile motor_all_state_t *motor, float dt);
static void svm(float alpha, float beta, uint32_t PWMHalfPeriod,
				uint32_t* tAout, uint32_t* tBout, uint32_t* tCout, uint32_t *svm_sector);
static void run_pid_control_pos(float angle_now, float angle_set, float dt, volatile motor_all_state_t *motor);
static void run_pid_control_speed(float dt, volatile motor_all_state_t *motor);
static void stop_pwm_hw(volatile motor_all_state_t *motor);
static void start_pwm_hw(volatile motor_all_state_t *motor);
static float correct_encoder(float obs_angle, float enc_angle, float speed, float sl_erpm, volatile motor_all_state_t *motor);
static float correct_hall(float angle, float dt, volatile motor_all_state_t *motor);
static void terminal_plot_hfi(int argc, const char **argv);
static void timer_update(volatile motor_all_state_t *motor, float dt);
static void hfi_update(volatile motor_all_state_t *motor);

// Threads
static THD_WORKING_AREA(timer_thread_wa, 1024);
static THD_FUNCTION(timer_thread, arg);
static volatile bool timer_thd_stop;

static THD_WORKING_AREA(hfi_thread_wa, 1024);
static THD_FUNCTION(hfi_thread, arg);
static volatile bool hfi_thd_stop;

// Macros
#ifdef HW_HAS_3_SHUNTS
#define TIMER_UPDATE_DUTY_M1(duty1, duty2, duty3) \
		TIM1->CR1 |= TIM_CR1_UDIS; \
		TIM1->CCR1 = duty1; \
		TIM1->CCR2 = duty2; \
		TIM1->CCR3 = duty3; \
		TIM1->CR1 &= ~TIM_CR1_UDIS;

#define TIMER_UPDATE_DUTY_M2(duty1, duty2, duty3) \
		TIM8->CR1 |= TIM_CR1_UDIS; \
		TIM8->CCR1 = duty1; \
		TIM8->CCR2 = duty2; \
		TIM8->CCR3 = duty3; \
		TIM8->CR1 &= ~TIM_CR1_UDIS;
#else
#define TIMER_UPDATE_DUTY_M1(duty1, duty2, duty3) \
		TIM1->CR1 |= TIM_CR1_UDIS; \
		TIM1->CCR1 = duty1; \
		TIM1->CCR2 = duty3; \
		TIM1->CCR3 = duty2; \
		TIM1->CR1 &= ~TIM_CR1_UDIS;
#define TIMER_UPDATE_DUTY_M2(duty1, duty2, duty3) \
		TIM8->CR1 |= TIM_CR1_UDIS; \
		TIM8->CCR1 = duty1; \
		TIM8->CCR2 = duty3; \
		TIM8->CCR3 = duty2; \
		TIM8->CR1 &= ~TIM_CR1_UDIS;
#endif

#define TIMER_UPDATE_SAMP(samp) \
		TIM2->CCR2 = (samp / 2);

#define TIMER_UPDATE_SAMP_TOP_M1(samp, top) \
		TIM1->CR1 |= TIM_CR1_UDIS; \
		TIM2->CR1 |= TIM_CR1_UDIS; \
		TIM1->ARR = top; \
		TIM2->CCR2 = samp / 2; \
		TIM1->CR1 &= ~TIM_CR1_UDIS; \
		TIM2->CR1 &= ~TIM_CR1_UDIS;
#define TIMER_UPDATE_SAMP_TOP_M2(samp, top) \
		TIM8->CR1 |= TIM_CR1_UDIS; \
		TIM2->CR1 |= TIM_CR1_UDIS; \
		TIM8->ARR = top; \
		TIM2->CCR2 = samp / 2; \
		TIM8->CR1 &= ~TIM_CR1_UDIS; \
		TIM2->CR1 &= ~TIM_CR1_UDIS;

#ifdef HW_HAS_3_SHUNTS
#define TIMER_UPDATE_DUTY_SAMP_M1(duty1, duty2, duty3, samp) \
		TIM1->CR1 |= TIM_CR1_UDIS; \
		TIM2->CR1 |= TIM_CR1_UDIS; \
		TIM1->CCR1 = duty1; \
		TIM1->CCR2 = duty2; \
		TIM1->CCR3 = duty3; \
		TIM2->CCR2 = samp / 2; \
		TIM1->CR1 &= ~TIM_CR1_UDIS; \
		TIM2->CR1 &= ~TIM_CR1_UDIS;
#define TIMER_UPDATE_DUTY_SAMP_M2(duty1, duty2, duty3, samp) \
		TIM8->CR1 |= TIM_CR1_UDIS; \
		TIM2->CR1 |= TIM_CR1_UDIS; \
		TIM8->CCR1 = duty1; \
		TIM8->CCR2 = duty2; \
		TIM8->CCR3 = duty3; \
		TIM2->CCR2 = samp / 2; \
		TIM8->CR1 &= ~TIM_CR1_UDIS; \
		TIM2->CR1 &= ~TIM_CR1_UDIS;
#else
#define TIMER_UPDATE_DUTY_SAMP_M1(duty1, duty2, duty3, samp) \
		TIM1->CR1 |= TIM_CR1_UDIS; \
		TIM2->CR1 |= TIM_CR1_UDIS; \
		TIM1->CCR1 = duty1; \
		TIM1->CCR2 = duty3; \
		TIM1->CCR3 = duty2; \
		TIM2->CCR2 = samp / 2; \
		TIM1->CR1 &= ~TIM_CR1_UDIS; \
		TIM2->CR1 &= ~TIM_CR1_UDIS;
#define TIMER_UPDATE_DUTY_SAMP_M2(duty1, duty2, duty3, samp) \
		TIM8->CR1 |= TIM_CR1_UDIS; \
		TIM2->CR1 |= TIM_CR1_UDIS; \
		TIM8->CCR1 = duty1; \
		TIM8->CCR2 = duty3; \
		TIM8->CCR3 = duty2; \
		TIM2->CCR2 = samp / 2; \
		TIM8->CR1 &= ~TIM_CR1_UDIS; \
		TIM2->CR1 &= ~TIM_CR1_UDIS;
#endif

static void update_hfi_samples(foc_hfi_samples samples, volatile motor_all_state_t *motor) {
	utils_sys_lock_cnt();

	memset((void*)&motor->m_hfi, 0, sizeof(motor->m_hfi));
	switch (samples) {
	case HFI_SAMPLES_8:
		motor->m_hfi.samples = 8;
		motor->m_hfi.table_fact = 4;
		motor->m_hfi.fft_bin0_func = utils_fft8_bin0;
		motor->m_hfi.fft_bin1_func = utils_fft8_bin1;
		motor->m_hfi.fft_bin2_func = utils_fft8_bin2;
		break;

	case HFI_SAMPLES_16:
		motor->m_hfi.samples = 16;
		motor->m_hfi.table_fact = 2;
		motor->m_hfi.fft_bin0_func = utils_fft16_bin0;
		motor->m_hfi.fft_bin1_func = utils_fft16_bin1;
		motor->m_hfi.fft_bin2_func = utils_fft16_bin2;
		break;

	case HFI_SAMPLES_32:
		motor->m_hfi.samples = 32;
		motor->m_hfi.table_fact = 1;
		motor->m_hfi.fft_bin0_func = utils_fft32_bin0;
		motor->m_hfi.fft_bin1_func = utils_fft32_bin1;
		motor->m_hfi.fft_bin2_func = utils_fft32_bin2;
		break;
	}

	utils_sys_unlock_cnt();
}

static void timer_reinit(int f_sw) {
	utils_sys_lock_cnt();

	TIM_DeInit(TIM1);
	TIM_DeInit(TIM8);
	TIM_DeInit(TIM2);

	TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
	TIM_OCInitTypeDef TIM_OCInitStructure;
	TIM_BDTRInitTypeDef TIM_BDTRInitStructure;

	TIM1->CNT = 0;
	TIM2->CNT = 0;
	TIM8->CNT = 0;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM8, ENABLE);

	// Time Base configuration
	TIM_TimeBaseStructure.TIM_Prescaler = 0;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_CenterAligned1;
	TIM_TimeBaseStructure.TIM_Period = (SYSTEM_CORE_CLOCK / f_sw);
	TIM_TimeBaseStructure.TIM_ClockDivision = 0;
	TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;

	TIM_TimeBaseInit(TIM1, &TIM_TimeBaseStructure);
	TIM_TimeBaseInit(TIM8, &TIM_TimeBaseStructure);

	// Channel 1, 2 and 3 Configuration in PWM mode
	TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
	TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
	TIM_OCInitStructure.TIM_OutputNState = TIM_OutputNState_Enable;
	TIM_OCInitStructure.TIM_Pulse = TIM1->ARR / 2;
	TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
	TIM_OCInitStructure.TIM_OCNPolarity = TIM_OCNPolarity_High;
	TIM_OCInitStructure.TIM_OCIdleState = TIM_OCIdleState_Set;
	TIM_OCInitStructure.TIM_OCNIdleState = TIM_OCNIdleState_Set;

	TIM_OC1Init(TIM1, &TIM_OCInitStructure);
	TIM_OC2Init(TIM1, &TIM_OCInitStructure);
	TIM_OC3Init(TIM1, &TIM_OCInitStructure);
	TIM_OC4Init(TIM1, &TIM_OCInitStructure);

	TIM_OC1PreloadConfig(TIM1, TIM_OCPreload_Enable);
	TIM_OC2PreloadConfig(TIM1, TIM_OCPreload_Enable);
	TIM_OC3PreloadConfig(TIM1, TIM_OCPreload_Enable);
	TIM_OC4PreloadConfig(TIM1, TIM_OCPreload_Enable);

	TIM_OC1Init(TIM8, &TIM_OCInitStructure);
	TIM_OC2Init(TIM8, &TIM_OCInitStructure);
	TIM_OC3Init(TIM8, &TIM_OCInitStructure);
	TIM_OC4Init(TIM8, &TIM_OCInitStructure);

	TIM_OC1PreloadConfig(TIM8, TIM_OCPreload_Enable);
	TIM_OC2PreloadConfig(TIM8, TIM_OCPreload_Enable);
	TIM_OC3PreloadConfig(TIM8, TIM_OCPreload_Enable);
	TIM_OC4PreloadConfig(TIM8, TIM_OCPreload_Enable);

	// Automatic Output enable, Break, dead time and lock configuration
	TIM_BDTRInitStructure.TIM_OSSRState = TIM_OSSRState_Enable;
	TIM_BDTRInitStructure.TIM_OSSIState = TIM_OSSIState_Enable;
	TIM_BDTRInitStructure.TIM_LOCKLevel = TIM_LOCKLevel_OFF;
	TIM_BDTRInitStructure.TIM_DeadTime =  conf_general_calculate_deadtime(HW_DEAD_TIME_NSEC, SYSTEM_CORE_CLOCK);
	TIM_BDTRInitStructure.TIM_AutomaticOutput = TIM_AutomaticOutput_Disable;

#ifdef HW_USE_BRK
	// Enable BRK function. Hardware will asynchronously stop any PWM activity upon an
	// external fault signal. PWM outputs remain disabled until MCU is reset.
	// software will catch the BRK flag to report the fault code
	TIM_BDTRInitStructure.TIM_Break = TIM_Break_Enable;
	TIM_BDTRInitStructure.TIM_BreakPolarity = TIM_BreakPolarity_Low;
#else
	TIM_BDTRInitStructure.TIM_Break = TIM_Break_Disable;
	TIM_BDTRInitStructure.TIM_BreakPolarity = TIM_BreakPolarity_High;
#endif

	TIM_BDTRConfig(TIM1, &TIM_BDTRInitStructure);
	TIM_CCPreloadControl(TIM1, ENABLE);
	TIM_ARRPreloadConfig(TIM1, ENABLE);

	TIM_BDTRConfig(TIM8, &TIM_BDTRInitStructure);
	TIM_CCPreloadControl(TIM8, ENABLE);
	TIM_ARRPreloadConfig(TIM8, ENABLE);

	// ------------- Timer2 for ADC sampling ------------- //
	// Time Base configuration
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

	TIM_TimeBaseStructure.TIM_Prescaler = 0;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseStructure.TIM_Period = 0xFFFF;
	TIM_TimeBaseStructure.TIM_ClockDivision = 0;
	TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;
	TIM_TimeBaseInit(TIM2, &TIM_TimeBaseStructure);

	TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
	TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
	TIM_OCInitStructure.TIM_Pulse = 250;
	TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
	TIM_OCInitStructure.TIM_OCNPolarity = TIM_OCNPolarity_High;
	TIM_OCInitStructure.TIM_OCIdleState = TIM_OCIdleState_Set;
	TIM_OCInitStructure.TIM_OCNIdleState = TIM_OCNIdleState_Set;
	TIM_OC1Init(TIM2, &TIM_OCInitStructure);
	TIM_OC1PreloadConfig(TIM2, TIM_OCPreload_Enable);
	TIM_OC2Init(TIM2, &TIM_OCInitStructure);
	TIM_OC2PreloadConfig(TIM2, TIM_OCPreload_Enable);
	TIM_OC3Init(TIM2, &TIM_OCInitStructure);
	TIM_OC3PreloadConfig(TIM2, TIM_OCPreload_Enable);

	TIM_ARRPreloadConfig(TIM2, ENABLE);
	TIM_CCPreloadControl(TIM2, ENABLE);

	// PWM outputs have to be enabled in order to trigger ADC on CCx
	TIM_CtrlPWMOutputs(TIM2, ENABLE);

	// TIM1 Master and TIM8 slave
#if defined HW_HAS_DUAL_MOTORS || defined HW_HAS_DUAL_PARALLEL
	// TODO: Explain. See: https://www.cnblogs.com/shangdawei/p/4758988.html
	TIM_SelectOutputTrigger(TIM1, TIM_TRGOSource_Enable);
	TIM_SelectMasterSlaveMode(TIM1, TIM_MasterSlaveMode_Enable);
	TIM_SelectInputTrigger(TIM8, TIM_TS_ITR0);
	TIM_SelectSlaveMode(TIM8, TIM_SlaveMode_Trigger);
	TIM_SelectOutputTrigger(TIM8, TIM_TRGOSource_Enable);
	TIM_SelectOutputTrigger(TIM8, TIM_TRGOSource_Update);
	TIM_SelectInputTrigger(TIM2, TIM_TS_ITR1);
	TIM_SelectSlaveMode(TIM2, TIM_SlaveMode_Reset);
#else
	TIM_SelectOutputTrigger(TIM1, TIM_TRGOSource_Update);
	TIM_SelectMasterSlaveMode(TIM1, TIM_MasterSlaveMode_Enable);
	TIM_SelectInputTrigger(TIM2, TIM_TS_ITR0);
	TIM_SelectSlaveMode(TIM2, TIM_SlaveMode_Reset);
#endif

	// Enable TIM1 and TIM2
#ifdef HW_HAS_DUAL_MOTORS
	TIM8->CNT = TIM1->ARR;
#else
	TIM8->CNT = 0;
#endif
	TIM1->CNT = 0;
	TIM_Cmd(TIM1, ENABLE);
	TIM_Cmd(TIM2, ENABLE);

	// Prevent all low side FETs from switching on
	stop_pwm_hw(&m_motor_1);
#ifdef HW_HAS_DUAL_MOTORS
	stop_pwm_hw(&m_motor_2);
#endif

	// Main Output Enable
	TIM_CtrlPWMOutputs(TIM1, ENABLE);
	TIM_CtrlPWMOutputs(TIM8, ENABLE);

	// Sample intervals
	TIMER_UPDATE_SAMP(MCPWM_FOC_CURRENT_SAMP_OFFSET);

	// Enable CC2 interrupt, which will be fired in V0 and V7
	TIM_ITConfig(TIM2, TIM_IT_CC2, ENABLE);
	utils_sys_unlock_cnt();

	nvicEnableVector(TIM2_IRQn, 6);
}

void mcpwm_foc_init(volatile mc_configuration *conf_m1, volatile mc_configuration *conf_m2) {
	utils_sys_lock_cnt();

#ifndef HW_HAS_DUAL_MOTORS
	(void)conf_m2;
#endif

	m_init_done = false;

	// Initialize variables
	memset((void*)&m_motor_1, 0, sizeof(motor_all_state_t));
	m_isr_motor = 0;

	m_motor_1.m_conf = conf_m1;
	m_motor_1.m_state = MC_STATE_OFF;
	m_motor_1.m_control_mode = CONTROL_MODE_NONE;
	m_motor_1.m_hall_dt_diff_last = 1.0;
#ifdef HW_HAS_DUAL_PARALLEL
	m_motor_1.m_curr_ofs[0] = 4096;
	m_motor_1.m_curr_ofs[1] = 4096;
	m_motor_1.m_curr_ofs[2] = 4096;
#else
	m_motor_1.m_curr_ofs[0] = 2048;
	m_motor_1.m_curr_ofs[1] = 2048;
	m_motor_1.m_curr_ofs[2] = 2048;
#endif
	update_hfi_samples(m_motor_1.m_conf->foc_hfi_samples, &m_motor_1);

#ifdef HW_HAS_DUAL_MOTORS
	memset((void*)&m_motor_2, 0, sizeof(motor_all_state_t));
	m_motor_2.m_conf = conf_m2;
	m_motor_2.m_state = MC_STATE_OFF;
	m_motor_2.m_control_mode = CONTROL_MODE_NONE;
	m_motor_2.m_hall_dt_diff_last = 1.0;
	m_motor_2.m_curr_ofs[0] = 2048;
	m_motor_2.m_curr_ofs[1] = 2048;
	m_motor_2.m_curr_ofs[2] = 2048;
	update_hfi_samples(m_motor_2.m_conf->foc_hfi_samples, &m_motor_2);
#endif

	virtual_motor_init();

	TIM_DeInit(TIM1);
	TIM_DeInit(TIM2);
	TIM_DeInit(TIM8);
	TIM1->CNT = 0;
	TIM2->CNT = 0;
	TIM8->CNT = 0;

	// ADC
	ADC_CommonInitTypeDef ADC_CommonInitStructure;
	DMA_InitTypeDef DMA_InitStructure;
	ADC_InitTypeDef ADC_InitStructure;

	// Clock
	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2 | RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOC, ENABLE);
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1 | RCC_APB2Periph_ADC2 | RCC_APB2Periph_ADC3, ENABLE);

	dmaStreamAllocate(STM32_DMA_STREAM(STM32_DMA_STREAM_ID(2, 4)),
					  5,
					  (stm32_dmaisr_t)mcpwm_foc_adc_int_handler,
					  (void *)0);

	// DMA for the ADC
	DMA_InitStructure.DMA_Channel = DMA_Channel_0;
	DMA_InitStructure.DMA_Memory0BaseAddr = (uint32_t)&ADC_Value;
	DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&ADC->CDR;
	DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralToMemory;
	DMA_InitStructure.DMA_BufferSize = HW_ADC_CHANNELS;
	DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
	DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
	DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
	DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
	DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
	DMA_InitStructure.DMA_Priority = DMA_Priority_High;
	DMA_InitStructure.DMA_FIFOMode = DMA_FIFOMode_Disable;
	DMA_InitStructure.DMA_FIFOThreshold = DMA_FIFOThreshold_1QuarterFull;
	DMA_InitStructure.DMA_MemoryBurst = DMA_MemoryBurst_Single;
	DMA_InitStructure.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
	DMA_Init(DMA2_Stream4, &DMA_InitStructure);

	DMA_Cmd(DMA2_Stream4, ENABLE);
	DMA_ITConfig(DMA2_Stream4, DMA_IT_TC, ENABLE);

	// ADC Common Init
	// Note that the ADC is running at 42MHz, which is higher than the
	// specified 36MHz in the data sheet, but it works.
	ADC_CommonInitStructure.ADC_Mode = ADC_TripleMode_RegSimult;
	ADC_CommonInitStructure.ADC_Prescaler = ADC_Prescaler_Div2;
	ADC_CommonInitStructure.ADC_DMAAccessMode = ADC_DMAAccessMode_1;
	ADC_CommonInitStructure.ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles;
	ADC_CommonInit(&ADC_CommonInitStructure);

	// Channel-specific settings
	ADC_InitStructure.ADC_Resolution = ADC_Resolution_12b;
	ADC_InitStructure.ADC_ScanConvMode = ENABLE;
	ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;
	ADC_InitStructure.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_Falling;
	ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_T2_CC2;
	ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
	ADC_InitStructure.ADC_NbrOfConversion = HW_ADC_NBR_CONV;

	ADC_Init(ADC1, &ADC_InitStructure);
	ADC_InitStructure.ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None;
	ADC_InitStructure.ADC_ExternalTrigConv = 0;
	ADC_Init(ADC2, &ADC_InitStructure);
	ADC_Init(ADC3, &ADC_InitStructure);

	ADC_TempSensorVrefintCmd(ENABLE);
	ADC_MultiModeDMARequestAfterLastTransferCmd(ENABLE);

	hw_setup_adc_channels();

	ADC_Cmd(ADC1, ENABLE);
	ADC_Cmd(ADC2, ENABLE);
	ADC_Cmd(ADC3, ENABLE);

	timer_reinit((int)m_motor_1.m_conf->foc_f_sw);

	stop_pwm_hw(&m_motor_1);
#ifdef HW_HAS_DUAL_MOTORS
	stop_pwm_hw(&m_motor_2);
#endif

	// Sample intervals. For now they are fixed with voltage samples in the center of V7
	// and current samples in the center of V0
	TIMER_UPDATE_SAMP(MCPWM_FOC_CURRENT_SAMP_OFFSET);

	// Enable CC2 interrupt, which will be fired in V0 and V7
	TIM_ITConfig(TIM2, TIM_IT_CC2, ENABLE);
	nvicEnableVector(TIM2_IRQn, 6);

	utils_sys_unlock_cnt();

	CURRENT_FILTER_ON();

	// Calibrate current offset
	ENABLE_GATE();
	DCCAL_OFF();
	do_dc_cal();

	// Start threads
	timer_thd_stop = false;
	chThdCreateStatic(timer_thread_wa, sizeof(timer_thread_wa), NORMALPRIO, timer_thread, NULL);

	hfi_thd_stop = false;
	chThdCreateStatic(hfi_thread_wa, sizeof(hfi_thread_wa), NORMALPRIO, hfi_thread, NULL);

	// Check if the system has resumed from IWDG reset
	if (timeout_had_IWDG_reset()) {
		mc_interface_fault_stop(FAULT_CODE_BOOTING_FROM_WATCHDOG_RESET, false, false);
	}

	terminal_register_command_callback(
			"foc_plot_hfi_en",
			"Enable HFI plotting. 0: off, 1: DFT, 2: Raw",
			"[en]",
			terminal_plot_hfi);

	m_init_done = true;
}

void mcpwm_foc_deinit(void) {
	if (!m_init_done) {
		return;
	}

	m_init_done = false;

	timer_thd_stop = true;
	while (timer_thd_stop) {
		chThdSleepMilliseconds(1);
	}

	hfi_thd_stop = true;
	while (hfi_thd_stop) {
		chThdSleepMilliseconds(1);
	}

	TIM_DeInit(TIM1);
	TIM_DeInit(TIM2);
	TIM_DeInit(TIM8);
	ADC_DeInit();
	DMA_DeInit(DMA2_Stream4);
	nvicDisableVector(ADC_IRQn);
	dmaStreamRelease(STM32_DMA_STREAM(STM32_DMA_STREAM_ID(2, 4)));
}

static volatile motor_all_state_t *motor_now(void) {
#ifdef HW_HAS_DUAL_MOTORS
	return mc_interface_motor_now() == 1 ? &m_motor_1 : &m_motor_2;
#else
	return &m_motor_1;
#endif
}

bool mcpwm_foc_init_done(void) {
	return m_init_done;
}

void mcpwm_foc_set_configuration(volatile mc_configuration *configuration) {
	motor_now()->m_conf = configuration;

	// Below we check if anything in the configuration changed that requires stopping the motor.

	uint32_t top = SYSTEM_CORE_CLOCK / (int)configuration->foc_f_sw;
	if (TIM1->ARR != top) {
#ifdef HW_HAS_DUAL_MOTORS
		m_motor_1.m_control_mode = CONTROL_MODE_NONE;
		m_motor_1.m_state = MC_STATE_OFF;
		stop_pwm_hw(&m_motor_1);

		m_motor_2.m_control_mode = CONTROL_MODE_NONE;
		m_motor_2.m_state = MC_STATE_OFF;
		stop_pwm_hw(&m_motor_2);

		timer_reinit((int)configuration->foc_f_sw);
#else
		motor_now()->m_control_mode = CONTROL_MODE_NONE;
		motor_now()->m_state = MC_STATE_OFF;
		stop_pwm_hw(motor_now());
		TIMER_UPDATE_SAMP_TOP_M1(MCPWM_FOC_CURRENT_SAMP_OFFSET, top);
#ifdef  HW_HAS_DUAL_PARALLEL
		TIMER_UPDATE_SAMP_TOP_M2(MCPWM_FOC_CURRENT_SAMP_OFFSET, top);
#endif
#endif
	}

	if (((1 << motor_now()->m_conf->foc_hfi_samples) * 8) != motor_now()->m_hfi.samples) {
		motor_now()->m_control_mode = CONTROL_MODE_NONE;
		motor_now()->m_state = MC_STATE_OFF;
		stop_pwm_hw(motor_now());
		update_hfi_samples(motor_now()->m_conf->foc_hfi_samples, motor_now());
	}
}

mc_state mcpwm_foc_get_state(void) {
	return motor_now()->m_state;
}

bool mcpwm_foc_is_dccal_done(void) {
	return m_dccal_done;
}

/**
 * Get the current motor used in the mcpwm ISR
 *
 * @return
 * 0: Not in ISR
 * 1: Motor 1
 * 2: Motor 2
 */
int mcpwm_foc_isr_motor(void) {
	return m_isr_motor;
}

/**
 * Switch off all FETs.
 */
void mcpwm_foc_stop_pwm(bool is_second_motor) {
#ifdef HW_HAS_DUAL_MOTORS
	volatile motor_all_state_t *motor = is_second_motor ? &m_motor_2 : &m_motor_1;
#else
	(void)is_second_motor;
	volatile motor_all_state_t *motor = &m_motor_1;
#endif

	motor->m_control_mode = CONTROL_MODE_NONE;
	motor->m_state = MC_STATE_OFF;
	stop_pwm_hw(motor);
}

/**
 * Use duty cycle control. Absolute values less than MCPWM_MIN_DUTY_CYCLE will
 * stop the motor.
 *
 * @param dutyCycle
 * The duty cycle to use.
 */
void mcpwm_foc_set_duty(float dutyCycle) {
	motor_now()->m_control_mode = CONTROL_MODE_DUTY;
	motor_now()->m_duty_cycle_set = dutyCycle;

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

/**
 * Use duty cycle control. Absolute values less than MCPWM_MIN_DUTY_CYCLE will
 * stop the motor.
 *
 * WARNING: This function does not use ramping. A too large step with a large motor
 * can destroy hardware.
 *
 * @param dutyCycle
 * The duty cycle to use.
 */
void mcpwm_foc_set_duty_noramp(float dutyCycle) {
	// TODO: Actually do this without ramping
	mcpwm_foc_set_duty(dutyCycle);
}

/**
 * Use PID rpm control. Note that this value has to be multiplied by half of
 * the number of motor poles.
 *
 * @param rpm
 * The electrical RPM goal value to use.
 */
void mcpwm_foc_set_pid_speed(float rpm) {
	motor_now()->m_control_mode = CONTROL_MODE_SPEED;
	motor_now()->m_speed_pid_set_rpm = rpm;

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

/**
 * Use PID position control. Note that this only works when encoder support
 * is enabled.
 *
 * @param pos
 * The desired position of the motor in degrees.
 */
void mcpwm_foc_set_pid_pos(float pos) {
	motor_now()->m_control_mode = CONTROL_MODE_POS;
	motor_now()->m_pos_pid_set = pos;

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

/**
 * Use current control and specify a goal current to use. The sign determines
 * the direction of the torque. Absolute values less than
 * conf->cc_min_current will release the motor.
 *
 * @param current
 * The current to use.
 */
void mcpwm_foc_set_current(float current) {
	if (fabsf(current) < motor_now()->m_conf->cc_min_current) {
		motor_now()->m_control_mode = CONTROL_MODE_NONE;
		motor_now()->m_state = MC_STATE_OFF;
		stop_pwm_hw(motor_now());
		return;
	}

	motor_now()->m_control_mode = CONTROL_MODE_CURRENT;
	motor_now()->m_iq_set = current;

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

/**
 * Brake the motor with a desired current. Absolute values less than
 * conf->cc_min_current will release the motor.
 *
 * @param current
 * The current to use. Positive and negative values give the same effect.
 */
void mcpwm_foc_set_brake_current(float current) {
	if (fabsf(current) < motor_now()->m_conf->cc_min_current) {
		motor_now()->m_control_mode = CONTROL_MODE_NONE;
		motor_now()->m_state = MC_STATE_OFF;
		stop_pwm_hw(motor_now());
		return;
	}

	motor_now()->m_control_mode = CONTROL_MODE_CURRENT_BRAKE;
	motor_now()->m_iq_set = current;

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

/**
 * Apply a fixed static current vector in open loop to emulate an electric
 * handbrake.
 *
 * @param current
 * The brake current to use.
 */
void mcpwm_foc_set_handbrake(float current) {
	if (fabsf(current) < motor_now()->m_conf->cc_min_current) {
		motor_now()->m_control_mode = CONTROL_MODE_NONE;
		motor_now()->m_state = MC_STATE_OFF;
		stop_pwm_hw(motor_now());
		return;
	}

	motor_now()->m_control_mode = CONTROL_MODE_HANDBRAKE;
	motor_now()->m_iq_set = current;

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

/**
 * Produce an openloop rotating current.
 *
 * @param current
 * The current to use.
 *
 * @param rpm
 * The RPM to use.
 */
void mcpwm_foc_set_openloop(float current, float rpm) {
	if (fabsf(current) < motor_now()->m_conf->cc_min_current) {
		motor_now()->m_control_mode = CONTROL_MODE_NONE;
		motor_now()->m_state = MC_STATE_OFF;
		stop_pwm_hw(motor_now());
		return;
	}

	utils_truncate_number(&current, -motor_now()->m_conf->l_current_max * motor_now()->m_conf->l_current_max_scale,
						  motor_now()->m_conf->l_current_max * motor_now()->m_conf->l_current_max_scale);

	motor_now()->m_control_mode = CONTROL_MODE_OPENLOOP;
	motor_now()->m_iq_set = current;
	motor_now()->m_openloop_speed = rpm * ((2.0 * M_PI) / 60.0);

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

/**
 * Produce an openloop current at a fixed phase.
 *
 * @param current
 * The current to use.
 *
 * @param phase
 * The phase to use in degrees, range [0.0 360.0]
 */
void mcpwm_foc_set_openloop_phase(float current, float phase) {
	if (fabsf(current) < motor_now()->m_conf->cc_min_current) {
		motor_now()->m_control_mode = CONTROL_MODE_NONE;
		motor_now()->m_state = MC_STATE_OFF;
		stop_pwm_hw(motor_now());
		return;
	}

	utils_truncate_number(&current, -motor_now()->m_conf->l_current_max * motor_now()->m_conf->l_current_max_scale,
						  motor_now()->m_conf->l_current_max * motor_now()->m_conf->l_current_max_scale);

	motor_now()->m_control_mode = CONTROL_MODE_OPENLOOP_PHASE;
	motor_now()->m_iq_set = current;

	motor_now()->m_openloop_phase = phase * M_PI / 180.0;
	utils_norm_angle_rad((float*)&motor_now()->m_openloop_phase);

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

/**
 * Set current offsets values,
 * this is used by the virtual motor to set the previously saved offsets back,
 * when it is disconnected
 */
void mcpwm_foc_set_current_offsets(volatile int curr0_offset,
								   volatile int curr1_offset,
								   volatile int curr2_offset) {
	motor_now()->m_curr_ofs[0] = curr0_offset;
	motor_now()->m_curr_ofs[1] = curr1_offset;
	motor_now()->m_curr_ofs[2] = curr2_offset;
}

/**
 * Produce an openloop rotating voltage.
 *
 * @param dutyCycle
 * The duty cycle to use.
 *
 * @param rpm
 * The RPM to use.
 */
void mcpwm_foc_set_openloop_duty(float dutyCycle, float rpm) {
	motor_now()->m_control_mode = CONTROL_MODE_OPENLOOP_DUTY;
	motor_now()->m_duty_cycle_set = dutyCycle;
	motor_now()->m_openloop_speed = rpm * ((2.0 * M_PI) / 60.0);

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

/**
 * Produce an openloop voltage at a fixed phase.
 *
 * @param dutyCycle
 * The duty cycle to use.
 *
 * @param phase
 * The phase to use in degrees, range [0.0 360.0]
 */
void mcpwm_foc_set_openloop_duty_phase(float dutyCycle, float phase) {
	motor_now()->m_control_mode = CONTROL_MODE_OPENLOOP_DUTY_PHASE;
	motor_now()->m_duty_cycle_set = dutyCycle;
	motor_now()->m_openloop_phase = phase * M_PI / 180.0;
	utils_norm_angle_rad((float*)&motor_now()->m_openloop_phase);

	if (motor_now()->m_state != MC_STATE_RUNNING) {
		motor_now()->m_state = MC_STATE_RUNNING;
	}
}

float mcpwm_foc_get_duty_cycle_set(void) {
	return motor_now()->m_duty_cycle_set;
}

float mcpwm_foc_get_duty_cycle_now(void) {
	return motor_now()->m_motor_state.duty_now;
}

float mcpwm_foc_get_pid_pos_set(void) {
	return motor_now()->m_pos_pid_set;
}

float mcpwm_foc_get_pid_pos_now(void) {
	return motor_now()->m_pos_pid_now;
}

/**
 * Get the current switching frequency.
 *
 * @return
 * The switching frequency in Hz.
 */
float mcpwm_foc_get_switching_frequency_now(void) {
	return motor_now()->m_conf->foc_f_sw;
}

/**
 * Get the current sampling frequency.
 *
 * @return
 * The sampling frequency in Hz.
 */
float mcpwm_foc_get_sampling_frequency_now(void) {
#ifdef HW_HAS_PHASE_SHUNTS
	if (motor_now()->m_conf->foc_sample_v0_v7) {
		return motor_now()->m_conf->foc_f_sw;
	} else {
		return motor_now()->m_conf->foc_f_sw / 2.0;
	}
#else
	return motor_now()->m_conf->foc_f_sw / 2.0;
#endif
}

/**
 * Returns Ts used for virtual motor sync
 */
float mcpwm_foc_get_ts(void) {
#ifdef HW_HAS_PHASE_SHUNTS
	if (motor_now()->m_conf->foc_sample_v0_v7) {
		return (1.0 / motor_now()->m_conf->foc_f_sw) ;
	} else {
		return (1.0 / (motor_now()->m_conf->foc_f_sw / 2.0));
	}
#else
	return (1.0 / motor_now()->m_conf->foc_f_sw) ;
#endif
}

bool mcpwm_foc_is_using_encoder(void) {
	return motor_now()->m_using_encoder;
}

float mcpwm_foc_get_tot_current_motor(bool is_second_motor) {
#ifdef HW_HAS_DUAL_MOTORS
	volatile motor_all_state_t *motor = is_second_motor ? &m_motor_2 : &m_motor_1;
	return SIGN(motor->m_motor_state.vq) * motor->m_motor_state.iq;
#else
	(void)is_second_motor;
	return SIGN(m_motor_1.m_motor_state.vq) * m_motor_1.m_motor_state.iq;
#endif
}

float mcpwm_foc_get_tot_current_filtered_motor(bool is_second_motor) {
#ifdef HW_HAS_DUAL_MOTORS
	volatile motor_all_state_t *motor = is_second_motor ? &m_motor_2 : &m_motor_1;
	return SIGN(motor->m_motor_state.vq) * motor->m_motor_state.iq_filter;
#else
	(void)is_second_motor;
	return SIGN(m_motor_1.m_motor_state.vq) * m_motor_1.m_motor_state.iq_filter;
#endif
}

float mcpwm_foc_get_tot_current_in_motor(bool is_second_motor) {
#ifdef HW_HAS_DUAL_MOTORS
	return (is_second_motor ? &m_motor_2 : &m_motor_1)->m_motor_state.i_bus;
#else
	(void)is_second_motor;
	return m_motor_1.m_motor_state.i_bus;
#endif
}

float mcpwm_foc_get_tot_current_in_filtered_motor(bool is_second_motor) {
	// TODO: Filter current?
#ifdef HW_HAS_DUAL_MOTORS
	return (is_second_motor ? &m_motor_2 : &m_motor_1)->m_motor_state.i_bus;
#else
	(void)is_second_motor;
	return m_motor_1.m_motor_state.i_bus;
#endif
}

float mcpwm_foc_get_abs_motor_current_motor(bool is_second_motor) {
#ifdef HW_HAS_DUAL_MOTORS
	return (is_second_motor ? &m_motor_2 : &m_motor_1)->m_motor_state.i_abs;
#else
	(void)is_second_motor;
	return m_motor_1.m_motor_state.i_abs;
#endif
}

float mcpwm_foc_get_abs_motor_current_filtered_motor(bool is_second_motor) {
#ifdef HW_HAS_DUAL_MOTORS
	return (is_second_motor ? &m_motor_2 : &m_motor_1)->m_motor_state.i_abs_filter;
#else
	(void)is_second_motor;
	return m_motor_1.m_motor_state.i_abs_filter;
#endif
}

mc_state mcpwm_foc_get_state_motor(bool is_second_motor) {
#ifdef HW_HAS_DUAL_MOTORS
	return (is_second_motor ? &m_motor_2 : &m_motor_1)->m_state;
#else
	(void)is_second_motor;
	return m_motor_1.m_state;
#endif
}

/**
 * Calculate the current RPM of the motor. This is a signed value and the sign
 * depends on the direction the motor is rotating in. Note that this value has
 * to be divided by half the number of motor poles.
 *
 * @return
 * The RPM value.
 */
float mcpwm_foc_get_rpm(void) {
	return motor_now()->m_motor_state.speed_rad_s / ((2.0 * M_PI) / 60.0);
	//	return motor_now()->m_speed_est_fast / ((2.0 * M_PI) / 60.0);
}

/**
 * Same as above, but uses the fast and noisier estimator.
 */
float mcpwm_foc_get_rpm_fast(void) {
	return motor_now()->m_speed_est_fast / ((2.0 * M_PI) / 60.0);
}

/**
 * Same as above, but uses the faster and noisier estimator.
 */
float mcpwm_foc_get_rpm_faster(void) {
	return motor_now()->m_speed_est_faster / ((2.0 * M_PI) / 60.0);
}

/**
 * Get the motor current. The sign of this value will
 * represent whether the motor is drawing (positive) or generating
 * (negative) current. This is the q-axis current which produces torque.
 *
 * @return
 * The motor current.
 */
float mcpwm_foc_get_tot_current(void) {
	return SIGN(motor_now()->m_motor_state.vq) * motor_now()->m_motor_state.iq;
}

/**
 * Get the filtered motor current. The sign of this value will
 * represent whether the motor is drawing (positive) or generating
 * (negative) current. This is the q-axis current which produces torque.
 *
 * @return
 * The filtered motor current.
 */
float mcpwm_foc_get_tot_current_filtered(void) {
	return SIGN(motor_now()->m_motor_state.vq) * motor_now()->m_motor_state.iq_filter;
}

/**
 * Get the magnitude of the motor current, which includes both the
 * D and Q axis.
 *
 * @return
 * The magnitude of the motor current.
 */
float mcpwm_foc_get_abs_motor_current(void) {
	return motor_now()->m_motor_state.i_abs;
}

/**
 * Get the magnitude of the motor current unbalance
 *
 * @return
 * The magnitude of the phase currents unbalance.
 */
float mcpwm_foc_get_abs_motor_current_unbalance(void) {
	return (float)(motor_now()->m_curr_unbalance) * FAC_CURRENT;
}

/**
 * Get the magnitude of the motor voltage.
 *
 * @return
 * The magnitude of the motor voltage.
 */
float mcpwm_foc_get_abs_motor_voltage(void) {
	const float vd_tmp = motor_now()->m_motor_state.vd;
	const float vq_tmp = motor_now()->m_motor_state.vq;
	return sqrtf(SQ(vd_tmp) + SQ(vq_tmp));
}

/**
 * Get the filtered magnitude of the motor current, which includes both the
 * D and Q axis.
 *
 * @return
 * The magnitude of the motor current.
 */
float mcpwm_foc_get_abs_motor_current_filtered(void) {
	return motor_now()->m_motor_state.i_abs_filter;
}

/**
 * Get the motor current. The sign of this value represents the direction
 * in which the motor generates torque.
 *
 * @return
 * The motor current.
 */
float mcpwm_foc_get_tot_current_directional(void) {
	return motor_now()->m_motor_state.iq;
}

/**
 * Get the filtered motor current. The sign of this value represents the
 * direction in which the motor generates torque.
 *
 * @return
 * The filtered motor current.
 */
float mcpwm_foc_get_tot_current_directional_filtered(void) {
	return motor_now()->m_motor_state.iq_filter;
}

/**
 * Get the direct axis motor current.
 *
 * @return
 * The D axis current.
 */
float mcpwm_foc_get_id(void) {
	return motor_now()->m_motor_state.id;
}

/**
 * Get the quadrature axis motor current.
 *
 * @return
 * The Q axis current.
 */
float mcpwm_foc_get_iq(void) {
	return motor_now()->m_motor_state.iq;
}

/**
 * Get the input current to the motor controller.
 *
 * @return
 * The input current.
 */
float mcpwm_foc_get_tot_current_in(void) {
	return motor_now()->m_motor_state.i_bus;
}

/**
 * Get the filtered input current to the motor controller.
 *
 * @return
 * The filtered input current.
 */
float mcpwm_foc_get_tot_current_in_filtered(void) {
	return motor_now()->m_motor_state.i_bus; // TODO: Calculate filtered current?
}

/**
 * Set the number of steps the motor has rotated. This number is signed and
 * becomes a negative when the motor is rotating backwards.
 *
 * @param steps
 * New number of steps will be set after this call.
 *
 * @return
 * The previous tachometer value in motor steps. The number of motor revolutions will
 * be this number divided by (3 * MOTOR_POLE_NUMBER).
 */
int mcpwm_foc_set_tachometer_value(int steps) {
	int val = motor_now()->m_tachometer;
	motor_now()->m_tachometer = steps;
	return val;
}

/**
 * Read the number of steps the motor has rotated. This number is signed and
 * will return a negative number when the motor is rotating backwards.
 *
 * @param reset
 * If true, the tachometer counter will be reset after this call.
 *
 * @return
 * The tachometer value in motor steps. The number of motor revolutions will
 * be this number divided by (3 * MOTOR_POLE_NUMBER).
 */
int mcpwm_foc_get_tachometer_value(bool reset) {
	int val = motor_now()->m_tachometer;

	if (reset) {
		motor_now()->m_tachometer = 0;
	}

	return val;
}

/**
 * Read the absolute number of steps the motor has rotated.
 *
 * @param reset
 * If true, the tachometer counter will be reset after this call.
 *
 * @return
 * The tachometer value in motor steps. The number of motor revolutions will
 * be this number divided by (3 * MOTOR_POLE_NUMBER).
 */
int mcpwm_foc_get_tachometer_abs_value(bool reset) {
	int val = motor_now()->m_tachometer_abs;

	if (reset) {
		motor_now()->m_tachometer_abs = 0;
	}

	return val;
}

/**
 * Read the motor phase.
 *
 * @return
 * The phase angle in degrees.
 */
float mcpwm_foc_get_phase(void) {
	float angle = motor_now()->m_motor_state.phase * (180.0 / M_PI);
	utils_norm_angle(&angle);
	return angle;
}

/**
 * Read the phase that the observer has calculated.
 *
 * @return
 * The phase angle in degrees.
 */
float mcpwm_foc_get_phase_observer(void) {
	float angle = motor_now()->m_phase_now_observer * (180.0 / M_PI);
	utils_norm_angle(&angle);
	return angle;
}

/**
 * Read the phase from based on the encoder.
 *
 * @return
 * The phase angle in degrees.
 */
float mcpwm_foc_get_phase_encoder(void) {
	float angle = motor_now()->m_phase_now_encoder * (180.0 / M_PI);
	utils_norm_angle(&angle);
	return angle;
}

float mcpwm_foc_get_vd(void) {
	return motor_now()->m_motor_state.vd;
}

float mcpwm_foc_get_vq(void) {
	return motor_now()->m_motor_state.vq;
}

/**
 * Get current offsets,
 * this is used by the virtual motor to save the current offsets,
 * when it is connected
 */
void mcpwm_foc_get_current_offsets(
		volatile int *curr0_offset,
		volatile int *curr1_offset,
		volatile int *curr2_offset,
		bool is_second_motor) {
#ifdef HW_HAS_DUAL_MOTORS
	volatile motor_all_state_t *motor = is_second_motor ? &m_motor_2 : &m_motor_1;
#else
	(void)is_second_motor;
	volatile motor_all_state_t *motor = &m_motor_1;
#endif
	*curr0_offset = motor->m_curr_ofs[0];
	*curr1_offset = motor->m_curr_ofs[1];
	*curr2_offset = motor->m_curr_ofs[2];
}

/**
 * Measure encoder offset and direction.
 *
 * @param current
 * The locking open loop current for the motor.
 *
 * @param offset
 * The detected offset.
 *
 * @param ratio
 * The ratio between electrical and mechanical revolutions
 *
 * @param direction
 * The detected direction.
 */
void mcpwm_foc_encoder_detect(float current, bool print, float *offset, float *ratio, bool *inverted) {
	mc_interface_lock();

	volatile motor_all_state_t *motor = motor_now();

	motor->m_phase_override = true;
	motor->m_id_set = current;
	motor->m_iq_set = 0.0;
	motor->m_control_mode = CONTROL_MODE_CURRENT;
	motor->m_state = MC_STATE_RUNNING;

	// Disable timeout
	systime_t tout = timeout_get_timeout_msec();
	float tout_c = timeout_get_brake_current();
	timeout_reset();
	timeout_configure(600000, 0.0);

	// Save configuration
	float offset_old = motor->m_conf->foc_encoder_offset;
	float inverted_old = motor->m_conf->foc_encoder_inverted;
	float ratio_old = motor->m_conf->foc_encoder_ratio;

	motor->m_conf->foc_encoder_offset = 0.0;
	motor->m_conf->foc_encoder_inverted = false;
	motor->m_conf->foc_encoder_ratio = 1.0;

	// Find index
	int cnt = 0;
	while(!encoder_index_found()) {
		for (float i = 0.0;i < 2.0 * M_PI;i += (2.0 * M_PI) / 500.0) {
			motor->m_phase_now_override = i;
			chThdSleepMilliseconds(1);
		}

		cnt++;
		if (cnt > 30) {
			// Give up
			break;
		}
	}

	if (print) {
		commands_printf("Index found");
	}

	// Rotate
	for (float i = 0.0;i < 2.0 * M_PI;i += (2.0 * M_PI) / 500.0) {
		motor->m_phase_now_override = i;
		chThdSleepMilliseconds(1);
	}

	if (print) {
		commands_printf("Rotated for sync");
	}

	// Inverted and ratio
	chThdSleepMilliseconds(1000);

	const int it_rat = 20;
	float s_sum = 0.0;
	float c_sum = 0.0;
	float first = motor->m_phase_now_encoder;

	for (int i = 0; i < it_rat; i++) {
		float phase_old = motor->m_phase_now_encoder;
		float phase_ovr_tmp = motor->m_phase_now_override;
		for (float j = phase_ovr_tmp; j < phase_ovr_tmp + (2.0 / 3.0) * M_PI;
				j += (2.0 * M_PI) / 500.0) {
			motor->m_phase_now_override = j;
			chThdSleepMilliseconds(1);
		}
		utils_norm_angle_rad((float*)&motor->m_phase_now_override);
		chThdSleepMilliseconds(300);
		float diff = utils_angle_difference_rad(motor->m_phase_now_encoder, phase_old);

		float s, c;
		sincosf(diff, &s, &c);
		s_sum += s;
		c_sum += c;

		if (print) {
			commands_printf("%.2f", (double)(diff * 180.0 / M_PI));
		}

		if (i > 3 && fabsf(utils_angle_difference_rad(motor->m_phase_now_encoder, first)) < fabsf(diff / 2.0)) {
			break;
		}
	}

	first = motor->m_phase_now_encoder;

	for (int i = 0; i < it_rat; i++) {
		float phase_old = motor->m_phase_now_encoder;
		float phase_ovr_tmp = motor->m_phase_now_override;
		for (float j = phase_ovr_tmp; j > phase_ovr_tmp - (2.0 / 3.0) * M_PI;
				j -= (2.0 * M_PI) / 500.0) {
			motor->m_phase_now_override = j;
			chThdSleepMilliseconds(1);
		}
		utils_norm_angle_rad((float*)&motor->m_phase_now_override);
		chThdSleepMilliseconds(300);
		float diff = utils_angle_difference_rad(phase_old, motor->m_phase_now_encoder);

		float s, c;
		sincosf(diff, &s, &c);
		s_sum += s;
		c_sum += c;

		if (print) {
			commands_printf("%.2f", (double)(diff * 180.0 / M_PI));
		}

		if (i > 3 && fabsf(utils_angle_difference_rad(motor->m_phase_now_encoder, first)) < fabsf(diff / 2.0)) {
			break;
		}
	}

	float diff = atan2f(s_sum, c_sum) * 180.0 / M_PI;
	*inverted = diff < 0.0;
	*ratio = roundf(((2.0 / 3.0) * 180.0) / fabsf(diff));

	motor->m_conf->foc_encoder_inverted = *inverted;
	motor->m_conf->foc_encoder_ratio = *ratio;

	if (print) {
		commands_printf("Inversion and ratio detected");
	}

	// Rotate
	for (float i = motor->m_phase_now_override;i < 2.0 * M_PI;i += (2.0 * M_PI) / 500.0) {
		motor->m_phase_now_override = i;
		chThdSleepMilliseconds(2);
	}

	if (print) {
		commands_printf("Rotated for sync");
		commands_printf("Enc: %.2f", (double)encoder_read_deg());
	}

	const int it_ofs = motor->m_conf->foc_encoder_ratio * 3.0;
	s_sum = 0.0;
	c_sum = 0.0;

	for (int i = 0;i < it_ofs;i++) {
		float step = (2.0 * M_PI * motor->m_conf->foc_encoder_ratio) / ((float)it_ofs);
		float override = (float)i * step;

		while (motor->m_phase_now_override != override) {
			utils_step_towards((float*)&motor->m_phase_now_override, override, step / 100.0);
			chThdSleepMilliseconds(4);
		}

		chThdSleepMilliseconds(100);

		float angle_diff = utils_angle_difference_rad(motor->m_phase_now_encoder, motor->m_phase_now_override);
		float s, c;
		sincosf(angle_diff, &s, &c);
		s_sum += s;
		c_sum += c;

		if (print) {
			commands_printf("%.2f", (double)(angle_diff * 180.0 / M_PI));
		}
	}

	for (int i = it_ofs;i > 0;i--) {
		float step = (2.0 * M_PI * motor->m_conf->foc_encoder_ratio) / ((float)it_ofs);
		float override = (float)i * step;

		while (motor->m_phase_now_override != override) {
			utils_step_towards((float*)&motor->m_phase_now_override, override, step / 100.0);
			chThdSleepMilliseconds(4);
		}

		chThdSleepMilliseconds(100);

		float angle_diff = utils_angle_difference_rad(motor->m_phase_now_encoder, motor->m_phase_now_override);
		float s, c;
		sincosf(angle_diff, &s, &c);
		s_sum += s;
		c_sum += c;

		if (print) {
			commands_printf("%.2f", (double)(angle_diff * 180.0 / M_PI));
		}
	}

	*offset = atan2f(s_sum, c_sum) * 180.0 / M_PI;

	if (print) {
		commands_printf("Avg: %.2f", (double)*offset);
	}

	utils_norm_angle(offset);

	if (print) {
		commands_printf("Offset detected");
	}

	motor->m_id_set = 0.0;
	motor->m_iq_set = 0.0;
	motor->m_phase_override = false;
	motor->m_control_mode = CONTROL_MODE_NONE;
	motor->m_state = MC_STATE_OFF;
	stop_pwm_hw(motor);

	// Restore configuration
	motor->m_conf->foc_encoder_inverted = inverted_old;
	motor->m_conf->foc_encoder_offset = offset_old;
	motor->m_conf->foc_encoder_ratio = ratio_old;

	// Enable timeout
	timeout_configure(tout, tout_c);

	mc_interface_unlock();
}

/**
 * Lock the motor with a current and sample the voiltage and current to
 * calculate the motor resistance.
 *
 * @param current
 * The locking current.
 *
 * @param samples
 * The number of samples to take.
 *
 * @param stop_after
 * Stop motor after finishing the measurement. Otherwise, the current will
 * still be applied after returning. Setting this to false is useful if you want
 * to run this function again right away, without stopping the motor in between.
 *
 * @return
 * The calculated motor resistance.
 */
float mcpwm_foc_measure_resistance(float current, int samples, bool stop_after) {
	mc_interface_lock();

	volatile motor_all_state_t *motor = motor_now();

	motor->m_phase_override = true;
	motor->m_phase_now_override = 0.0;
	motor->m_id_set = 0.0;
	motor->m_control_mode = CONTROL_MODE_CURRENT;
	motor->m_state = MC_STATE_RUNNING;

	// Disable timeout
	systime_t tout = timeout_get_timeout_msec();
	float tout_c = timeout_get_brake_current();
	timeout_reset();
	timeout_configure(60000, 0.0);

	// Ramp up the current slowly
	while (fabsf(motor->m_iq_set - current) > 0.001) {
		utils_step_towards((float*)&motor->m_iq_set, current, fabsf(current) / 500.0);
		chThdSleepMilliseconds(1);
	}

	// Wait for the current to rise and the motor to lock.
	chThdSleepMilliseconds(100);

	// Sample
	motor->m_samples.avg_current_tot = 0.0;
	motor->m_samples.avg_voltage_tot = 0.0;
	motor->m_samples.sample_num = 0;

	int cnt = 0;
	while (motor->m_samples.sample_num < samples) {
		chThdSleepMilliseconds(1);
		cnt++;
		// Timeout
		if (cnt > 10000) {
			break;
		}

		if (mc_interface_get_fault() != FAULT_CODE_NONE) {
			motor->m_id_set = 0.0;
			motor->m_iq_set = 0.0;
			motor->m_phase_override = false;
			motor->m_control_mode = CONTROL_MODE_NONE;
			motor->m_state = MC_STATE_OFF;
			stop_pwm_hw(motor);

			timeout_configure(tout, tout_c);
			mc_interface_unlock();

			return 0.0;
		}
	}

	const float current_avg = motor->m_samples.avg_current_tot / (float)motor->m_samples.sample_num;
	const float voltage_avg = motor->m_samples.avg_voltage_tot / (float)motor->m_samples.sample_num;

	// Stop
	if (stop_after) {
		motor->m_id_set = 0.0;
		motor->m_iq_set = 0.0;
		motor->m_phase_override = false;
		motor->m_control_mode = CONTROL_MODE_NONE;
		motor->m_state = MC_STATE_OFF;
		stop_pwm_hw(motor);
	}

	// Enable timeout
	timeout_configure(tout, tout_c);
	mc_interface_unlock();

	return (voltage_avg / current_avg) * (2.0 / 3.0);
}

/**
 * Measure the motor inductance with short voltage pulses.
 *
 * @param duty
 * The duty cycle to use in the pulses.
 *
 * @param samples
 * The number of samples to average over.
 *
 * @param
 * The current that was used for this measurement.
 *
 * @return
 * The average d and q axis inductance in uH.
 */
float mcpwm_foc_measure_inductance(float duty, int samples, float *curr, float *ld_lq_diff) {
	volatile motor_all_state_t *motor = motor_now();

	mc_sensor_mode sensor_mode_old = motor->m_conf->sensor_mode;
	float f_sw_old = motor->m_conf->foc_f_sw;
	float hfi_voltage_start_old = motor->m_conf->foc_hfi_voltage_start;
	float hfi_voltage_run_old = motor->m_conf->foc_hfi_voltage_run;
	float hfi_voltage_max_old = motor->m_conf->foc_hfi_voltage_max;
	bool sample_v0_v7_old = motor->m_conf->foc_sample_v0_v7;
	foc_hfi_samples samples_old = motor->m_conf->foc_hfi_samples;
	bool sample_high_current_old = motor->m_conf->foc_sample_high_current;

	mc_interface_lock();
	motor->m_control_mode = CONTROL_MODE_NONE;
	motor->m_state = MC_STATE_OFF;
	stop_pwm_hw(motor);

	motor->m_conf->foc_sensor_mode = FOC_SENSOR_MODE_HFI;
	motor->m_conf->foc_hfi_voltage_start = duty * GET_INPUT_VOLTAGE() * (2.0 / 3.0);
	motor->m_conf->foc_hfi_voltage_run = duty * GET_INPUT_VOLTAGE() * (2.0 / 3.0);
	motor->m_conf->foc_hfi_voltage_max = duty * GET_INPUT_VOLTAGE() * (2.0 / 3.0);
	motor->m_conf->foc_sample_v0_v7 = false;
	motor->m_conf->foc_hfi_samples = HFI_SAMPLES_32;
	motor->m_conf->foc_sample_high_current = false;

	update_hfi_samples(motor->m_conf->foc_hfi_samples, motor);

	chThdSleepMilliseconds(1);

	timeout_reset();
	mcpwm_foc_set_duty(0.0);
	chThdSleepMilliseconds(1);

	int ready_cnt = 0;
	while (!motor->m_hfi.ready) {
		chThdSleepMilliseconds(1);
		ready_cnt++;
		if (ready_cnt > 100) {
			break;
		}
	}

	if (samples < 10) {
		samples = 10;
	}

	float l_sum = 0.0;
	float ld_lq_diff_sum = 0.0;
	float i_sum = 0.0;
	float iterations = 0.0;

	for (int i = 0;i < (samples / 10);i++) {
		if (mc_interface_get_fault() != FAULT_CODE_NONE) {
			motor->m_id_set = 0.0;
			motor->m_iq_set = 0.0;
			motor->m_control_mode = CONTROL_MODE_NONE;
			motor->m_state = MC_STATE_OFF;
			stop_pwm_hw(motor);

			motor->m_conf->foc_sensor_mode = sensor_mode_old;
			motor->m_conf->foc_f_sw = f_sw_old;
			motor->m_conf->foc_hfi_voltage_start = hfi_voltage_start_old;
			motor->m_conf->foc_hfi_voltage_run = hfi_voltage_run_old;
			motor->m_conf->foc_hfi_voltage_max = hfi_voltage_max_old;
			motor->m_conf->foc_sample_v0_v7 = sample_v0_v7_old;
			motor->m_conf->foc_hfi_samples = samples_old;
			motor->m_conf->foc_sample_high_current = sample_high_current_old;

			update_hfi_samples(motor->m_conf->foc_hfi_samples, motor);

			mc_interface_unlock();

			return 0.0;
		}

		timeout_reset();
		mcpwm_foc_set_duty(0.0);
		chThdSleepMilliseconds(10);

		float real_bin0, imag_bin0;
		float real_bin2, imag_bin2;
		float real_bin0_i, imag_bin0_i;

		motor->m_hfi.fft_bin0_func((float*)motor->m_hfi.buffer, &real_bin0, &imag_bin0);
		motor->m_hfi.fft_bin2_func((float*)motor->m_hfi.buffer, &real_bin2, &imag_bin2);
		motor->m_hfi.fft_bin0_func((float*)motor->m_hfi.buffer_current, &real_bin0_i, &imag_bin0_i);

		l_sum += real_bin0;
		ld_lq_diff_sum += 2.0 * sqrtf(SQ(real_bin2) + SQ(imag_bin2));
		i_sum += real_bin0_i;

		iterations++;
	}

	mcpwm_foc_set_current(0.0);

	motor->m_conf->foc_sensor_mode = sensor_mode_old;
	motor->m_conf->foc_f_sw = f_sw_old;
	motor->m_conf->foc_hfi_voltage_start = hfi_voltage_start_old;
	motor->m_conf->foc_hfi_voltage_run = hfi_voltage_run_old;
	motor->m_conf->foc_hfi_voltage_max = hfi_voltage_max_old;
	motor->m_conf->foc_sample_v0_v7 = sample_v0_v7_old;
	motor->m_conf->foc_hfi_samples = samples_old;
	motor->m_conf->foc_sample_high_current = sample_high_current_old;

	update_hfi_samples(motor->m_conf->foc_hfi_samples, motor);

	mc_interface_unlock();

	if (curr) {
		*curr = i_sum / iterations;
	}

	if (ld_lq_diff) {
		*ld_lq_diff = (ld_lq_diff_sum / iterations) * 1e6 * (2.0 / 3.0);
	}

	return (l_sum / iterations) * 1e6 * (2.0 / 3.0);
}

/**
 * Measure the motor inductance with short voltage pulses. The difference from the
 * other function is that this one will aim for a specific measurement current. It
 * will also use an appropriate switching frequency.
 *
 * @param curr_goal
 * The measurement current to aim for.
 *
 * @param samples
 * The number of samples to average over.
 *
 * @param *curr
 * The current that was used for this measurement.
 *
 * @return
 * The average d and q axis inductance in uH.
 */
float mcpwm_foc_measure_inductance_current(float curr_goal, int samples, float *curr, float *ld_lq_diff) {
	float duty_last = 0.0;
	for (float i = 0.02;i < 0.5;i *= 1.5) {
		float i_tmp;
		mcpwm_foc_measure_inductance(i, 10, &i_tmp, 0);

		duty_last = i;
		if (i_tmp >= curr_goal) {
			break;
		}
	}

	float ind = mcpwm_foc_measure_inductance(duty_last, samples, curr, ld_lq_diff);
	return ind;
}

/**
 * Automatically measure the resistance and inductance of the motor with small steps.
 *
 * @param res
 * The measured resistance in ohm.
 *
 * @param ind
 * The measured inductance in microhenry.
 *
 * @return
 * True if the measurement succeeded, false otherwise.
 */
bool mcpwm_foc_measure_res_ind(float *res, float *ind) {
	volatile motor_all_state_t *motor = motor_now();

	const float f_sw_old = motor->m_conf->foc_f_sw;
	const float kp_old = motor->m_conf->foc_current_kp;
	const float ki_old = motor->m_conf->foc_current_ki;
	const float res_old = motor->m_conf->foc_motor_r;

	motor->m_conf->foc_current_kp = 0.001;
	motor->m_conf->foc_current_ki = 1.0;

	float i_last = 0.0;
	for (float i = 2.0;i < (motor->m_conf->l_current_max / 2.0);i *= 1.5) {
		if (i > (1.0 / mcpwm_foc_measure_resistance(i, 20, false))) {
			i_last = i;
			break;
		}
	}

	if (i_last < 0.01) {
		i_last = (motor->m_conf->l_current_max / 2.0);
	}

#ifdef HW_AXIOM_FORCE_HIGH_CURRENT_MEASUREMENTS
	i_last = (motor->m_conf->l_current_max / 2.0);
#endif

	*res = mcpwm_foc_measure_resistance(i_last, 200, true);
	motor->m_conf->foc_motor_r = *res;
	*ind = mcpwm_foc_measure_inductance_current(i_last, 200, 0, 0);

	motor->m_conf->foc_f_sw = f_sw_old;
	motor->m_conf->foc_current_kp = kp_old;
	motor->m_conf->foc_current_ki = ki_old;
	motor->m_conf->foc_motor_r = res_old;

	return true;
}

/**
 * Run the motor in open loop and figure out at which angles the hall sensors are.
 *
 * @param current
 * Current to use.
 *
 * @param hall_table
 * Table to store the result to.
 *
 * @return
 * true: Success
 * false: Something went wrong
 */
bool mcpwm_foc_hall_detect(float current, uint8_t *hall_table) {
	volatile motor_all_state_t *motor = motor_now();

	mc_interface_lock();

	motor->m_phase_override = true;
	motor->m_id_set = 0.0;
	motor->m_iq_set = 0.0;
	motor->m_control_mode = CONTROL_MODE_CURRENT;
	motor->m_state = MC_STATE_RUNNING;

	// Disable timeout
	systime_t tout = timeout_get_timeout_msec();
	float tout_c = timeout_get_brake_current();
	timeout_reset();
	timeout_configure(60000, 0.0);

	// Lock the motor
	motor->m_phase_now_override = 0;

	for (int i = 0;i < 1000;i++) {
		motor->m_id_set = (float)i * current / 1000.0;
		chThdSleepMilliseconds(1);
	}

	float sin_hall[8];
	float cos_hall[8];
	int hall_iterations[8];
	memset(sin_hall, 0, sizeof(sin_hall));
	memset(cos_hall, 0, sizeof(cos_hall));
	memset(hall_iterations, 0, sizeof(hall_iterations));

	// Forwards
	for (int i = 0;i < 3;i++) {
		for (int j = 0;j < 360;j++) {
			motor->m_phase_now_override = (float)j * M_PI / 180.0;
			chThdSleepMilliseconds(5);

			int hall = utils_read_hall(motor != &m_motor_1);
			float s, c;
			sincosf(motor->m_phase_now_override, &s, &c);
			sin_hall[hall] += s;
			cos_hall[hall] += c;
			hall_iterations[hall]++;
		}
	}

	// Reverse
	for (int i = 0;i < 3;i++) {
		for (int j = 360;j >= 0;j--) {
			motor->m_phase_now_override = (float)j * M_PI / 180.0;
			chThdSleepMilliseconds(5);

			int hall = utils_read_hall(motor != &m_motor_1);
			float s, c;
			sincosf(motor->m_phase_now_override, &s, &c);
			sin_hall[hall] += s;
			cos_hall[hall] += c;
			hall_iterations[hall]++;
		}
	}

	motor->m_id_set = 0.0;
	motor->m_iq_set = 0.0;
	motor->m_phase_override = false;
	motor->m_control_mode = CONTROL_MODE_NONE;
	motor->m_state = MC_STATE_OFF;
	stop_pwm_hw(motor);

	// Enable timeout
	timeout_configure(tout, tout_c);

	int fails = 0;
	for(int i = 0;i < 8;i++) {
		if (hall_iterations[i] > 30) {
			float ang = atan2f(sin_hall[i], cos_hall[i]) * 180.0 / M_PI;
			utils_norm_angle(&ang);
			hall_table[i] = (uint8_t)(ang * 200.0 / 360.0);
		} else {
			hall_table[i] = 255;
			fails++;
		}
	}

	mc_interface_unlock();

	return fails == 2;
}

void mcpwm_foc_print_state(void) {
	commands_printf("Mod d:        %.2f", (double)motor_now()->m_motor_state.mod_d);
	commands_printf("Mod q:        %.2f", (double)motor_now()->m_motor_state.mod_q);
	commands_printf("Duty:         %.2f", (double)motor_now()->m_motor_state.duty_now);
	commands_printf("Vd:           %.2f", (double)motor_now()->m_motor_state.vd);
	commands_printf("Vq:           %.2f", (double)motor_now()->m_motor_state.vq);
	commands_printf("Phase:        %.2f", (double)motor_now()->m_motor_state.phase);
	commands_printf("V_alpha:      %.2f", (double)motor_now()->m_motor_state.v_alpha);
	commands_printf("V_beta:       %.2f", (double)motor_now()->m_motor_state.v_beta);
	commands_printf("id:           %.2f", (double)motor_now()->m_motor_state.id);
	commands_printf("iq:           %.2f", (double)motor_now()->m_motor_state.iq);
	commands_printf("id_filter:    %.2f", (double)motor_now()->m_motor_state.id_filter);
	commands_printf("iq_filter:    %.2f", (double)motor_now()->m_motor_state.iq_filter);
	commands_printf("id_target:    %.2f", (double)motor_now()->m_motor_state.id_target);
	commands_printf("iq_target:    %.2f", (double)motor_now()->m_motor_state.iq_target);
	commands_printf("i_abs:        %.2f", (double)motor_now()->m_motor_state.i_abs);
	commands_printf("i_abs_filter: %.2f", (double)motor_now()->m_motor_state.i_abs_filter);
	commands_printf("Obs_x1:       %.2f", (double)motor_now()->m_observer_x1);
	commands_printf("Obs_x2:       %.2f", (double)motor_now()->m_observer_x2);
	commands_printf("vd_int:       %.2f", (double)motor_now()->m_motor_state.vd_int);
	commands_printf("vq_int:       %.2f", (double)motor_now()->m_motor_state.vq_int);
}

float mcpwm_foc_get_last_adc_isr_duration(void) {
	return m_last_adc_isr_duration;
}

void mcpwm_foc_tim_sample_int_handler(void) {
	if (m_init_done) {
		// Generate COM event here for synchronization
		TIM_GenerateEvent(TIM1, TIM_EventSource_COM);
		TIM_GenerateEvent(TIM8, TIM_EventSource_COM);

		virtual_motor_int_handler(
				m_motor_1.m_motor_state.v_alpha,
				m_motor_1.m_motor_state.v_beta);
	}
}

void mcpwm_foc_adc_int_handler(void *p, uint32_t flags) {
	(void)p;
	(void)flags;

	static int skip = 0;
	if (++skip == FOC_CONTROL_LOOP_FREQ_DIVIDER) {
		skip = 0;
	} else {
		return;
	}

	uint32_t t_start = timer_time_now();

	bool is_v7 = !(TIM1->CR1 & TIM_CR1_DIR);
	int norm_curr_ofs = 0;

#ifdef HW_HAS_DUAL_MOTORS
	bool is_second_motor = is_v7;
	norm_curr_ofs = is_second_motor ? 3 : 0;
	volatile motor_all_state_t *motor_now = is_second_motor ? &m_motor_2 : &m_motor_1;
	volatile motor_all_state_t *motor_other = is_second_motor ? &m_motor_1 : &m_motor_2;
	m_isr_motor = is_second_motor ? 2 : 1;
#ifdef HW_HAS_3_SHUNTS
	volatile TIM_TypeDef *tim = is_second_motor ? TIM8 : TIM1;
#endif
#else
	volatile motor_all_state_t *motor_other = &m_motor_1;
	volatile motor_all_state_t *motor_now = &m_motor_1;;
	m_isr_motor = 1;
#ifdef HW_HAS_3_SHUNTS
	volatile TIM_TypeDef *tim = TIM1;
#endif
#endif

	volatile mc_configuration *conf_now = motor_now->m_conf;

	if (motor_other->m_duty_next_set) {
		motor_other->m_duty_next_set = false;
#ifdef HW_HAS_DUAL_MOTORS
		if (is_second_motor) {
			TIMER_UPDATE_DUTY_M1(motor_other->m_duty1_next, motor_other->m_duty2_next, motor_other->m_duty3_next);
		} else {
			TIMER_UPDATE_DUTY_M2(motor_other->m_duty1_next, motor_other->m_duty2_next, motor_other->m_duty3_next);
		}
#else
		TIMER_UPDATE_DUTY_M1(motor_now->m_duty1_next, motor_now->m_duty2_next, motor_now->m_duty3_next);
#ifdef HW_HAS_DUAL_PARALLEL
		TIMER_UPDATE_DUTY_M2(motor_now->m_duty1_next, motor_now->m_duty2_next, motor_now->m_duty3_next);
#endif
#endif
	}

#ifndef HW_HAS_DUAL_MOTORS
#ifdef HW_HAS_PHASE_SHUNTS
	if (!conf_now->foc_sample_v0_v7 && is_v7) {
		return;
	}
#else
	if (is_v7) {
		return;
	}
#endif
#endif

	// Reset the watchdog
	timeout_feed_WDT(THREAD_MCPWM);

#ifdef AD2S1205_SAMPLE_GPIO
	// force a position sample in the AD2S1205 resolver IC (falling edge)
	palClearPad(AD2S1205_SAMPLE_GPIO, AD2S1205_SAMPLE_PIN);
#endif

#ifdef HW_HAS_DUAL_MOTORS
	int curr0 = 0;
	int curr1 = 0;

	if (is_second_motor) {
		curr0 = GET_CURRENT1_M2();
		curr1 = GET_CURRENT2_M2();
	} else {
		curr0 = GET_CURRENT1();
		curr1 = GET_CURRENT2();
	}
#else
	int curr0 = GET_CURRENT1();
	int curr1 = GET_CURRENT2();
#ifdef HW_HAS_DUAL_PARALLEL
	curr0 += GET_CURRENT1_M2();
	curr1 += GET_CURRENT2_M2();
#endif
#endif

#ifdef HW_HAS_3_SHUNTS
#ifdef HW_HAS_DUAL_MOTORS
	int curr2 = is_second_motor ? GET_CURRENT3_M2() : GET_CURRENT3();
#else
	int curr2 = GET_CURRENT3();
#ifdef HW_HAS_DUAL_PARALLEL
	curr2 += GET_CURRENT3_M2();
#endif
#endif
#endif

	motor_now->m_curr_sum[0] += curr0;
	motor_now->m_curr_sum[1] += curr1;
#ifdef HW_HAS_3_SHUNTS
	motor_now->m_curr_sum[2] += curr2;
#endif

	curr0 -= motor_now->m_curr_ofs[0];
	curr1 -= motor_now->m_curr_ofs[1];
#ifdef HW_HAS_3_SHUNTS
	curr2 -= motor_now->m_curr_ofs[2];
	motor_now->m_curr_unbalance = curr0 + curr1 + curr2;
#endif

	motor_now->m_curr_samples++;

	ADC_curr_norm_value[0 + norm_curr_ofs] = curr0;
	ADC_curr_norm_value[1 + norm_curr_ofs] = curr1;
#ifdef HW_HAS_3_SHUNTS
	ADC_curr_norm_value[2 + norm_curr_ofs] = curr2;
#else
	ADC_curr_norm_value[2 + norm_curr_ofs] = -(ADC_curr_norm_value[0] + ADC_curr_norm_value[1]);
#endif

	// Use the best current samples depending on the modulation state.
#ifdef HW_HAS_3_SHUNTS
	if (conf_now->foc_sample_high_current) {
		// High current sampling mode. Choose the lower currents to derive the highest one
		// in order to be able to measure higher currents.
		const float i0_abs = fabsf(ADC_curr_norm_value[0 + norm_curr_ofs]);
		const float i1_abs = fabsf(ADC_curr_norm_value[1 + norm_curr_ofs]);
		const float i2_abs = fabsf(ADC_curr_norm_value[2 + norm_curr_ofs]);

		if (i0_abs > i1_abs && i0_abs > i2_abs) {
			ADC_curr_norm_value[0 + norm_curr_ofs] = -(ADC_curr_norm_value[1 + norm_curr_ofs] + ADC_curr_norm_value[2 + norm_curr_ofs]);
		} else if (i1_abs > i0_abs && i1_abs > i2_abs) {
			ADC_curr_norm_value[1 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[2 + norm_curr_ofs]);
		} else if (i2_abs > i0_abs && i2_abs > i1_abs) {
			ADC_curr_norm_value[2 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[1 + norm_curr_ofs]);
		}
	} else {
#ifdef HW_HAS_PHASE_SHUNTS
		if (is_v7) {
			if (tim->CCR1 > 500 && tim->CCR2 > 500) {
				// Use the same 2 shunts on low modulation, as that will avoid jumps in the current reading.
				// This is especially important when using HFI.
				ADC_curr_norm_value[2 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[1 + norm_curr_ofs]);
			} else {
				if (tim->CCR1 < tim->CCR2 && tim->CCR1 < tim->CCR3) {
					ADC_curr_norm_value[0 + norm_curr_ofs] = -(ADC_curr_norm_value[1 + norm_curr_ofs] + ADC_curr_norm_value[2 + norm_curr_ofs]);
				} else if (tim->CCR2 < tim->CCR1 && tim->CCR2 < tim->CCR3) {
					ADC_curr_norm_value[1 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[2 + norm_curr_ofs]);
				} else if (tim->CCR3 < tim->CCR1 && tim->CCR3 < tim->CCR2) {
					ADC_curr_norm_value[2 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[1 + norm_curr_ofs]);
				}
			}
		} else {
			if (tim->CCR1 < (tim->ARR - 500) && tim->CCR2 < (tim->ARR - 500)) {
				// Use the same 2 shunts on low modulation, as that will avoid jumps in the current reading.
				// This is especially important when using HFI.
				ADC_curr_norm_value[2 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[1 + norm_curr_ofs]);
			} else {
				if (tim->CCR1 > tim->CCR2 && tim->CCR1 > tim->CCR3) {
					ADC_curr_norm_value[0 + norm_curr_ofs] = -(ADC_curr_norm_value[1 + norm_curr_ofs] + ADC_curr_norm_value[2 + norm_curr_ofs]);
				} else if (tim->CCR2 > tim->CCR1 && tim->CCR2 > tim->CCR3) {
					ADC_curr_norm_value[1 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[2 + norm_curr_ofs]);
				} else if (tim->CCR3 > tim->CCR1 && tim->CCR3 > tim->CCR2) {
					ADC_curr_norm_value[2 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[1 + norm_curr_ofs]);
				}
			}
		}
#else
		if (tim->CCR1 < (tim->ARR - 500) && tim->CCR2 < (tim->ARR - 500)) {
			// Use the same 2 shunts on low modulation, as that will avoid jumps in the current reading.
			// This is especially important when using HFI.
			ADC_curr_norm_value[2 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[1 + norm_curr_ofs]);
		} else {
			if (tim->CCR1 > tim->CCR2 && tim->CCR1 > tim->CCR3) {
				ADC_curr_norm_value[0 + norm_curr_ofs] = -(ADC_curr_norm_value[1 + norm_curr_ofs] + ADC_curr_norm_value[2 + norm_curr_ofs]);
			} else if (tim->CCR2 > tim->CCR1 && tim->CCR2 > tim->CCR3) {
				ADC_curr_norm_value[1 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[2 + norm_curr_ofs]);
			} else if (tim->CCR3 > tim->CCR1 && tim->CCR3 > tim->CCR2) {
				ADC_curr_norm_value[2 + norm_curr_ofs] = -(ADC_curr_norm_value[0 + norm_curr_ofs] + ADC_curr_norm_value[1 + norm_curr_ofs]);
			}
		}
#endif
	}
#endif

	float ia = (float)ADC_curr_norm_value[0 + norm_curr_ofs] * FAC_CURRENT;
	float ib = (float)ADC_curr_norm_value[1 + norm_curr_ofs] * FAC_CURRENT;
//	float ic = -(ia + ib);

#ifdef HW_HAS_PHASE_SHUNTS
	float dt;
	if (conf_now->foc_sample_v0_v7) {
		dt = 1.0 / conf_now->foc_f_sw;
	} else {
		dt = 1.0 / (conf_now->foc_f_sw / 2.0);
	}
#else
	float dt = 1.0 / (conf_now->foc_f_sw / 2.0);
#endif

	// This has to be done for the skip function to have any chance at working with the
	// observer and control loops.
	// TODO: Test this.
	dt /= (float)FOC_CONTROL_LOOP_FREQ_DIVIDER;

	UTILS_LP_FAST(motor_now->m_motor_state.v_bus, GET_INPUT_VOLTAGE(), 0.1);

	float enc_ang = 0;
	if (encoder_is_configured()) {
		if (virtual_motor_is_connected()){
			enc_ang = virtual_motor_get_angle_deg();
		} else {
			enc_ang = encoder_read_deg();
		}

		float phase_tmp = enc_ang;
		if (conf_now->foc_encoder_inverted) {
			phase_tmp = 360.0 - phase_tmp;
		}
		phase_tmp *= conf_now->foc_encoder_ratio;
		phase_tmp -= conf_now->foc_encoder_offset;
		utils_norm_angle((float*)&phase_tmp);
		motor_now->m_phase_now_encoder = phase_tmp * (M_PI / 180.0);
	}

	const float phase_diff = utils_angle_difference_rad(motor_now->m_motor_state.phase, motor_now->m_phase_before);
	motor_now->m_phase_before = motor_now->m_motor_state.phase;

	if (motor_now->m_state == MC_STATE_RUNNING) {
		// Clarke transform assuming balanced currents
		motor_now->m_motor_state.i_alpha = ia;
		motor_now->m_motor_state.i_beta = ONE_BY_SQRT3 * ia + TWO_BY_SQRT3 * ib;

		// Full Clarke transform in case there are current offsets
//		m_motor_state.i_alpha = (2.0 / 3.0) * ia - (1.0 / 3.0) * ib - (1.0 / 3.0) * ic;
//		m_motor_state.i_beta = ONE_BY_SQRT3 * ib - ONE_BY_SQRT3 * ic;

		const float duty_abs = fabsf(motor_now->m_motor_state.duty_now);
		float id_set_tmp = motor_now->m_id_set;
		float iq_set_tmp = motor_now->m_iq_set;
		motor_now->m_motor_state.max_duty = conf_now->l_max_duty;

		UTILS_LP_FAST(motor_now->m_duty_filtered, motor_now->m_motor_state.duty_now, 0.1);
		utils_truncate_number((float*)&motor_now->m_duty_filtered, -1.0, 1.0);

		float duty_set = motor_now->m_duty_cycle_set;
		bool control_duty = motor_now->m_control_mode == CONTROL_MODE_DUTY ||
				motor_now->m_control_mode == CONTROL_MODE_OPENLOOP_DUTY ||
				motor_now->m_control_mode == CONTROL_MODE_OPENLOOP_DUTY_PHASE;

		// When the modulation is low in brake mode and the set brake current
		// cannot be reached, short all phases to get more braking without
		// applying active braking. Use a bit of hysteresis when leaving
		// the shorted mode.
		if (motor_now->m_control_mode == CONTROL_MODE_CURRENT_BRAKE &&
				fabsf(motor_now->m_duty_filtered) < conf_now->l_min_duty * 1.5 &&
				(motor_now->m_motor_state.i_abs * (motor_now->m_was_full_brake ? 1.0 : 1.5)) < fabsf(motor_now->m_iq_set)) {
			control_duty = true;
			duty_set = 0.0;
			motor_now->m_was_full_brake = true;
		} else {
			motor_now->m_was_full_brake = false;
		}

		// Brake when set ERPM is below min ERPM
		if (motor_now->m_control_mode == CONTROL_MODE_SPEED &&
				fabsf(motor_now->m_speed_pid_set_rpm) < conf_now->s_pid_min_erpm) {
			control_duty = true;
			duty_set = 0.0;
		}

		// Reset integrator when leaving duty cycle mode, as the windup protection is not too fast. Making
		// better windup protection is probably better, but not easy.
		if (!control_duty && motor_now->m_was_control_duty) {
			motor_now->m_motor_state.vq_int = motor_now->m_motor_state.vq;
			if (conf_now->foc_cc_decoupling == FOC_CC_DECOUPLING_BEMF ||
					conf_now->foc_cc_decoupling == FOC_CC_DECOUPLING_CROSS_BEMF) {
				motor_now->m_motor_state.vq_int -= motor_now->m_motor_state.speed_rad_s * conf_now->foc_motor_flux_linkage;
			}
		}
		motor_now->m_was_control_duty = control_duty;

		if (control_duty) {
			// Duty cycle control
			if (fabsf(duty_set) < (duty_abs - 0.05) ||
					(SIGN(motor_now->m_motor_state.vq) * motor_now->m_motor_state.iq) < conf_now->lo_current_min) {
				// Truncating the duty cycle here would be dangerous, so run a PID controller.

				// Compensation for supply voltage variations
				float scale = 1.0 / GET_INPUT_VOLTAGE();

				// Compute error
				float error = duty_set - motor_now->m_motor_state.duty_now;

				// Compute parameters
				float p_term = error * conf_now->foc_duty_dowmramp_kp * scale;
				motor_now->m_duty_i_term += error * (conf_now->foc_duty_dowmramp_ki * dt) * scale;

				// I-term wind-up protection
				utils_truncate_number((float*)&motor_now->m_duty_i_term, -1.0, 1.0);

				// Calculate output
				float output = p_term + motor_now->m_duty_i_term;
				utils_truncate_number(&output, -1.0, 1.0);
				iq_set_tmp = output * conf_now->lo_current_max;
			} else {
				// If the duty cycle is less than or equal to the set duty cycle just limit
				// the modulation and use the maximum allowed current.
				motor_now->m_duty_i_term = 0.0;
				motor_now->m_motor_state.max_duty = duty_set;
				if (duty_set > 0.0) {
					iq_set_tmp = conf_now->lo_current_max;
				} else {
					iq_set_tmp = -conf_now->lo_current_max;
				}
			}
		} else if (motor_now->m_control_mode == CONTROL_MODE_CURRENT_BRAKE) {
			// Braking
			iq_set_tmp = fabsf(iq_set_tmp);

			if (phase_diff > 0.0) {
				iq_set_tmp = -iq_set_tmp;
			} else if (phase_diff == 0.0) {
				iq_set_tmp = 0.0;
			}
		}

		// Run observer
		if (!motor_now->m_phase_override) {
			observer_update(motor_now->m_motor_state.v_alpha, motor_now->m_motor_state.v_beta,
							motor_now->m_motor_state.i_alpha, motor_now->m_motor_state.i_beta, dt,
							&motor_now->m_observer_x1, &motor_now->m_observer_x2, &motor_now->m_phase_now_observer, motor_now);
			motor_now->m_phase_now_observer += motor_now->m_pll_speed * dt * 0.5;
			utils_norm_angle_rad((float*)&motor_now->m_phase_now_observer);
		}

		switch (conf_now->foc_sensor_mode) {
		case FOC_SENSOR_MODE_ENCODER:
			if (encoder_index_found()) {
				motor_now->m_motor_state.phase = correct_encoder(
						motor_now->m_phase_now_observer,
						motor_now->m_phase_now_encoder,
						motor_now->m_speed_est_fast,
						conf_now->foc_sl_erpm,
						motor_now);
			} else {
				// Rotate the motor in open loop if the index isn't found.
				motor_now->m_motor_state.phase = motor_now->m_phase_now_encoder_no_index;
			}

			if (!motor_now->m_phase_override) {
				id_set_tmp = 0.0;
			}
			break;
		case FOC_SENSOR_MODE_HALL:
			motor_now->m_phase_now_observer = correct_hall(motor_now->m_phase_now_observer, dt, motor_now);
			motor_now->m_motor_state.phase = motor_now->m_phase_now_observer;

			if (!motor_now->m_phase_override) {
				id_set_tmp = 0.0;
			}
			break;
		case FOC_SENSOR_MODE_SENSORLESS:
			if (motor_now->m_phase_observer_override) {
				motor_now->m_motor_state.phase = motor_now->m_phase_now_observer_override;
			} else {
				motor_now->m_motor_state.phase = motor_now->m_phase_now_observer;
			}

			// Inject D axis current at low speed to make the observer track
			// better. This does not seem to be necessary with dead time
			// compensation.
			// Note: this is done at high rate prevent noise.
			if (!motor_now->m_phase_override) {
				if (duty_abs < conf_now->foc_sl_d_current_duty) {
					id_set_tmp = utils_map(duty_abs, 0.0, conf_now->foc_sl_d_current_duty,
										   fabsf(motor_now->m_motor_state.iq_target) * conf_now->foc_sl_d_current_factor, 0.0);
				} else {
					id_set_tmp = 0.0;
				}
			}
			break;

		case FOC_SENSOR_MODE_HFI:
			if (fabsf(motor_now->m_speed_est_fast * (60.0 / (2.0 * M_PI))) > conf_now->foc_sl_erpm_hfi) {
				motor_now->m_hfi.observer_zero_time = 0;
			} else {
				motor_now->m_hfi.observer_zero_time += dt;
			}

			if (motor_now->m_hfi.observer_zero_time < conf_now->foc_hfi_obs_ovr_sec) {
				motor_now->m_hfi.angle = motor_now->m_phase_now_observer;
			}

			motor_now->m_motor_state.phase = correct_encoder(
					motor_now->m_phase_now_observer,
					motor_now->m_hfi.angle,
					motor_now->m_speed_est_fast,
					conf_now->foc_sl_erpm_hfi,
					motor_now);

			if (!motor_now->m_phase_override) {
				id_set_tmp = 0.0;
			}
			break;
		}

		if (motor_now->m_control_mode == CONTROL_MODE_HANDBRAKE) {
			// Force the phase to 0 in handbrake mode so that the current simply locks the rotor.
			motor_now->m_motor_state.phase = 0.0;
		} else if (motor_now->m_control_mode == CONTROL_MODE_OPENLOOP ||
				motor_now->m_control_mode == CONTROL_MODE_OPENLOOP_DUTY) {
			motor_now->m_openloop_angle += dt * motor_now->m_openloop_speed;
			utils_norm_angle_rad((float*)&motor_now->m_openloop_angle);
			motor_now->m_motor_state.phase = motor_now->m_openloop_angle;
		} else if (motor_now->m_control_mode == CONTROL_MODE_OPENLOOP_PHASE ||
				motor_now->m_control_mode == CONTROL_MODE_OPENLOOP_DUTY_PHASE) {
			motor_now->m_motor_state.phase = motor_now->m_openloop_phase;
		}

		if (motor_now->m_phase_override) {
			motor_now->m_motor_state.phase = motor_now->m_phase_now_override;
		}

		// Apply current limits
		// TODO: Consider D axis current for the input current as well.
		const float mod_q = motor_now->m_motor_state.mod_q;
		if (mod_q > 0.001) {
			utils_truncate_number(&iq_set_tmp, conf_now->lo_in_current_min / mod_q, conf_now->lo_in_current_max / mod_q);
		} else if (mod_q < -0.001) {
			utils_truncate_number(&iq_set_tmp, conf_now->lo_in_current_max / mod_q, conf_now->lo_in_current_min / mod_q);
		}

		if (mod_q > 0.0) {
			utils_truncate_number(&iq_set_tmp, conf_now->lo_current_min, conf_now->lo_current_max);
		} else {
			utils_truncate_number(&iq_set_tmp, -conf_now->lo_current_max, -conf_now->lo_current_min);
		}

		utils_saturate_vector_2d(&id_set_tmp, &iq_set_tmp,
								 utils_max_abs(conf_now->lo_current_max, conf_now->lo_current_min));

		motor_now->m_motor_state.id_target = id_set_tmp;
		motor_now->m_motor_state.iq_target = iq_set_tmp;

		control_current(motor_now, dt);
	} else {
		// The current is 0 when the motor is undriven
		motor_now->m_motor_state.i_alpha = 0.0;
		motor_now->m_motor_state.i_beta = 0.0;
		motor_now->m_motor_state.id = 0.0;
		motor_now->m_motor_state.iq = 0.0;
		motor_now->m_motor_state.id_filter = 0.0;
		motor_now->m_motor_state.iq_filter = 0.0;
		motor_now->m_motor_state.i_bus = 0.0;
		motor_now->m_motor_state.i_abs = 0.0;
		motor_now->m_motor_state.i_abs_filter = 0.0;

		// Track back emf
#ifdef HW_HAS_DUAL_MOTORS
#ifdef HW_HAS_3_SHUNTS
		float Va, Vb, Vc;
		if (is_second_motor) {
			Va = ADC_VOLTS(ADC_IND_SENS4) * ((VIN_R1 + VIN_R2) / VIN_R2);
			Vb = ADC_VOLTS(ADC_IND_SENS5) * ((VIN_R1 + VIN_R2) / VIN_R2);
			Vc = ADC_VOLTS(ADC_IND_SENS6) * ((VIN_R1 + VIN_R2) / VIN_R2);
		} else {
			Va = ADC_VOLTS(ADC_IND_SENS1) * ((VIN_R1 + VIN_R2) / VIN_R2);
			Vb = ADC_VOLTS(ADC_IND_SENS2) * ((VIN_R1 + VIN_R2) / VIN_R2);
			Vc = ADC_VOLTS(ADC_IND_SENS3) * ((VIN_R1 + VIN_R2) / VIN_R2);
		}
#else
		float Va, Vb, Vc;
		if (is_second_motor) {
			Va = ADC_VOLTS(ADC_IND_SENS4) * ((VIN_R1 + VIN_R2) / VIN_R2);
			Vb = ADC_VOLTS(ADC_IND_SENS6) * ((VIN_R1 + VIN_R2) / VIN_R2);
			Vc = ADC_VOLTS(ADC_IND_SENS5) * ((VIN_R1 + VIN_R2) / VIN_R2);
		} else {
			Va = ADC_VOLTS(ADC_IND_SENS1) * ((VIN_R1 + VIN_R2) / VIN_R2);
			Vb = ADC_VOLTS(ADC_IND_SENS3) * ((VIN_R1 + VIN_R2) / VIN_R2);
			Vc = ADC_VOLTS(ADC_IND_SENS2) * ((VIN_R1 + VIN_R2) / VIN_R2);
		}
#endif
#else
#ifdef HW_HAS_3_SHUNTS
		float Va = ADC_VOLTS(ADC_IND_SENS1) * ((VIN_R1 + VIN_R2) / VIN_R2);
		float Vb = ADC_VOLTS(ADC_IND_SENS2) * ((VIN_R1 + VIN_R2) / VIN_R2);
		float Vc = ADC_VOLTS(ADC_IND_SENS3) * ((VIN_R1 + VIN_R2) / VIN_R2);
#else
		float Va = ADC_VOLTS(ADC_IND_SENS1) * ((VIN_R1 + VIN_R2) / VIN_R2);
		float Vb = ADC_VOLTS(ADC_IND_SENS3) * ((VIN_R1 + VIN_R2) / VIN_R2);
		float Vc = ADC_VOLTS(ADC_IND_SENS2) * ((VIN_R1 + VIN_R2) / VIN_R2);
#endif
#endif

		// Full Clarke transform (no balanced voltages)
		motor_now->m_motor_state.v_alpha = (2.0 / 3.0) * Va - (1.0 / 3.0) * Vb - (1.0 / 3.0) * Vc;
		motor_now->m_motor_state.v_beta = ONE_BY_SQRT3 * Vb - ONE_BY_SQRT3 * Vc;

#ifdef HW_USE_LINE_TO_LINE
		// rotate alpha-beta 30 degrees to compensate for line-to-line phase voltage sensing
		float x_tmp = motor_now->m_motor_state.v_alpha;
		float y_tmp = motor_now->m_motor_state.v_beta;

		motor_now->m_motor_state.v_alpha = x_tmp * COS_MINUS_30_DEG - y_tmp * SIN_MINUS_30_DEG;
		motor_now->m_motor_state.v_beta = x_tmp * SIN_MINUS_30_DEG + y_tmp * COS_MINUS_30_DEG;

		// compensate voltage amplitude
		motor_now->m_motor_state.v_alpha *= ONE_BY_SQRT3;
		motor_now->m_motor_state.v_beta *= ONE_BY_SQRT3;
#endif

		// Run observer
		observer_update(motor_now->m_motor_state.v_alpha, motor_now->m_motor_state.v_beta,
						motor_now->m_motor_state.i_alpha, motor_now->m_motor_state.i_beta, dt,
						&motor_now->m_observer_x1, &motor_now->m_observer_x2, 0, motor_now);
		motor_now->m_phase_now_observer = utils_fast_atan2(motor_now->m_x2_prev + motor_now->m_observer_x2,
														   motor_now->m_x1_prev + motor_now->m_observer_x1);

		motor_now->m_x1_prev = motor_now->m_observer_x1;
		motor_now->m_x2_prev = motor_now->m_observer_x2;

		switch (conf_now->foc_sensor_mode) {
		case FOC_SENSOR_MODE_ENCODER:
			motor_now->m_motor_state.phase = correct_encoder(
					motor_now->m_phase_now_observer,
					motor_now->m_phase_now_encoder,
					motor_now->m_speed_est_fast,
					conf_now->foc_sl_erpm,
					motor_now);
			break;
		case FOC_SENSOR_MODE_HALL:
			motor_now->m_phase_now_observer = correct_hall(motor_now->m_phase_now_observer, dt, motor_now);
			motor_now->m_motor_state.phase = motor_now->m_phase_now_observer;
			break;
		case FOC_SENSOR_MODE_SENSORLESS:
			motor_now->m_motor_state.phase = motor_now->m_phase_now_observer;
			break;
		case FOC_SENSOR_MODE_HFI: {
			motor_now->m_motor_state.phase = motor_now->m_phase_now_observer;
			if (fabsf(motor_now->m_pll_speed * (60.0 / (2.0 * M_PI))) < (conf_now->foc_sl_erpm_hfi * 1.1)) {
				motor_now->m_hfi.est_done_cnt = 0;
			}
		} break;
		}

		// HFI Restore
		CURRENT_FILTER_ON();
		motor_now->m_hfi.ind = 0;
		motor_now->m_hfi.ready = false;
		motor_now->m_hfi.is_samp_n = false;
		motor_now->m_hfi.prev_sample = 0.0;
		motor_now->m_hfi.angle = motor_now->m_motor_state.phase;

		float c, s;
		utils_fast_sincos_better(motor_now->m_motor_state.phase, &s, &c);

		// Park transform
		float vd_tmp = c * motor_now->m_motor_state.v_alpha + s * motor_now->m_motor_state.v_beta;
		float vq_tmp = c * motor_now->m_motor_state.v_beta  - s * motor_now->m_motor_state.v_alpha;

		UTILS_NAN_ZERO(motor_now->m_motor_state.vd);
		UTILS_NAN_ZERO(motor_now->m_motor_state.vq);

		UTILS_LP_FAST(motor_now->m_motor_state.vd, vd_tmp, 0.2);
		UTILS_LP_FAST(motor_now->m_motor_state.vq, vq_tmp, 0.2);

		// Set the current controller integrator to the BEMF voltage to avoid
		// a current spike when the motor is driven again. Notice that we have
		// to take decoupling into account.
		motor_now->m_motor_state.vd_int = motor_now->m_motor_state.vd;
		motor_now->m_motor_state.vq_int = motor_now->m_motor_state.vq;

		if (conf_now->foc_cc_decoupling == FOC_CC_DECOUPLING_BEMF ||
				conf_now->foc_cc_decoupling == FOC_CC_DECOUPLING_CROSS_BEMF) {
			motor_now->m_motor_state.vq_int -= motor_now->m_motor_state.speed_rad_s * conf_now->foc_motor_flux_linkage;
		}

		// Update corresponding modulation
		motor_now->m_motor_state.mod_d = motor_now->m_motor_state.vd / ((2.0 / 3.0) * motor_now->m_motor_state.v_bus);
		motor_now->m_motor_state.mod_q = motor_now->m_motor_state.vq / ((2.0 / 3.0) * motor_now->m_motor_state.v_bus);
	}

	// Calculate duty cycle
	motor_now->m_motor_state.duty_now = SIGN(motor_now->m_motor_state.vq) *
			sqrtf(SQ(motor_now->m_motor_state.mod_d) + SQ(motor_now->m_motor_state.mod_q))
			/ SQRT3_BY_2;

	// Run PLL for speed estimation
	pll_run(motor_now->m_motor_state.phase, dt, &motor_now->m_pll_phase, &motor_now->m_pll_speed, conf_now);
	motor_now->m_motor_state.speed_rad_s = motor_now->m_pll_speed;

	// Low latency speed estimation, for e.g. HFI.
	{
		// Based on back emf and motor parameters. This could be useful for a resistance observer in the future.
//		UTILS_LP_FAST(m_speed_est_fast, (m_motor_state.vq - (3.0 / 2.0) * m_conf->foc_motor_r * m_motor_state.iq) / m_conf->foc_motor_flux_linkage, 0.05);

		// Based on angle difference
		float diff = utils_angle_difference_rad(motor_now->m_motor_state.phase, motor_now->m_phase_before_speed_est);
		utils_truncate_number(&diff, -M_PI / 3.0, M_PI / 3.0);

		UTILS_LP_FAST(motor_now->m_speed_est_fast, diff / dt, 0.01);
		UTILS_NAN_ZERO(motor_now->m_speed_est_fast);

		UTILS_LP_FAST(motor_now->m_speed_est_faster, diff / dt, 0.2);
		UTILS_NAN_ZERO(motor_now->m_speed_est_faster);

		motor_now->m_phase_before_speed_est = motor_now->m_motor_state.phase;
	}

	// Update tachometer (resolution = 60 deg as for BLDC)
	float ph_tmp = motor_now->m_motor_state.phase;
	utils_norm_angle_rad(&ph_tmp);
	int step = (int)floorf((ph_tmp + M_PI) / (2.0 * M_PI) * 6.0);
	utils_truncate_number_int(&step, 0, 5);
	int diff = step - motor_now->m_tacho_step_last;
	motor_now->m_tacho_step_last = step;

	if (diff > 3) {
		diff -= 6;
	} else if (diff < -2) {
		diff += 6;
	}

	motor_now->m_tachometer += diff;
	motor_now->m_tachometer_abs += abs(diff);

	// Track position control angle
	// TODO: Have another look at this.
	float angle_now = 0.0;
	if (encoder_is_configured()) {
		if (conf_now->m_sensor_port_mode == SENSOR_PORT_MODE_TS5700N8501_MULTITURN) {
			angle_now = encoder_read_deg_multiturn();
		} else {
			angle_now = enc_ang;
		}
	} else {
		angle_now = motor_now->m_motor_state.phase * (180.0 / M_PI);
	}

	if (conf_now->p_pid_ang_div > 0.98 && conf_now->p_pid_ang_div < 1.02) {
		motor_now->m_pos_pid_now = angle_now;
	} else {
		float diff_f = utils_angle_difference(angle_now, motor_now->m_pid_div_angle_last);
		motor_now->m_pid_div_angle_last = angle_now;
		motor_now->m_pos_pid_now += diff_f / conf_now->p_pid_ang_div;
		utils_norm_angle((float*)&motor_now->m_pos_pid_now);
	}

	// Run position control
	if (motor_now->m_state == MC_STATE_RUNNING) {
		run_pid_control_pos(motor_now->m_pos_pid_now, motor_now->m_pos_pid_set, dt, motor_now);
	}

#ifdef AD2S1205_SAMPLE_GPIO
	// Release sample in the AD2S1205 resolver IC.
	palSetPad(AD2S1205_SAMPLE_GPIO, AD2S1205_SAMPLE_PIN);
#endif

#ifdef HW_HAS_DUAL_MOTORS
	mc_interface_mc_timer_isr(is_second_motor);
#else
	mc_interface_mc_timer_isr(false);
#endif

	m_isr_motor = 0;
	m_last_adc_isr_duration = timer_seconds_elapsed_since(t_start);
}

// Private functions

static void timer_update(volatile motor_all_state_t *motor, float dt) {
	float openloop_rpm = utils_map(fabsf(motor->m_motor_state.iq_target),
								   0.0, motor->m_conf->l_current_max,
								   0.0, motor->m_conf->foc_openloop_rpm);

	utils_truncate_number_abs(&openloop_rpm, motor->m_conf->foc_openloop_rpm);

	const float min_rads = (openloop_rpm * 2.0 * M_PI) / 60.0;

	float add_min_speed = 0.0;
	if (motor->m_motor_state.duty_now > 0.0) {
		add_min_speed = min_rads * dt;
	} else {
		add_min_speed = -min_rads * dt;
	}

	// Open loop encoder angle for when the index is not found
	motor->m_phase_now_encoder_no_index += add_min_speed;
	utils_norm_angle_rad((float*)&motor->m_phase_now_encoder_no_index);

	// Output a minimum speed from the observer
	if (fabsf(motor->m_pll_speed) < min_rads) {
		motor->m_min_rpm_hyst_timer += dt;
	} else if (motor->m_min_rpm_hyst_timer > 0.0) {
		motor->m_min_rpm_hyst_timer -= dt;
	}

	// Don't use this in brake mode.
	if (motor->m_control_mode == CONTROL_MODE_CURRENT_BRAKE || fabsf(motor->m_motor_state.duty_now) < 0.001) {
		motor->m_min_rpm_hyst_timer = 0.0;
		motor->m_phase_observer_override = false;
	}

	bool started_now = false;
	if (motor->m_min_rpm_hyst_timer > motor->m_conf->foc_sl_openloop_hyst && motor->m_min_rpm_timer <= 0.0001) {
		motor->m_min_rpm_timer = motor->m_conf->foc_sl_openloop_time;
		started_now = true;
	}

	if (motor->m_min_rpm_timer > 0.0) {
		motor->m_phase_now_observer_override += add_min_speed;

		// When the motor gets stuck it tends to be 90 degrees off, so start the open loop
		// sequence by correcting with 90 degrees.
		if (started_now) {
			if (motor->m_motor_state.duty_now > 0.0) {
				motor->m_phase_now_observer_override += M_PI / 2.0;
			} else {
				motor->m_phase_now_observer_override -= M_PI / 2.0;
			}
		}

		utils_norm_angle_rad((float*)&motor->m_phase_now_observer_override);
		motor->m_phase_observer_override = true;
		motor->m_min_rpm_timer -= dt;
		motor->m_min_rpm_hyst_timer = 0.0;
	} else {
		motor->m_phase_now_observer_override = motor->m_phase_now_observer;
		motor->m_phase_observer_override = false;
	}

	// Samples
	if (motor->m_state == MC_STATE_RUNNING) {
		const volatile float vd_tmp = motor->m_motor_state.vd;
		const volatile float vq_tmp = motor->m_motor_state.vq;
		const volatile float id_tmp = motor->m_motor_state.id;
		const volatile float iq_tmp = motor->m_motor_state.iq;

		motor->m_samples.avg_current_tot += sqrtf(SQ(id_tmp) + SQ(iq_tmp));
		motor->m_samples.avg_voltage_tot += sqrtf(SQ(vd_tmp) + SQ(vq_tmp));
		motor->m_samples.sample_num++;
	}

	// Update and the observer gain.

	// Old gain scaling, based on duty cycle
//	motor->m_gamma_now = utils_map(fabsf(motor->m_motor_state.duty_now), 0.0, 1.0,
//			motor->m_conf->foc_observer_gain * motor->m_conf->foc_observer_gain_slow,
//			motor->m_conf->foc_observer_gain);

	// Observer gain scaling, based on bus voltage and duty cycle
	float gamma_tmp = utils_map(fabsf(motor->m_motor_state.duty_now),
								0.0, 40.0 / motor->m_motor_state.v_bus,
								0,
								motor->m_conf->foc_observer_gain);
	if (gamma_tmp < (motor->m_conf->foc_observer_gain_slow * motor->m_conf->foc_observer_gain)) {
		gamma_tmp = motor->m_conf->foc_observer_gain_slow * motor->m_conf->foc_observer_gain;
	}

	// 4.0 scaling is kind of arbitrary, but it should make configs from old VESC Tools more likely to work.
	motor->m_gamma_now = gamma_tmp * 4.0;
}

static THD_FUNCTION(timer_thread, arg) {
	(void)arg;

	chRegSetThreadName("foc timer");

	for(;;) {
		const float dt = 0.001;

		if (timer_thd_stop) {
			timer_thd_stop = false;
			return;
		}

		timer_update(&m_motor_1, dt);
#ifdef HW_HAS_DUAL_MOTORS
		timer_update(&m_motor_2, dt);
#endif

		run_pid_control_speed(dt, &m_motor_1);
#ifdef HW_HAS_DUAL_MOTORS
		run_pid_control_speed(dt, &m_motor_2);
#endif

		chThdSleepMilliseconds(1);
	}
}

static void hfi_update(volatile motor_all_state_t *motor) {
	float rpm_abs = fabsf(motor->m_speed_est_fast * (60.0 / (2.0 * M_PI)));

	if (rpm_abs > motor->m_conf->foc_sl_erpm_hfi) {
		motor->m_hfi.angle = motor->m_phase_now_observer;
	}

	if (motor->m_hfi.ready) {
		float real_bin1, imag_bin1, real_bin2, imag_bin2;
		motor->m_hfi.fft_bin1_func((float*)motor->m_hfi.buffer, &real_bin1, &imag_bin1);
		motor->m_hfi.fft_bin2_func((float*)motor->m_hfi.buffer, &real_bin2, &imag_bin2);

		float mag_bin_1 = sqrtf(SQ(imag_bin1) + SQ(real_bin1));
		float angle_bin_1 = -utils_fast_atan2(imag_bin1, real_bin1);

		angle_bin_1 += M_PI / 1.7; // Why 1.7??
		utils_norm_angle_rad(&angle_bin_1);

		float mag_bin_2 = sqrtf(SQ(imag_bin2) + SQ(real_bin2));
		float angle_bin_2 = -utils_fast_atan2(imag_bin2, real_bin2) / 2.0;

		// Assuming this thread is much faster than it takes to fill the HFI buffer completely,
		// we should lag 1/2 HFI buffer behind in phase. Compensate for that here.
		float dt_sw;
		if (motor->m_conf->foc_sample_v0_v7) {
			dt_sw = 1.0 / motor->m_conf->foc_f_sw;
		} else {
			dt_sw = 1.0 / (motor->m_conf->foc_f_sw / 2.0);
		}
		angle_bin_2 += motor->m_motor_state.speed_rad_s * ((float)motor->m_hfi.samples / 2.0) * dt_sw;

		if (fabsf(utils_angle_difference_rad(angle_bin_2 + M_PI, motor->m_hfi.angle)) <
				fabsf(utils_angle_difference_rad(angle_bin_2, motor->m_hfi.angle))) {
			angle_bin_2 += M_PI;
		}

		if (motor->m_hfi.est_done_cnt < motor->m_conf->foc_hfi_start_samples) {
			motor->m_hfi.est_done_cnt++;

			if (fabsf(utils_angle_difference_rad(angle_bin_2, angle_bin_1)) > (M_PI / 2.0)) {
				motor->m_hfi.flip_cnt++;
			}
		} else {
			if (motor->m_hfi.flip_cnt >= (motor->m_conf->foc_hfi_start_samples / 2)) {
				angle_bin_2 += M_PI;
			}
			motor->m_hfi.flip_cnt = 0;
		}

		motor->m_hfi.angle = angle_bin_2;
		utils_norm_angle_rad((float*)&motor->m_hfi.angle);

		// As angle_bin_1 is based on saturation, it is only accurate when the motor current is low. It
		// might be possible to compensate for that, which would allow HFI on non-salient motors.
		//			m_hfi.angle = angle_bin_1;

		if (motor->m_hfi_plot_en == 1) {
			static float hfi_plot_div = 0;
			hfi_plot_div++;

			if (hfi_plot_div >= 8) {
				hfi_plot_div = 0;

				float real_bin0, imag_bin0;
				motor->m_hfi.fft_bin0_func((float*)motor->m_hfi.buffer, &real_bin0, &imag_bin0);

				commands_plot_set_graph(0);
				commands_send_plot_points(motor->m_hfi_plot_sample, motor->m_hfi.angle);

				commands_plot_set_graph(1);
				commands_send_plot_points(motor->m_hfi_plot_sample, angle_bin_1);

				commands_plot_set_graph(2);
				commands_send_plot_points(motor->m_hfi_plot_sample, 2.0 * mag_bin_2 * 1e6);

				commands_plot_set_graph(3);
				commands_send_plot_points(motor->m_hfi_plot_sample, 2.0 * mag_bin_1 * 1e6);

				commands_plot_set_graph(4);
				commands_send_plot_points(motor->m_hfi_plot_sample, real_bin0 * 1e6);

//					commands_plot_set_graph(0);
//					commands_send_plot_points(motor->m_hfi_plot_sample, motor->m_motor_state.speed_rad_s);
//
//					commands_plot_set_graph(1);
//					commands_send_plot_points(motor->m_hfi_plot_sample, motor->m_speed_est_fast);

				motor->m_hfi_plot_sample++;
			}
		} else if (motor->m_hfi_plot_en == 2) {
			static float hfi_plot_div = 0;
			hfi_plot_div++;

			if (hfi_plot_div >= 8) {
				hfi_plot_div = 0;

				if (motor->m_hfi_plot_sample >= motor->m_hfi.samples) {
					motor->m_hfi_plot_sample = 0;
				}

				commands_plot_set_graph(0);
				commands_send_plot_points(motor->m_hfi_plot_sample, motor->m_hfi.buffer_current[(int)motor->m_hfi_plot_sample]);

				commands_plot_set_graph(1);
				commands_send_plot_points(motor->m_hfi_plot_sample, motor->m_hfi.buffer[(int)motor->m_hfi_plot_sample] * 1e6);

				motor->m_hfi_plot_sample++;
			}
		}
	} else {
		motor->m_hfi.angle = motor->m_phase_now_observer;
	}
}

static THD_FUNCTION(hfi_thread, arg) {
	(void)arg;

	chRegSetThreadName("foc hfi");

	for(;;) {
		if (hfi_thd_stop) {
			hfi_thd_stop = false;
			return;
		}

		hfi_update(&m_motor_1);
#ifdef HW_HAS_DUAL_MOTORS
		hfi_update(&m_motor_2);
#endif

		chThdSleepMicroseconds(500);
	}
}

static void do_dc_cal(void) {
	DCCAL_ON();

	// Wait max 5 seconds
	int cnt = 0;
	while(IS_DRV_FAULT()){
		chThdSleepMilliseconds(1);
		cnt++;
		if (cnt > 5000) {
			break;
		}
	};

	chThdSleepMilliseconds(1000);

	memset((int*)m_motor_1.m_curr_sum, 0, sizeof(m_motor_1.m_curr_sum));
	m_motor_1.m_curr_samples = 0;
	while(m_motor_1.m_curr_samples < 4000) {};
	m_motor_1.m_curr_ofs[0] = m_motor_1.m_curr_sum[0] / m_motor_1.m_curr_samples;
	m_motor_1.m_curr_ofs[1] = m_motor_1.m_curr_sum[1] / m_motor_1.m_curr_samples;
#ifdef HW_HAS_3_SHUNTS
	m_motor_1.m_curr_ofs[2] = m_motor_1.m_curr_sum[2] / m_motor_1.m_curr_samples;
#endif

#ifdef HW_HAS_DUAL_MOTORS
	memset((int*)m_motor_2.m_curr_sum, 0, sizeof(m_motor_2.m_curr_sum));
	m_motor_2.m_curr_samples = 0;
	while(m_motor_2.m_curr_samples < 4000) {};
	m_motor_2.m_curr_ofs[0] = m_motor_2.m_curr_sum[0] / m_motor_2.m_curr_samples;
	m_motor_2.m_curr_ofs[1] = m_motor_2.m_curr_sum[1] / m_motor_2.m_curr_samples;
#ifdef HW_HAS_3_SHUNTS
	m_motor_2.m_curr_ofs[2] = m_motor_2.m_curr_sum[2] / m_motor_2.m_curr_samples;
#endif
#endif

	DCCAL_OFF();
	m_dccal_done = true;
}

// See http://cas.ensmp.fr/~praly/Telechargement/Journaux/2010-IEEE_TPEL-Lee-Hong-Nam-Ortega-Praly-Astolfi.pdf
void observer_update(float v_alpha, float v_beta, float i_alpha, float i_beta,
					 float dt, volatile float *x1, volatile float *x2, volatile float *phase, volatile motor_all_state_t *motor) {

	volatile mc_configuration *conf_now = motor->m_conf;

	const float L = (3.0 / 2.0) * conf_now->foc_motor_l;
	float R = (3.0 / 2.0) * conf_now->foc_motor_r;

	// Saturation compensation
	const float sign = (motor->m_motor_state.iq * motor->m_motor_state.vq) >= 0.0 ? 1.0 : -1.0;
	R -= R * sign * conf_now->foc_sat_comp * (motor->m_motor_state.i_abs_filter / conf_now->l_current_max);

	// Temperature compensation
	const float t = mc_interface_temp_motor_filtered();
	if (conf_now->foc_temp_comp && t > -25.0) {
		R += R * 0.00386 * (t - conf_now->foc_temp_comp_base_temp);
	}

	const float L_ia = L * i_alpha;
	const float L_ib = L * i_beta;
	const float R_ia = R * i_alpha;
	const float R_ib = R * i_beta;
	const float lambda_2 = SQ(conf_now->foc_motor_flux_linkage);
	const float gamma_half = motor->m_gamma_now * 0.5;

	switch (conf_now->foc_observer_type) {
	case FOC_OBSERVER_ORTEGA_ORIGINAL: {
		float err = lambda_2 - (SQ(*x1 - L_ia) + SQ(*x2 - L_ib));
		float x1_dot = -R_ia + v_alpha + gamma_half * (*x1 - L_ia) * err;
		float x2_dot = -R_ib + v_beta + gamma_half * (*x2 - L_ib) * err;
		*x1 += x1_dot * dt;
		*x2 += x2_dot * dt;
	} break;

	case FOC_OBSERVER_ORTEGA_ITERATIVE: {
		// Iterative with some trial and error
		const int iterations = 6;
		const float dt_iteration = dt / (float)iterations;
		for (int i = 0;i < iterations;i++) {
			float err = lambda_2 - (SQ(*x1 - L_ia) + SQ(*x2 - L_ib));
			float gamma_tmp = gamma_half;
			if (utils_truncate_number_abs(&err, lambda_2 * 0.2)) {
				gamma_tmp *= 10.0;
			}
			float x1_dot = -R_ia + v_alpha + gamma_tmp * (*x1 - L_ia) * err;
			float x2_dot = -R_ib + v_beta + gamma_tmp * (*x2 - L_ib) * err;

			*x1 += x1_dot * dt_iteration;
			*x2 += x2_dot * dt_iteration;
		}
	} break;

	default:
		break;
	}

	// Same as iterative, but without iterations.
//	float err = lambda_2 - (SQ(*x1 - L_ia) + SQ(*x2 - L_ib));
//	float gamma_tmp = gamma_half;
//	if (utils_truncate_number_abs(&err, lambda_2 * 0.2)) {
//		gamma_tmp *= 10.0;
//	}
//	float x1_dot = -R_ia + v_alpha + gamma_tmp * (*x1 - L_ia) * err;
//	float x2_dot = -R_ib + v_beta + gamma_tmp * (*x2 - L_ib) * err;
//	*x1 += x1_dot * dt;
//	*x2 += x2_dot * dt;

	UTILS_NAN_ZERO(*x1);
	UTILS_NAN_ZERO(*x2);

	if (phase) {
		*phase = utils_fast_atan2(*x2 - L_ib, *x1 - L_ia);
	}
}

static void pll_run(float phase, float dt, volatile float *phase_var,
					volatile float *speed_var, volatile mc_configuration *conf) {
	UTILS_NAN_ZERO(*phase_var);
	float delta_theta = phase - *phase_var;
	utils_norm_angle_rad(&delta_theta);
	UTILS_NAN_ZERO(*speed_var);
	*phase_var += (*speed_var + conf->foc_pll_kp * delta_theta) * dt;
	utils_norm_angle_rad((float*)phase_var);
	*speed_var += conf->foc_pll_ki * delta_theta * dt;
}

/**
 * Run the current control loop.
 *
 * @param state_m
 * The motor state.
 *
 * Parameters that shall be set before calling this function:
 * id_target
 * iq_target
 * max_duty
 * phase
 * i_alpha
 * i_beta
 * v_bus
 * speed_rad_s
 *
 * Parameters that will be updated in this function:
 * i_bus
 * i_abs
 * i_abs_filter
 * v_alpha
 * v_beta
 * mod_d
 * mod_q
 * id
 * iq
 * id_filter
 * iq_filter
 * vd
 * vq
 * vd_int
 * vq_int
 * svm_sector
 *
 * @param dt
 * The time step in seconds.
 */
static void control_current(volatile motor_all_state_t *motor, float dt) {
	volatile motor_state_t *state_m = &motor->m_motor_state;
	volatile mc_configuration *conf_now = motor->m_conf;

	float c,s;
	utils_fast_sincos_better(state_m->phase, &s, &c);

	float abs_rpm = fabsf(motor->m_speed_est_fast * 60 / (2 * M_PI));

	bool do_hfi = conf_now->foc_sensor_mode == FOC_SENSOR_MODE_HFI &&
			!motor->m_phase_override &&
			abs_rpm < (conf_now->foc_sl_erpm_hfi * (motor->m_cc_was_hfi ? 1.8 : 1.5));
	motor->m_cc_was_hfi = do_hfi;

	// Only allow Q axis current after the HFI ambiguity is resolved. This causes
	// a short delay when starting.
	if (do_hfi && motor->m_hfi.est_done_cnt < conf_now->foc_hfi_start_samples) {
		state_m->iq_target = 0;
	}

	float max_duty = fabsf(state_m->max_duty);
	utils_truncate_number(&max_duty, 0.0, conf_now->l_max_duty);

	state_m->id = c * state_m->i_alpha + s * state_m->i_beta;
	state_m->iq = c * state_m->i_beta  - s * state_m->i_alpha;
	UTILS_LP_FAST(state_m->id_filter, state_m->id, conf_now->foc_current_filter_const);
	UTILS_LP_FAST(state_m->iq_filter, state_m->iq, conf_now->foc_current_filter_const);

	float Ierr_d = state_m->id_target - state_m->id;
	float Ierr_q = state_m->iq_target - state_m->iq;

	state_m->vd = state_m->vd_int + Ierr_d * conf_now->foc_current_kp;
	state_m->vq = state_m->vq_int + Ierr_q * conf_now->foc_current_kp;

	// Temperature compensation
	const float t = mc_interface_temp_motor_filtered();
	float ki = conf_now->foc_current_ki;
	if (conf_now->foc_temp_comp && t > -5.0) {
		ki += ki * 0.00386 * (t - conf_now->foc_temp_comp_base_temp);
	}

	state_m->vd_int += Ierr_d * (ki * dt);
	state_m->vq_int += Ierr_q * (ki * dt);

	// Decoupling
	float dec_vd = 0.0;
	float dec_vq = 0.0;
	float dec_bemf = 0.0;

	if (motor->m_control_mode < CONTROL_MODE_HANDBRAKE && conf_now->foc_cc_decoupling != FOC_CC_DECOUPLING_DISABLED) {
		switch (conf_now->foc_cc_decoupling) {
		case FOC_CC_DECOUPLING_CROSS:
			dec_vd = state_m->iq * state_m->speed_rad_s * conf_now->foc_motor_l * (3.0 / 2.0);
			dec_vq = state_m->id * state_m->speed_rad_s * conf_now->foc_motor_l * (3.0 / 2.0);
			break;

		case FOC_CC_DECOUPLING_BEMF:
			dec_bemf = state_m->speed_rad_s * conf_now->foc_motor_flux_linkage;
			break;

		case FOC_CC_DECOUPLING_CROSS_BEMF:
			dec_vd = state_m->iq * state_m->speed_rad_s * conf_now->foc_motor_l * (3.0 / 2.0);
			dec_vq = state_m->id * state_m->speed_rad_s * conf_now->foc_motor_l * (3.0 / 2.0);
			dec_bemf = state_m->speed_rad_s * conf_now->foc_motor_flux_linkage;
			break;

		default:
			break;
		}
	}

	state_m->vd -= dec_vd;
	state_m->vq += dec_vq + dec_bemf;

	float max_v_mag = (2.0 / 3.0) * max_duty * SQRT3_BY_2 * state_m->v_bus;

	// Saturation
	utils_saturate_vector_2d((float*)&state_m->vd, (float*)&state_m->vq, max_v_mag);
	state_m->mod_d = state_m->vd / ((2.0 / 3.0) * state_m->v_bus);
	state_m->mod_q = state_m->vq / ((2.0 / 3.0) * state_m->v_bus);

	// Integrator windup protection
	// This is important, tricky and probably needs improvement.
	// Currently we start by truncating the d-axis and then the q-axis with the magnitude that is
	// left. Axis decoupling is taken into account in the truncation. How to do that best is also
	// an open question...

	// Take both cross and back emf decoupling into consideration. Seems to make the control
	// noisy at full modulation.
//	utils_truncate_number((float*)&state_m->vd_int, -max_v_mag + dec_vd, max_v_mag + dec_vd);
//	float mag_left = sqrtf(SQ(max_v_mag) - SQ(state_m->vd_int - dec_vd));
//	utils_truncate_number((float*)&state_m->vq_int, -mag_left - (dec_vq + dec_bemf), mag_left - (dec_vq + dec_bemf));

	// Take only back emf decoupling into consideration. Seems to work best.
	utils_truncate_number((float*)&state_m->vd_int, -max_v_mag, max_v_mag);
	float mag_left = sqrtf(SQ(max_v_mag) - SQ(state_m->vd_int));
	utils_truncate_number((float*)&state_m->vq_int, -mag_left - dec_bemf, mag_left - dec_bemf);

	// Ignore decoupling. Works badly when back emf decoupling is used, probably not
	// the best way to go.
//	utils_truncate_number((float*)&state_m->vd_int, -max_v_mag, max_v_mag);
//	float mag_left = sqrtf(SQ(max_v_mag) - SQ(state_m->vd_int));
//	utils_truncate_number((float*)&state_m->vq_int, -mag_left, mag_left);

	// This is how anti-windup was done in FW < 4.0. Does not work well when there is too much D axis voltage.
//	utils_truncate_number((float*)&state_m->vd_int, -max_v_mag, max_v_mag);
//	utils_truncate_number((float*)&state_m->vq_int, -max_v_mag, max_v_mag);

	// TODO: Have a look at this?
	state_m->i_bus = state_m->mod_d * state_m->id + state_m->mod_q * state_m->iq;
	state_m->i_abs = sqrtf(SQ(state_m->id) + SQ(state_m->iq));
	state_m->i_abs_filter = sqrtf(SQ(state_m->id_filter) + SQ(state_m->iq_filter));

	float mod_alpha = c * state_m->mod_d - s * state_m->mod_q;
	float mod_beta  = c * state_m->mod_q + s * state_m->mod_d;

	// Deadtime compensation
	const float i_alpha_filter = c * state_m->id_target - s * state_m->iq_target;
	const float i_beta_filter = c * state_m->iq_target + s * state_m->id_target;
	const float ia_filter = i_alpha_filter;
	const float ib_filter = -0.5 * i_alpha_filter + SQRT3_BY_2 * i_beta_filter;
	const float ic_filter = -0.5 * i_alpha_filter - SQRT3_BY_2 * i_beta_filter;
	const float mod_alpha_filter_sgn = (2.0 / 3.0) * SIGN(ia_filter) - (1.0 / 3.0) * SIGN(ib_filter) - (1.0 / 3.0) * SIGN(ic_filter);
	const float mod_beta_filter_sgn = ONE_BY_SQRT3 * SIGN(ib_filter) - ONE_BY_SQRT3 * SIGN(ic_filter);
	const float mod_comp_fact = conf_now->foc_dt_us * 1e-6 * conf_now->foc_f_sw;
	const float mod_alpha_comp = mod_alpha_filter_sgn * mod_comp_fact;
	const float mod_beta_comp = mod_beta_filter_sgn * mod_comp_fact;

	// Apply compensation here so that 0 duty cycle has no glitches.
	state_m->v_alpha = (mod_alpha - mod_alpha_comp) * (2.0 / 3.0) * state_m->v_bus;
	state_m->v_beta = (mod_beta - mod_beta_comp) * (2.0 / 3.0) * state_m->v_bus;
	state_m->vd = c * motor->m_motor_state.v_alpha + s * motor->m_motor_state.v_beta;
	state_m->vq = c * motor->m_motor_state.v_beta  - s * motor->m_motor_state.v_alpha;

	// HFI
	if (do_hfi) {
		CURRENT_FILTER_OFF();

		float mod_alpha_tmp = mod_alpha;
		float mod_beta_tmp = mod_beta;

		float hfi_voltage;
		if (motor->m_hfi.est_done_cnt < conf_now->foc_hfi_start_samples) {
			hfi_voltage = conf_now->foc_hfi_voltage_start;
		} else {
			hfi_voltage = utils_map(fabsf(state_m->iq), 0.0, conf_now->l_current_max,
									conf_now->foc_hfi_voltage_run, conf_now->foc_hfi_voltage_max);
		}

		utils_truncate_number_abs(&hfi_voltage, state_m->v_bus * (2.0 / 3.0) * 0.9);

		if (motor->m_hfi.is_samp_n) {
			float sample_now = (utils_tab_sin_32_1[motor->m_hfi.ind * motor->m_hfi.table_fact] * state_m->i_alpha -
					utils_tab_cos_32_1[motor->m_hfi.ind * motor->m_hfi.table_fact] * state_m->i_beta);
			float current_sample = sample_now - motor->m_hfi.prev_sample;

			motor->m_hfi.buffer_current[motor->m_hfi.ind] = current_sample;

			if (current_sample > 0.01) {
				motor->m_hfi.buffer[motor->m_hfi.ind] = ((hfi_voltage / 2.0 - conf_now->foc_motor_r *
						current_sample) / (conf_now->foc_f_sw * current_sample));
			}

			motor->m_hfi.ind++;
			if (motor->m_hfi.ind == motor->m_hfi.samples) {
				motor->m_hfi.ind = 0;
				motor->m_hfi.ready = true;
			}

			mod_alpha_tmp += hfi_voltage * utils_tab_sin_32_1[motor->m_hfi.ind * motor->m_hfi.table_fact] / ((2.0 / 3.0) * state_m->v_bus);
			mod_beta_tmp -= hfi_voltage * utils_tab_cos_32_1[motor->m_hfi.ind * motor->m_hfi.table_fact] / ((2.0 / 3.0) * state_m->v_bus);
		} else {
			motor->m_hfi.prev_sample = utils_tab_sin_32_1[motor->m_hfi.ind * motor->m_hfi.table_fact] * state_m->i_alpha -
					utils_tab_cos_32_1[motor->m_hfi.ind * motor->m_hfi.table_fact] * state_m->i_beta;

			mod_alpha_tmp -= hfi_voltage * utils_tab_sin_32_1[motor->m_hfi.ind * motor->m_hfi.table_fact] / ((2.0 / 3.0) * state_m->v_bus);
			mod_beta_tmp += hfi_voltage * utils_tab_cos_32_1[motor->m_hfi.ind * motor->m_hfi.table_fact] / ((2.0 / 3.0) * state_m->v_bus);
		}

		utils_saturate_vector_2d(&mod_alpha_tmp, &mod_beta_tmp, SQRT3_BY_2 * 0.95);
		motor->m_hfi.is_samp_n = !motor->m_hfi.is_samp_n;

		if (conf_now->foc_sample_v0_v7) {
			mod_alpha = mod_alpha_tmp;
			mod_beta = mod_beta_tmp;
		} else {
			// Delay adding the HFI voltage when not sampling in both 0 vectors, as it will cancel
			// itself with the opposite pulse from the previous HFI sample. This makes more sense
			// when drawing the SVM waveform.
			svm(-mod_alpha_tmp, -mod_beta_tmp, TIM1->ARR,
				(uint32_t*)&motor->m_duty1_next,
				(uint32_t*)&motor->m_duty2_next,
				(uint32_t*)&motor->m_duty3_next,
				(uint32_t*)&state_m->svm_sector);
			motor->m_duty_next_set = true;
		}
	} else {
		CURRENT_FILTER_ON();
		motor->m_hfi.ind = 0;
		motor->m_hfi.ready = false;
		motor->m_hfi.is_samp_n = false;
		motor->m_hfi.prev_sample = 0.0;
	}

	// Set output (HW Dependent)
	uint32_t duty1, duty2, duty3, top;
	top = TIM1->ARR;
	svm(-mod_alpha, -mod_beta, top, &duty1, &duty2, &duty3, (uint32_t*)&state_m->svm_sector);

	if (motor == &m_motor_1) {
		TIMER_UPDATE_DUTY_M1(duty1, duty2, duty3);
#ifdef HW_HAS_DUAL_PARALLEL
		TIMER_UPDATE_DUTY_M2(duty1, duty2, duty3);
#endif
	} else {
#ifndef HW_HAS_DUAL_PARALLEL
		TIMER_UPDATE_DUTY_M2(duty1, duty2, duty3);
#endif
	}

	// do not allow to turn on PWM outputs if virtual motor is used
	if(virtual_motor_is_connected() == false) {
		if (!motor->m_output_on) {
			start_pwm_hw(motor);
		}
	}
}

// Magnitude must not be larger than sqrt(3)/2, or 0.866
static void svm(float alpha, float beta, uint32_t PWMHalfPeriod,
				uint32_t* tAout, uint32_t* tBout, uint32_t* tCout, uint32_t *svm_sector) {
	uint32_t sector;

	if (beta >= 0.0f) {
		if (alpha >= 0.0f) {
			//quadrant I
			if (ONE_BY_SQRT3 * beta > alpha) {
				sector = 2;
			} else {
				sector = 1;
			}
		} else {
			//quadrant II
			if (-ONE_BY_SQRT3 * beta > alpha) {
				sector = 3;
			} else {
				sector = 2;
			}
		}
	} else {
		if (alpha >= 0.0f) {
			//quadrant IV5
			if (-ONE_BY_SQRT3 * beta > alpha) {
				sector = 5;
			} else {
				sector = 6;
			}
		} else {
			//quadrant III
			if (ONE_BY_SQRT3 * beta > alpha) {
				sector = 4;
			} else {
				sector = 5;
			}
		}
	}

	// PWM timings
	uint32_t tA, tB, tC;

	switch (sector) {

	// sector 1-2
	case 1: {
		// Vector on-times
		uint32_t t1 = (alpha - ONE_BY_SQRT3 * beta) * PWMHalfPeriod;
		uint32_t t2 = (TWO_BY_SQRT3 * beta) * PWMHalfPeriod;

		// PWM timings
		tA = (PWMHalfPeriod - t1 - t2) / 2;
		tB = tA + t1;
		tC = tB + t2;

		break;
	}

	// sector 2-3
	case 2: {
		// Vector on-times
		uint32_t t2 = (alpha + ONE_BY_SQRT3 * beta) * PWMHalfPeriod;
		uint32_t t3 = (-alpha + ONE_BY_SQRT3 * beta) * PWMHalfPeriod;

		// PWM timings
		tB = (PWMHalfPeriod - t2 - t3) / 2;
		tA = tB + t3;
		tC = tA + t2;

		break;
	}

	// sector 3-4
	case 3: {
		// Vector on-times
		uint32_t t3 = (TWO_BY_SQRT3 * beta) * PWMHalfPeriod;
		uint32_t t4 = (-alpha - ONE_BY_SQRT3 * beta) * PWMHalfPeriod;

		// PWM timings
		tB = (PWMHalfPeriod - t3 - t4) / 2;
		tC = tB + t3;
		tA = tC + t4;

		break;
	}

	// sector 4-5
	case 4: {
		// Vector on-times
		uint32_t t4 = (-alpha + ONE_BY_SQRT3 * beta) * PWMHalfPeriod;
		uint32_t t5 = (-TWO_BY_SQRT3 * beta) * PWMHalfPeriod;

		// PWM timings
		tC = (PWMHalfPeriod - t4 - t5) / 2;
		tB = tC + t5;
		tA = tB + t4;

		break;
	}

	// sector 5-6
	case 5: {
		// Vector on-times
		uint32_t t5 = (-alpha - ONE_BY_SQRT3 * beta) * PWMHalfPeriod;
		uint32_t t6 = (alpha - ONE_BY_SQRT3 * beta) * PWMHalfPeriod;

		// PWM timings
		tC = (PWMHalfPeriod - t5 - t6) / 2;
		tA = tC + t5;
		tB = tA + t6;

		break;
	}

	// sector 6-1
	case 6: {
		// Vector on-times
		uint32_t t6 = (-TWO_BY_SQRT3 * beta) * PWMHalfPeriod;
		uint32_t t1 = (alpha + ONE_BY_SQRT3 * beta) * PWMHalfPeriod;

		// PWM timings
		tA = (PWMHalfPeriod - t6 - t1) / 2;
		tC = tA + t1;
		tB = tC + t6;

		break;
	}
	}

	*tAout = tA;
	*tBout = tB;
	*tCout = tC;
	*svm_sector = sector;
}

static void run_pid_control_pos(float angle_now, float angle_set, float dt, volatile motor_all_state_t *motor) {
	volatile mc_configuration *conf_now = motor->m_conf;
	float p_term;
	float d_term;

	// PID is off. Return.
	if (motor->m_control_mode != CONTROL_MODE_POS) {
		motor->m_pos_i_term = 0;
		motor->m_pos_prev_error = 0;
		return;
	}

	// Compute parameters
	float error = utils_angle_difference(angle_set, angle_now);

	if (encoder_is_configured()) {
		if (conf_now->foc_encoder_inverted) {
			error = -error;
		}
	}

	p_term = error * conf_now->p_pid_kp;
	motor->m_pos_i_term += error * (conf_now->p_pid_ki * dt);

	// Average DT for the D term when the error does not change. This likely
	// happens at low speed when the position resolution is low and several
	// control iterations run without position updates.
	// TODO: Are there problems with this approach?
	motor->m_pos_dt_int += dt;
	if (error == motor->m_pos_prev_error) {
		d_term = 0.0;
	} else {
		d_term = (error - motor->m_pos_prev_error) * (conf_now->p_pid_kd / motor->m_pos_dt_int);
		motor->m_pos_dt_int = 0.0;
	}

	// Filter D
	UTILS_LP_FAST(motor->m_pos_d_filter, d_term, conf_now->p_pid_kd_filter);
	d_term = motor->m_pos_d_filter;


	// I-term wind-up protection
	float p_tmp = p_term;
	utils_truncate_number_abs(&p_tmp, 1.0);
	utils_truncate_number_abs((float*)&motor->m_pos_i_term, 1.0 - fabsf(p_tmp));

	// Store previous error
	motor->m_pos_prev_error = error;

	// Calculate output
	float output = p_term + motor->m_pos_i_term + d_term;
	utils_truncate_number(&output, -1.0, 1.0);

	if (encoder_is_configured()) {
		if (encoder_index_found()) {
			motor->m_iq_set = output * conf_now->lo_current_max;
		} else {
			// Rotate the motor with 40 % power until the encoder index is found.
			motor->m_iq_set = 0.4 * conf_now->lo_current_max;
		}
	} else {
		motor->m_iq_set = output * conf_now->lo_current_max;
	}
}

static void run_pid_control_speed(float dt, volatile motor_all_state_t *motor) {
	volatile mc_configuration *conf_now = motor->m_conf;
	float p_term;
	float d_term;

	// PID is off. Return.
	if (motor->m_control_mode != CONTROL_MODE_SPEED) {
		motor->m_speed_i_term = 0.0;
		motor->m_speed_prev_error = 0.0;
		return;
	}

	const float rpm = mcpwm_foc_get_rpm();
	float error = motor->m_speed_pid_set_rpm - rpm;

	// Too low RPM set. Reset state and return.
	if (fabsf(motor->m_speed_pid_set_rpm) < conf_now->s_pid_min_erpm) {
		motor->m_speed_i_term = 0.0;
		motor->m_speed_prev_error = error;
		return;
	}

	// Compute parameters
	p_term = error * conf_now->s_pid_kp * (1.0 / 20.0);
	motor->m_speed_i_term += error * (conf_now->s_pid_ki * dt) * (1.0 / 20.0);
	d_term = (error - motor->m_speed_prev_error) * (conf_now->s_pid_kd / dt) * (1.0 / 20.0);

	// Filter D
	UTILS_LP_FAST(motor->m_speed_d_filter, d_term, conf_now->s_pid_kd_filter);
	d_term = motor->m_speed_d_filter;

	// I-term wind-up protection
	utils_truncate_number((float*)&motor->m_speed_i_term, -1.0, 1.0);

	// Store previous error
	motor->m_speed_prev_error = error;

	// Calculate output
	float output = p_term + motor->m_speed_i_term + d_term;
	utils_truncate_number(&output, -1.0, 1.0);

	// Optionally disable braking
	if (!conf_now->s_pid_allow_braking) {
		if (rpm > 20.0 && output < 0.0) {
			output = 0.0;
		}

		if (rpm < -20.0 && output > 0.0) {
			output = 0.0;
		}
	}

	motor->m_iq_set = output * conf_now->lo_current_max;
}

static void stop_pwm_hw(volatile motor_all_state_t *motor) {
	motor->m_id_set = 0.0;
	motor->m_iq_set = 0.0;

	if (motor == &m_motor_1) {
		TIM_SelectOCxM(TIM1, TIM_Channel_1, TIM_ForcedAction_InActive);
		TIM_CCxCmd(TIM1, TIM_Channel_1, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM1, TIM_Channel_1, TIM_CCxN_Disable);

		TIM_SelectOCxM(TIM1, TIM_Channel_2, TIM_ForcedAction_InActive);
		TIM_CCxCmd(TIM1, TIM_Channel_2, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM1, TIM_Channel_2, TIM_CCxN_Disable);

		TIM_SelectOCxM(TIM1, TIM_Channel_3, TIM_ForcedAction_InActive);
		TIM_CCxCmd(TIM1, TIM_Channel_3, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM1, TIM_Channel_3, TIM_CCxN_Disable);

		TIM_GenerateEvent(TIM1, TIM_EventSource_COM);

#ifdef HW_HAS_DUAL_PARALLEL
		TIM_SelectOCxM(TIM8, TIM_Channel_1, TIM_ForcedAction_InActive);
		TIM_CCxCmd(TIM8, TIM_Channel_1, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_1, TIM_CCxN_Disable);

		TIM_SelectOCxM(TIM8, TIM_Channel_2, TIM_ForcedAction_InActive);
		TIM_CCxCmd(TIM8, TIM_Channel_2, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_2, TIM_CCxN_Disable);

		TIM_SelectOCxM(TIM8, TIM_Channel_3, TIM_ForcedAction_InActive);
		TIM_CCxCmd(TIM8, TIM_Channel_3, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_3, TIM_CCxN_Disable);

		TIM_GenerateEvent(TIM8, TIM_EventSource_COM);
#endif

#ifdef HW_HAS_DRV8313
		DISABLE_BR();
#endif

		motor->m_output_on = false;
	} else {
		TIM_SelectOCxM(TIM8, TIM_Channel_1, TIM_ForcedAction_InActive);
		TIM_CCxCmd(TIM8, TIM_Channel_1, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_1, TIM_CCxN_Disable);

		TIM_SelectOCxM(TIM8, TIM_Channel_2, TIM_ForcedAction_InActive);
		TIM_CCxCmd(TIM8, TIM_Channel_2, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_2, TIM_CCxN_Disable);

		TIM_SelectOCxM(TIM8, TIM_Channel_3, TIM_ForcedAction_InActive);
		TIM_CCxCmd(TIM8, TIM_Channel_3, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_3, TIM_CCxN_Disable);

		TIM_GenerateEvent(TIM8, TIM_EventSource_COM);

#ifdef HW_HAS_DRV8313_2
		DISABLE_BR_2();
#endif

		motor->m_output_on = false;
	}
}

static void start_pwm_hw(volatile motor_all_state_t *motor) {
	if (motor == &m_motor_1) {
		TIM_SelectOCxM(TIM1, TIM_Channel_1, TIM_OCMode_PWM1);
		TIM_CCxCmd(TIM1, TIM_Channel_1, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM1, TIM_Channel_1, TIM_CCxN_Enable);

		TIM_SelectOCxM(TIM1, TIM_Channel_2, TIM_OCMode_PWM1);
		TIM_CCxCmd(TIM1, TIM_Channel_2, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM1, TIM_Channel_2, TIM_CCxN_Enable);

		TIM_SelectOCxM(TIM1, TIM_Channel_3, TIM_OCMode_PWM1);
		TIM_CCxCmd(TIM1, TIM_Channel_3, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM1, TIM_Channel_3, TIM_CCxN_Enable);

#ifdef HW_HAS_DUAL_PARALLEL
		TIM_SelectOCxM(TIM8, TIM_Channel_1, TIM_OCMode_PWM1);
		TIM_CCxCmd(TIM8, TIM_Channel_1, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_1, TIM_CCxN_Enable);

		TIM_SelectOCxM(TIM8, TIM_Channel_2, TIM_OCMode_PWM1);
		TIM_CCxCmd(TIM8, TIM_Channel_2, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_2, TIM_CCxN_Enable);

		TIM_SelectOCxM(TIM8, TIM_Channel_3, TIM_OCMode_PWM1);
		TIM_CCxCmd(TIM8, TIM_Channel_3, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_3, TIM_CCxN_Enable);
#endif

		// Generate COM event in ADC interrupt to get better synchronization
		//	TIM_GenerateEvent(TIM1, TIM_EventSource_COM);

#ifdef HW_HAS_DRV8313
		ENABLE_BR();
#endif
		motor->m_output_on = true;
	} else {
		TIM_SelectOCxM(TIM8, TIM_Channel_1, TIM_OCMode_PWM1);
		TIM_CCxCmd(TIM8, TIM_Channel_1, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_1, TIM_CCxN_Enable);

		TIM_SelectOCxM(TIM8, TIM_Channel_2, TIM_OCMode_PWM1);
		TIM_CCxCmd(TIM8, TIM_Channel_2, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_2, TIM_CCxN_Enable);

		TIM_SelectOCxM(TIM8, TIM_Channel_3, TIM_OCMode_PWM1);
		TIM_CCxCmd(TIM8, TIM_Channel_3, TIM_CCx_Enable);
		TIM_CCxNCmd(TIM8, TIM_Channel_3, TIM_CCxN_Enable);

#ifdef HW_HAS_DRV8313_2
		ENABLE_BR_2();
#endif
		motor->m_output_on = true;
	}
}

static float correct_encoder(float obs_angle, float enc_angle, float speed,
							 float sl_erpm, volatile motor_all_state_t *motor) {
	float rpm_abs = fabsf(speed / ((2.0 * M_PI) / 60.0));

	// Hysteresis 5 % of total speed
	float hyst = sl_erpm * 0.05;
	if (motor->m_using_encoder) {
		if (rpm_abs > (sl_erpm + hyst)) {
			motor->m_using_encoder = false;
		}
	} else {
		if (rpm_abs < (sl_erpm- hyst)) {
			motor->m_using_encoder = true;
		}
	}

	return motor->m_using_encoder ? enc_angle : obs_angle;
}

static float correct_hall(float angle, float dt, volatile motor_all_state_t *motor) {
	volatile mc_configuration *conf_now = motor->m_conf;
	motor->m_hall_dt_diff_now += dt;

	float rad_per_sec = (M_PI / 3.0) / motor->m_hall_dt_diff_last;
	float rpm_abs_fast = fabsf(motor->m_speed_est_fast / ((2.0 * M_PI) / 60.0));
	float rpm_abs_hall = fabsf(rad_per_sec / ((2.0 * M_PI) / 60.0));

	// Hysteresis 5 % of total speed
	float hyst = conf_now->foc_sl_erpm * 0.1;
	if (motor->m_using_hall) {
		if (fminf(rpm_abs_fast, rpm_abs_hall) > (conf_now->foc_sl_erpm + hyst)) {
			motor->m_using_hall = false;
		}
	} else {
		if (rpm_abs_fast < (conf_now->foc_sl_erpm - hyst)) {
			motor->m_using_hall = true;
		}
	}

	int ang_hall_int = conf_now->foc_hall_table[utils_read_hall(motor != &m_motor_1)];

	// Only override the observer if the hall sensor value is valid.
	if (ang_hall_int < 201) {
		float ang_hall_now = (((float)ang_hall_int / 200.0) * 360.0) * M_PI / 180.0;

		if (motor->m_ang_hall_int_prev < 0) {
			// Previous angle not valid
			motor->m_ang_hall_int_prev = ang_hall_int;
			motor->m_ang_hall = ang_hall_now;
		} else if (ang_hall_int != motor->m_ang_hall_int_prev) {
			int diff = ang_hall_int - motor->m_ang_hall_int_prev;
			if (diff > 100) {
				diff -= 200;
			} else if (diff < -100) {
				diff += 200;
			}

			// This is only valid if the direction did not just change. If it did, we use the
			// last speed together with the sign right now.
			if (SIGN(diff) == SIGN(motor->m_hall_dt_diff_last)) {
				if (diff > 0) {
					motor->m_hall_dt_diff_last = motor->m_hall_dt_diff_now;
				} else {
					motor->m_hall_dt_diff_last = -motor->m_hall_dt_diff_now;
				}
			} else {
				motor->m_hall_dt_diff_last = -motor->m_hall_dt_diff_last;
			}

			motor->m_hall_dt_diff_now = 0.0;

			// A transition was just made. The angle is in the middle of the new and old angle.
			int ang_avg = motor->m_ang_hall_int_prev + diff / 2;
			ang_avg %= 200;
			motor->m_ang_hall = (((float)ang_avg / 200.0) * 360.0) * M_PI / 180.0;
		}

		motor->m_ang_hall_int_prev = ang_hall_int;

		if (((60.0 / (2.0 * M_PI)) * ((M_PI / 3.0) / motor->m_hall_dt_diff_now)) < 100) {
			// Don't interpolate on very low speed, just use the closest hall sensor. The reason is that we might
			// get stuck at 60 degrees off if a direction change happens between two steps.
			motor->m_ang_hall = ang_hall_now;
		} else {
			// Interpolate
			float diff = utils_angle_difference_rad(motor->m_ang_hall, ang_hall_now);
			if (fabsf(diff) < ((2.0 * M_PI) / 12.0)) {
				// Do interpolation
				motor->m_ang_hall += rad_per_sec * dt;
			} else {
				// We are too far away with the interpolation
				motor->m_ang_hall -= diff / 100.0;
			}
		}

		utils_norm_angle_rad((float*)&motor->m_ang_hall);
		if (motor->m_using_hall) {
			angle = motor->m_ang_hall;
		}
	} else {
		// Invalid hall reading. Don't update angle.
		motor->m_ang_hall_int_prev = -1;

		// Also allow open loop in order to behave like normal sensorless
		// operation. Then the motor works even if the hall sensor cable
		// gets disconnected (when the sensor spacing is 120 degrees).
		if (motor->m_phase_observer_override && motor->m_state == MC_STATE_RUNNING) {
			angle = motor->m_phase_now_observer_override;
		}
	}

	return angle;
}

static void terminal_plot_hfi(int argc, const char **argv) {
	if (argc == 2) {
		int d = -1;
		sscanf(argv[1], "%d", &d);

		if (d == 0 || d == 1 || d == 2) {
			motor_now()->m_hfi_plot_en = d;
			if (motor_now()->m_hfi_plot_en == 1) {
				motor_now()->m_hfi_plot_sample = 0.0;
				commands_init_plot("Sample", "Value");
				commands_plot_add_graph("Phase");
				commands_plot_add_graph("Phase bin2");
				commands_plot_add_graph("Ld - Lq (uH");
				commands_plot_add_graph("L Diff Sat (uH)");
				commands_plot_add_graph("L Avg (uH)");
			} else if (motor_now()->m_hfi_plot_en == 2) {
				motor_now()->m_hfi_plot_sample = 0.0;
				commands_init_plot("Sample Index", "Value");
				commands_plot_add_graph("Current (A)");
				commands_plot_add_graph("Inductance (uH)");
			}

			commands_printf(motor_now()->m_hfi_plot_en ?
					"HFI plot enabled" :
					"HFI plot disabled");
		} else {
			commands_printf("Invalid Argument. en has to be 0, 1 or 2.\n");
		}
	} else {
		commands_printf("This command requires one argument.\n");
	}
}
