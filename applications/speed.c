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
    MOTOR_KILLED,       // permanent motor off (battery dead)
    MOTOR_EOL
} MOTOR_STATE;

const char *const motor_states[] =
    { "MOTOR_OFF", "MOTOR_ON", "MOTOR_START", "MOTOR_KILLED"};

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
        (*speed)++;
    if (*speed == DEFAULT_SPEED)
        return true;
    return false;
}

#define RAMPING_TIME_MS 50.0 // mS - 20 times per second

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

typedef enum _run_modes
{
    MODE_OFF, MODE_RUN, MODE_START
} RUN_MODES;

// adjust the speed including ramping, and return true if the ramping is complete.
static float adjust_speed (uint8_t user_setting, RUN_MODES mode)
{
    static float present_speed = 0.0;      // speed that motor is set to.
    const float SAFE_SPEED = 600.0;

    if (mode == MODE_OFF)
    {
        present_speed = 0.0;
        mc_interface_release_motor ();
        set_max_current (settings->limits[0]);
        return present_speed;
    }
    else if (mode == MODE_START)
    {
        set_max_current (settings->guard_high);
        present_speed = SAFE_SPEED;
        mc_interface_set_pid_speed (present_speed);
        return present_speed;
    }
    else
    {
        // mode == MODE_RUN
        present_speed = ramping (present_speed, settings->speeds[user_setting]);
        mc_interface_set_pid_speed (present_speed);
        set_max_current (settings->limits[user_setting]);
    }
    // @formatter:off
    #if(SPEED_LOG == 1)
        const char *const mode_str[] = { "OFF", "RUN", "START" };
    #endif
    SPED_LOG(("MODE=%.5s present=%4.2f, programmed=%4.2f",
            mode_str[(int) mode], (double) present_speed, (double) settings->speeds[user_setting]));
    // @formatter:on
return    present_speed;
}

static THD_FUNCTION(speed_thread, arg) // @suppress("No return")
{
    (void) arg;

    chRegSetThreadName ("SPEED");

    MOTOR_STATE state = MOTOR_OFF;

    // the message retrieved from the mailbox
    msg_t fetch = MSG_OK;

    // timeout value (used as a timeout service)
    systime_t timeout = TIME_INFINITE; // timeout in ticks. Use MS2ST(milliseconds) to set the value in milliseconds

    int32_t event = SPEED_OFF;

    float present_speed = 0.0;		// speed that motor is set to.

    // Delay this task starting so that settings are updated
    fetch = chMBFetch (&speed_mbox, (msg_t*) &event, MS2ST(500/*mSec*/));

    settings = get_sikorski_settings_ptr ();
    uint8_t user_speed = DEFAULT_SPEED; // the index to the speed setting. Always start out in the default speed

    for (;;)
    {

        fetch = chMBFetch (&speed_mbox, (msg_t*) &event, timeout);
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
                    timeout = MS2ST(RAMPING_TIME_MS); // start running this thread fast to achieve ramping.

                    // start ramping up to the first speed.
                    adjust_speed (user_speed, MODE_RUN);
                    send_to_display (DISP_SPEED_1 + user_speed);
                }
                break;
            case SPEED_KILL:
                state = MOTOR_KILLED;
                timeout = TIME_INFINITE;
                adjust_speed (user_speed, MODE_OFF);
                break;
            case TIMER_EXPIRY:
                timeout = migrate (&user_speed) ? TIME_INFINITE : MS2ST(settings->migrate_rate);
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
                timeout = MS2ST(settings->migrate_rate);
                adjust_speed (user_speed, MODE_OFF);
                break;
            case SPEED_UP:
                increase (&user_speed);
                adjust_speed (user_speed, MODE_RUN);
                send_to_display (DISP_SPEED_1 + user_speed);
                timeout = MS2ST(RAMPING_TIME_MS);
                break;
            case SPEED_DOWN:
                decrease (&user_speed);
                adjust_speed (user_speed, MODE_RUN);
                send_to_display (DISP_SPEED_1 + user_speed);
                timeout = MS2ST(RAMPING_TIME_MS);
                break;
            case SPEED_KILL:
                state = MOTOR_KILLED;
                timeout = TIME_INFINITE;
                adjust_speed (user_speed, MODE_OFF);
                break;
            case TIMER_EXPIRY: // runs often while ramping
                present_speed = adjust_speed (user_speed, MODE_RUN);    // ramping is taken care of in this function
                if (present_speed == settings->speeds[user_speed])
                    timeout = TIME_INFINITE;
                else
                    timeout = MS2ST(RAMPING_TIME_MS);
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
                timeout = MS2ST(settings->migrate_rate);
                adjust_speed (user_speed, MODE_OFF);
                send_to_ready (READY_OFF);		// don't need to check for start any more
                break;

            case SPEED_READY:   // READY thread approved start conditions
                state = MOTOR_ON;
                adjust_speed (user_speed, MODE_RUN);
                send_to_display (DISP_ON_TRIGGER);
                send_to_display (DISP_SPEED_1 + user_speed);
                timeout = MS2ST(RAMPING_TIME_MS); // start running this thread fast to achieve ramping.
                break;
            case SPEED_KILL:
                state = MOTOR_KILLED;
                timeout = TIME_INFINITE;
                adjust_speed (user_speed, MODE_OFF);
                break;
            default:
                break;
            }
            break;
        case MOTOR_KILLED:
            break;
        default:
            break;
        }

        if (old_state != state)
            SPED_LOG(("NEW State = %s", motor_states[state]));
    }
}

/*------------ LPF Object -----------------*/
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
    uint8_t running_safe_ct = 0; // count up when safety band is acheived. If it gets above threshold, indicate to
    // speed thread that it is safe to run full speed

    // timeout value (used as a timeout service)
    systime_t timeout = TIME_INFINITE; // timeout in ticks. Use MS2ST(milliseconds) to set the value in milliseconds

    // read the motor current, and see if the blades are unobstructed and the scooter is in water
    float motor_amps = 0.0;
    float filtered;
    LPF_CONTEXT lpfy;
    lpf_init (&lpfy, settings->f_alpha, 0.0);

    // Delay this task starting so that settings are updated
    fetch = chMBFetch (&ready_mbox, (msg_t*) &event, MS2ST(500/*mSec*/));

    for (;;)
    {
        fetch = chMBFetch (&ready_mbox, (msg_t*) &event, timeout);
        if (fetch == MSG_TIMEOUT)
            event = TIMER_EXPIRY;

        switch (event)
        {
        case READY_ON:
            adjust_speed (0, MODE_START);
            timeout = MS2ST(RAMPING_TIME_MS);	// 50 mS = 20Hz
            running_safe_ct = 0;
            lpf_init (&lpfy, settings->f_alpha, 1.0);
            break;

        case READY_OFF:
            timeout = TIME_INFINITE;	// turn task 'OFF', until turned on again.
            running_safe_ct = 0;
            break;

        case TIMER_EXPIRY:
            motor_amps = mc_interface_get_tot_current_filtered ();
            filtered = lpf_sample (&lpfy, motor_amps);

            if (filtered > settings->guard_high || filtered < settings->guard_low)
                running_safe_ct = 0;
            else
            {
                running_safe_ct += 1;
                if (running_safe_ct > settings->safe_count)
                {
                    send_to_speed (SPEED_READY);
                    timeout = TIME_INFINITE;
                }
            }
            SAFE_LOG(("SAFETY: Amps: %f, %f (%i)", (double) motor_amps, (double) filtered, running_safe_ct));
            break;
        default:
            break;
        }
    }
}

