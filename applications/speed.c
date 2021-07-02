/*
 Copyright 2019 Claroworks

 written by Mike Wilson mail4mikew@gmail.com

 This file is part of an application designed to work with VESC firmware,
 and is intended for use with dive propulsion vehicles.

 This firmware is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This firmware is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// speed.c
//
// This file defines a thread that takes speed events and performs timing and
//		logic to determine motor speed actions
//
#include <stddef.h>
#include <stdbool.h>

#include "ch.h" // ChibiOS
#include "hal.h" // ChibiOS HAL
#include "mc_interface.h" // Motor control functions

#include "commands.h"
#include "terminal.h"

#include "batteries.h"
#include "settings.h"
#include "display.h"
#include "speed.h"

#define QUEUE_SZ 4
static msg_t msg_queue[QUEUE_SZ];
mailbox_t speed_mbox;

static msg_t msg_queue2[QUEUE_SZ];
mailbox_t ready_mbox;

static sikorski_data *settings;

#define SPED_LOG(a) if(settings->logging & SPEED_LOG) commands_printf a
#define SAFE_LOG(a) if(settings->logging & SAFETY_LOG) commands_printf a

typedef enum _motor_state
{
    MOTOR_OFF = 0,		// motor not running
    MOTOR_ON,			// motor running
    MOTOR_START,		// test for obstruction
    MOTOR_EOL
} MOTOR_STATE;

const char *const motor_states[] =
    { "MOTOR_OFF", "MOTOR_ON", "MOTOR_START"};

static THD_FUNCTION(speed_thread, arg);
static THD_WORKING_AREA(speed_thread_wa, 2048);

/*----------------------------------------------------*/
/*   Helper 'READY' thread monitors motor current     */
/*----------------------------------------------------*/

static THD_FUNCTION(motor_ready_thread, arg);
static THD_WORKING_AREA(ready_thread_wa, 2048);

void send_to_ready (MESSAGE event)
{
    (void) chMBPost (&ready_mbox, (msg_t) event, TIME_IMMEDIATE);
}

/*--------------------------------------------*/

void speed_init (void)
{
    // Start speed thread.
    chThdCreateStatic (speed_thread_wa, sizeof(speed_thread_wa), NORMALPRIO, speed_thread, NULL);
    // message queue
    chMBObjectInit (&speed_mbox, msg_queue, QUEUE_SZ);

    // Start ready thread, which reads the motor current to determine if scooter is in water.
    chThdCreateStatic (ready_thread_wa, sizeof(ready_thread_wa), NORMALPRIO, motor_ready_thread, NULL);
    // message queue
    chMBObjectInit (&ready_mbox, msg_queue2, QUEUE_SZ);
}

#define DEFAULT_SPEED ((settings->speed_default) -1)

void send_to_speed (MESSAGE event)
{
    (void) chMBPost (&speed_mbox, (msg_t) event, TIME_IMMEDIATE);
}

static void increase (uint8_t *speed)
{
    if (*speed >= settings->max_speed - 1)
        *speed = settings->max_speed - 1;
    else (*speed)++;
}

static void decrease (uint8_t *speed)
{
    if (*speed > 0)
        (*speed)--;
}

static bool migrate (uint8_t *speed)		// Programmed speed migrates toward the default speed. Return true if it got there.
{
    if (*speed > DEFAULT_SPEED)
        (*speed)--;
    else if (*speed < DEFAULT_SPEED)
        if(settings->low_migrate == 1)  // Enable low migrate
			return true;
		else
			(*speed)++;
    if (*speed == DEFAULT_SPEED)
        return true;
    return false;
}

#define RAMPING_TIME_MS 50.0 // mS - 20 times per second
#define CHECK_BATTERY_PERIOD_MS 5000 // check the battery every 5 seconds while running

// return next speed to send, ramping from the current speed
// toward the programmed speed.
static float ramping (float present, float programmed)
{
    const float delta = ((float) settings->ramping) / (1000.0 / RAMPING_TIME_MS);
    float diff = programmed - present;

    if (diff > delta)
        return present + delta;

    if (diff < -delta)
        return present - delta;

    return programmed;
}

static void set_max_current (float max_current)
{
    mc_configuration *conf = (mc_configuration*) mc_interface_get_configuration ();
    conf->l_in_current_max = max_current;
    conf->lo_in_current_max = max_current;
}

// this limits the motor attempting to 'catch up' with it's programmed position when
// it gets blocked (especially in safety mode), otherwise it lurches up attempting to recover
static void set_max_ERPM (float max_ERPM)
{
    mc_configuration *conf = (mc_configuration*) mc_interface_get_configuration ();
    conf->l_max_erpm = max_ERPM;
}

typedef enum _run_modes
{
    MODE_OFF, MODE_RUN, MODE_START
} RUN_MODES;

// given the desired speed, use the battery voltage to determine
// if the speed should be limited. Limit the speed relative
// to the battery voltage in the case where the display shows 1 bar.
static float limit_speed_by_battery(float speed)
{
    float lowest_battery = get_lowest_battery_voltage();

    mc_configuration *conf = (mc_configuration*) mc_interface_get_configuration ();

    // if speed limit is not enabled, just return original speed
    if(settings->b2Rratio == 0.0)
    {
        return speed;
    }

    float batt_cutoff = conf->l_battery_cut_end / 2.0;
    float batt_low = conf->l_battery_cut_start / 2.0;

    SPED_LOG(("cutoff=%2.2f batt=%2.2f low=%2.2f",
                    (double) batt_cutoff, (double) lowest_battery, (double) batt_low));
    if(lowest_battery > batt_low)
        return speed;   // no need to limit speed

    // if really dead, disable the motor. (although this is redundant -
    // the motor should be disabled by the motor control code as well)
    if(lowest_battery <= batt_cutoff)
        return 0.0;

    // the batteries are 'low'. so limit the speed based on battery voltage
    // within the ERPM_range. Create formula needed in form  y=mx+b
    // to compute the limited speed

    // m = dY/dX:
    float m = (speed) / (batt_low - batt_cutoff);

    // solve for b: b = y0 - m * x0
    float b = 0 - (m * batt_cutoff);

    // apply formula to look up the appropriate speed for this voltage
    float limited_speed = (lowest_battery * m) + b;
    SPED_LOG(("speed=%4.2f, limited=%4.2f by batt=%2.2f (2x)",
                (double) speed, (double) limited_speed, (double) (lowest_battery * 2.0)));

    return limited_speed;
}

float get_limited_speed(uint8_t user_setting)
{
    float user_speed = settings->speeds[user_setting];
    return limit_speed_by_battery(user_speed);
}

// adjust the speed including ramping, and return true if the ramping is complete.
static float adjust_speed (uint8_t user_setting, RUN_MODES mode)
{
    static float present_speed = 0.0;      // speed that motor is set to.

    if (mode == MODE_OFF)
    {
        present_speed = 0.0;
        mc_interface_release_motor ();
        set_max_current(settings->limits[0]);
        return present_speed;
    }
    else if (mode == MODE_START)
    {
        set_max_current(settings->guard_limit);
        present_speed = settings->guard_erpm;
        present_speed = limit_speed_by_battery(present_speed);
        set_max_ERPM(settings->guard_max_erpm);
        mc_interface_set_pid_speed (present_speed);
        return present_speed;
    }
    else // mode == MODE_RUN
    {
        #define RUNNING_MAX_ERPM  100000 // MAX ERPM when running is just large and unrestrained.
        set_max_ERPM(RUNNING_MAX_ERPM);
        set_max_current(settings->limits[user_setting]);

        // ramp from the present speed toward the desired speed from user setting
        present_speed = ramping (present_speed, get_limited_speed(user_setting));

        mc_interface_set_pid_speed (present_speed);
    }

    #if(SPEED_LOG == 1)
        const char *const mode_str[] = { "OFF", "RUN", "START" };
    #endif
    SPED_LOG(("MODE=%.5s present=%4.2f, programmed=%4.2f",
            mode_str[(int) mode], (double) present_speed, (double) get_limited_speed(user_setting)));

return    present_speed;
}

// timeout value (used as a timeout service)
static systime_t speed_timeout = TIME_INFINITE; // timeout in ticks. Use MS2ST(milliseconds) to set the value in milliseconds
static void set_timeout(systime_t new_period)
{
    speed_timeout = new_period;
    SPED_LOG(("TIMEOUT = %f", ((double)ST2MS(new_period))/(double)1000.0 ));
}

static THD_FUNCTION(speed_thread, arg) // @suppress("No return")
{
    (void) arg;

    chRegSetThreadName ("SPEED");

    MOTOR_STATE state = MOTOR_OFF;

    // the message retrieved from the mailbox
    msg_t fetch = MSG_OK;

    int32_t event = SPEED_OFF;

    float present_speed = 0.0;		// speed that motor is set to.

    settings = get_sikorski_settings_ptr ();
    while(settings->magic != VALID_VALUE)
    {
        chThdSleepMilliseconds(50);   // sleep long enough for other applications to be online
    }

    uint8_t user_speed = DEFAULT_SPEED; // the index to the speed setting. Always start out in the default speed

    for (;;)
    {

        fetch = chMBFetch (&speed_mbox, (msg_t*) &event, speed_timeout);
        if (fetch == MSG_TIMEOUT)
            event = TIMER_EXPIRY;

        SPED_LOG(("SPEED = %s, Event = %s", motor_states[state], message_text (event)));

        MOTOR_STATE old_state = state;

        switch (state)
        {
        case MOTOR_OFF:
            switch (event)
            {
            case SPEED_ON:
                send_to_display (DISP_ON_TRIGGER);
                if (settings->use_safety && user_speed == DEFAULT_SPEED) // if off trigger for a "long time", or, haven't been in water yet...
                {
                    state = MOTOR_START;
                    send_to_ready (READY_ON);
                }
                else
                {
                    state = MOTOR_ON;
                    set_timeout(MS2ST(RAMPING_TIME_MS)); // start running this thread fast to achieve ramping.

                    // start ramping up to the first speed.
                    adjust_speed (user_speed, MODE_RUN);
                    send_to_display (DISP_SPEED_1 + user_speed);
                }
                break;
            case CHECK_BATTERY:
                break;
            case TIMER_EXPIRY:
                set_timeout(migrate (&user_speed) ? TIME_INFINITE : MS2ST(settings->migrate_rate));
                break;
            default:
                break;
            }
            break;
        case MOTOR_ON:
            switch (event)
            {
            case SPEED_OFF:
                state = MOTOR_OFF;
                send_to_display (DISP_OFF_TRIGGER);
                set_timeout(MS2ST(settings->migrate_rate));
                adjust_speed (user_speed, MODE_OFF);
                break;
            case SPEED_UP:
                increase (&user_speed);
                adjust_speed (user_speed, MODE_RUN);
                send_to_display (DISP_SPEED_1 + user_speed);
                set_timeout(MS2ST(RAMPING_TIME_MS));
                break;
            case SPEED_DOWN:
                decrease (&user_speed);
                adjust_speed (user_speed, MODE_RUN);
                send_to_display (DISP_SPEED_1 + user_speed);
                set_timeout(MS2ST(RAMPING_TIME_MS));
                break;
            case JUMP_SPEED: //new jump speed
                user_speed = settings->jump_speed -1;
                adjust_speed (user_speed, MODE_RUN);
                send_to_display (DISP_SPEED_1 + user_speed);
                set_timeout(MS2ST(RAMPING_TIME_MS));
                break;
			case REVERSE_SPEED: // Reverse speed
                user_speed = settings->(-(reverse_speed));
                adjust_speed (user_speed, MODE_RUN);
                set_timeout(MS2ST(RAMPING_TIME_MS));
                break;
            case CHECK_BATTERY:
                adjust_speed (user_speed, MODE_RUN);
                break;
            case TIMER_EXPIRY: // runs often while ramping
                present_speed = adjust_speed (user_speed, MODE_RUN);    // ramping is taken care of in this function
                if (present_speed == get_limited_speed(user_speed))
                    set_timeout(MS2ST(CHECK_BATTERY_PERIOD_MS));
                else
                    set_timeout(MS2ST(RAMPING_TIME_MS));
                break;
            default:
                break;
            }
            break;
        case MOTOR_START: // wait here until it is indicated that we are running in water with no obstructions
            switch (event)
            {
            case SPEED_OFF:		// user gave up and turned motor off.
                state = MOTOR_OFF;
                send_to_display (DISP_OFF_TRIGGER);
                set_timeout(MS2ST(settings->migrate_rate));
                adjust_speed (user_speed, MODE_OFF);
                send_to_ready (READY_OFF);		// don't need to check for start any more
                break;

            case SPEED_READY:   // READY thread approved start conditions
                state = MOTOR_ON;
                adjust_speed (user_speed, MODE_RUN);
                send_to_display (DISP_ON_TRIGGER);
                send_to_display (DISP_SPEED_1 + user_speed);
                set_timeout(MS2ST(RAMPING_TIME_MS)); // start running this thread fast to achieve ramping.
                break;
            case CHECK_BATTERY:
                adjust_speed (user_speed, MODE_START);
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }

        if (old_state != state)
            SPED_LOG(("NEW State = %s", motor_states[state]));
    }
}

/*------------ LPF Object -----------------*/
// LOW PASS FILTER (Leaky Integrator)
typedef struct _lpf_context
{
    float y;
    float alpha;
} LPF_CONTEXT;

static void lpf_init (LPF_CONTEXT *lpf_context, float alpha, float input)
{
    lpf_context->alpha = alpha;
    lpf_context->y = input;
}

static float lpf_sample (LPF_CONTEXT *lpf_context, float input)
{
    lpf_context->y += lpf_context->alpha * (input - lpf_context->y);
    return lpf_context->y;
}

/*------------ Motor Ready Thread -----------------*/

static THD_FUNCTION(motor_ready_thread, arg) // @suppress("No return")
{
    (void) arg;

    chRegSetThreadName ("READY");

    // the message retrieved from the mailbox
    msg_t fetch = MSG_OK;

    int32_t event = SPEED_OFF;
    uint8_t running_safe_ct = 0; // count up when safety band is achieved. If it gets above threshold, indicate to
                                 // speed thread that it is safe to run full speed
    uint8_t running_fail_ct = 0; // count up when safety band is exceeded (obstruction detected)

    // timeout value (used as a timeout service)
    systime_t ready_timeout = TIME_INFINITE; // timeout in ticks. Use MS2ST(milliseconds) to set the value in milliseconds

    // read the motor current, and see if the blades are unobstructed and the scooter is in water
    float motor_amps = 0.0;
    float filtered;
    LPF_CONTEXT lpfy;
    lpf_init (&lpfy, settings->f_alpha, 0.0);

    // Delay this task starting so that settings are updated
    fetch = chMBFetch (&ready_mbox, (msg_t*) &event, MS2ST(500/*mSec*/));

    for (;;)
    {
        fetch = chMBFetch (&ready_mbox, (msg_t*) &event, ready_timeout);
        if (fetch == MSG_TIMEOUT)
            event = TIMER_EXPIRY;

        switch (event)
        {
        case READY_ON:
            adjust_speed (0, MODE_START);
            ready_timeout = MS2ST(RAMPING_TIME_MS);	// 50 mS = 20Hz
            running_safe_ct = 0;
            lpf_init (&lpfy, settings->f_alpha, 1.0);
            break;

        case READY_OFF:
            ready_timeout = TIME_INFINITE;	// turn task 'OFF', until turned on again.
            running_safe_ct = 0;
            break;

        case TIMER_EXPIRY:
            motor_amps = mc_interface_get_tot_current_filtered ();
            filtered = lpf_sample (&lpfy, motor_amps);

            if (filtered < settings->guard_high)
                running_fail_ct = 0;
            else {
                running_fail_ct += 1;
                if (running_fail_ct > settings->fail_count)
                {
                    send_to_speed (SPEED_OFF);
                    ready_timeout = TIME_INFINITE;
                }
            }

            if (filtered > settings->guard_high || filtered < settings->guard_low)
                running_safe_ct = 0;
            else
            {
                running_safe_ct += 1;
                if (running_safe_ct > settings->safe_count)
                {
                    send_to_speed (SPEED_READY);
                    ready_timeout = TIME_INFINITE;
                }
            }
            SAFE_LOG(("SAFETY: Amps: %f, %f (%i)", (double) motor_amps, (double) filtered, running_safe_ct));
            break;
        default:
            break;
        }
    }
}

