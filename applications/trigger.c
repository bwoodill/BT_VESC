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
	
	Additional Copyright 2021 Benjamin Woodill bwoodill@gmail.com
*/

// This file defines a thread that takes switch events and performs timing and
//		logic to determine user trigger actions.
// The actions are then sent to the speed control thread.
//
#include <stddef.h>

#include "ch.h" // ChibiOS
#include "hal.h" // ChibiOS HAL
#include "mc_interface.h" // Motor control functions

#include "commands.h"
#include "terminal.h"
#include "settings.h"
#include "app_version.h"
#include "speed.h" 	// thread handling the motor speed logic
#include "trigger.h" // thread handling the trigger logic

#define TRIG_LOG(a) if(settings->logging & TRIGGER_LOG) commands_printf a

#define QUEUE_SZ 4
static msg_t msg_queue[QUEUE_SZ];
mailbox_t trigger_mbox;

static sikorski_data * settings;

typedef enum _sw_state
{
    SWST_OFF = 0,		// idle state, off trigger for long time
    SWST_GOING_ON,      // initial trigger on, part of double click to turn on
    SWST_ON,			// on trigger for a long time, motor not running
    SWST_ONE_OFF,		// part of double click just before turning motor on
    SWST_ONE_ON,		// double click to start was successful (turn motor on)
    SWST_GOING_OFF,		// part of first click to adjust motor speed
    SWST_CLICKED,		// second part of a single click
    SWST_CLCKD_OFF,		// third part of a double click
    SWST_CLCKD_THREE,	// triple click max speed
	SWST_CRUISE,		// cruise control
	SWST_CLCKD_FOUR,	// four click
	SWST_CLCKD_FIVE,	// Five Click Reverse
	SWST_CLCKD_THREE_START, // start full speed
	SWST_ONE_START,		//clicked twice before, process release
	SWST_CLCKD_FOUR_START, // clicked three before, process four click start
	SWST_CLCKD_REV_START, // clicked four before, process five click reverse start
	SWST_CLCKD_TWO_OFF,	//released after two clicks, execute second click
	SWST_CLCKD_THREE_OFF, // released after three clicks, execute three click
	SWST_CLCKD_FOUR_OFF, //released after four clicks, execute four click
	SWST_CLCKD_FOUR_OFF_START, //released after four clicks, execute four click start
	SWST_CLCKD_FIVE_OFF, //clicked four before, process five click start
    SWST_EOL
} SW_STATE;

const char *const sw_states[] =
    { "SWST_OFF", "SWST_GOING_ON", "SWST_ON", "SWST_ONE_OFF", "SWST_ONE_ON", "SWST_GOING_OFF", "SWST_CLICKED", "SWST_CLCKD_OFF", "SWST_CLCKD_THREE", "SWST_CRUISE", "SWST_CLCKD_FOUR", "SWST_CLCKD_FIVE", "SWST_CLCKD_THREE_START", "SWST_CLCKD_FOUR_START", "SWST_CLCKD_REV_START", "SWST_CLCKD_TWO_OFF", "SWST_CLCKD_THREE_OFF", "SWST_CLCKD_FOUR_OFF", "SWST_CLCKD_FOUR_OFF_START", "SWST_CLCKD_FIVE_OFF", "SWST_ONE_START" };

static THD_FUNCTION(trigger_thread, arg);
static THD_WORKING_AREA(trigger_thread_wa, 2048);

void trigger_init (void)
{
    /* Start trigger thread. */
    chThdCreateStatic (trigger_thread_wa, sizeof(trigger_thread_wa), NORMALPRIO, trigger_thread, NULL);

    // init the trigger (incoming) message queue
    chMBObjectInit (&trigger_mbox, msg_queue, QUEUE_SZ);
}

void send_to_trigger (MESSAGE event)
{
    (void) chMBPost (&trigger_mbox, (msg_t) event, TIME_IMMEDIATE);
}

static THD_FUNCTION(trigger_thread, arg) // @suppress("No return")
{
    (void) arg;

    chRegSetThreadName ("TRIGGER");

    SW_STATE state = SWST_OFF;

    // the message retrieved from the mailbox
    msg_t fetch = MSG_OK;
    int32_t event;

    settings = get_sikorski_settings_ptr();
    while(settings->magic != VALID_VALUE)
    {
        chThdSleepMilliseconds(50);   // sleep long enough for other applications to be online
    }

    // timeout value (used as a timeout service)
    systime_t timeout = TIME_INFINITE; // timeout in ticks. Use MS2ST(milliseconds) to set the value in milliseconds

    for (;;)
    {
        fetch = chMBFetch (&trigger_mbox, (msg_t*) &event, timeout);

        if (fetch == MSG_TIMEOUT)
            event = TIMER_EXPIRY;

        TRIG_LOG(("TRIGGER State = %s, Event = 0x%x", sw_states[state], event));
        SW_STATE old_state = state;

        switch (state)
        {
        case SWST_OFF:
            if (event == SW_PRESSED)
            {
                state = SWST_GOING_ON;
                timeout = MS2ST(settings->trig_on_time);
            }
            break;
        case SWST_ON:
            if (event == SW_RELEASED)
            {
                state = SWST_OFF;
                timeout = TIME_INFINITE;
            }
            break;
        case SWST_GOING_ON:	//initial release
            if (event == SW_RELEASED)
            {
                state = SWST_ONE_OFF;
                timeout = MS2ST(settings->trig_off_time);
            }
            if (event == TIMER_EXPIRY)
            {
                // report the application version
                commands_printf(APP_VERSION);
                state = SWST_ON;
                timeout = TIME_INFINITE;
            }
            break;
        case SWST_ONE_OFF: //clicked once before, process second click
            if (event == SW_PRESSED)
            {
				if ((settings->reverse) == 0 && (settings->jump) == 0)
				{
					state = SWST_ONE_ON;
					timeout = TIME_INFINITE;
					send_to_speed (SPEED_ON);
				}
				else
				{
					state = SWST_ONE_START;
					timeout = MS2ST(settings->trig_off_time);
				}
			}
            if (event == TIMER_EXPIRY)
            {
                state = SWST_OFF;
                timeout = TIME_INFINITE;
            }
            break;
        case SWST_ONE_START: // clicked twice before, process release
            if (event == SW_RELEASED)
            {
				state = SWST_CLCKD_THREE_START;
				timeout = MS2ST(settings->trig_off_time);
            }
            if (event == TIMER_EXPIRY) //only clicked twice so turn on
            {
                state = SWST_ONE_ON;
				send_to_speed (SPEED_ON);
                timeout = TIME_INFINITE;
            }
            break;
		case SWST_CLCKD_THREE_START: // clicked twice before, process three click jump start
            if (event == SW_PRESSED)
            {
                if ((settings->reverse) == 0)
				{
					state = SWST_ONE_ON;
					send_to_speed (JUMP_SPEED_START);
					timeout = TIME_INFINITE;
				}
				else
				{
					state = SWST_CLCKD_FOUR_OFF_START;
					timeout = MS2ST(settings->trig_off_time);
				}
            }
			if (event == TIMER_EXPIRY)
            {
                state = SWST_OFF;
                timeout = TIME_INFINITE;
                send_to_speed (SPEED_OFF);
            }
            break;
		case SWST_CLCKD_FOUR_OFF_START: // clicked three before, process four click start
            if (event == SW_RELEASED)
            {
				state = SWST_CLCKD_FOUR_START;
				timeout = MS2ST(settings->trig_off_time);
            }
			if (event == TIMER_EXPIRY)
            {
				if ((settings->jump) == 1)
				{
					state = SWST_ONE_ON;
					send_to_speed (JUMP_SPEED_START);
					timeout = TIME_INFINITE;
				}
				else
				{
					state = SWST_ONE_ON;
					timeout = TIME_INFINITE;
					send_to_speed (SPEED_DOWN);
				}
            }
            break;
		case SWST_CLCKD_FOUR_START: // clicked three before, execute four click start
            if (event == SW_PRESSED)
            {
				state = SWST_CLCKD_FIVE_OFF;
				timeout = MS2ST(settings->trig_off_time);
            }
			if (event == TIMER_EXPIRY)
            {
                state = SWST_OFF;
                timeout = TIME_INFINITE;
                send_to_speed (SPEED_OFF);
            }
            break;
		case SWST_CLCKD_FIVE_OFF: // clicked four before, process five click start
            if (event == SW_RELEASED)
            {
				state = SWST_CLCKD_REV_START;
				timeout = MS2ST(settings->trig_off_time);
            }
			if (event == TIMER_EXPIRY)
            {
				state = SWST_ONE_ON;
                timeout = TIME_INFINITE;   
            }
            break;	
		case SWST_CLCKD_REV_START: // clicked four before, process five click reverse start
            if (event == SW_PRESSED)
            {
                state = SWST_OFF;
				timeout = TIME_INFINITE;
				send_to_speed (REVERSE_SPEED_START);
            }
			if (event == TIMER_EXPIRY)
            {
                state = SWST_OFF;
                timeout = TIME_INFINITE;
                send_to_speed (SPEED_OFF);
            }
            break;	
        case SWST_ONE_ON: // motor will be running in this state
            if (event == SW_RELEASED)
            {
                state = SWST_GOING_OFF;
                timeout = MS2ST(settings->trig_on_time);
            }
            break;
        case SWST_GOING_OFF: //released after running, process first click
            if (event == SW_PRESSED)
            {
                state = SWST_CLICKED;
                timeout = MS2ST(settings->trig_off_time);
            }
            if (event == TIMER_EXPIRY)
            {
                state = SWST_OFF;
                timeout = TIME_INFINITE;
                send_to_speed (SPEED_OFF);
            }
            break;
        case SWST_CLICKED:	//released after one click, execute first click
            if (event == SW_RELEASED)
            {
                state = SWST_CLCKD_OFF;
                timeout = MS2ST(settings->trig_on_time);
            }
            if (event == TIMER_EXPIRY)
            {
                state = SWST_ONE_ON;
                timeout = TIME_INFINITE;
                send_to_speed (SPEED_DOWN);
            }
            break;
        case SWST_CLCKD_OFF:	//clicked one before, process second click
            if (event == SW_PRESSED)
            {
                state = SWST_CLCKD_TWO_OFF;
                timeout = MS2ST(settings->trig_on_time);
            }
            if (event == TIMER_EXPIRY)
            {
                state = SWST_OFF;
                timeout = TIME_INFINITE;
                send_to_speed (SPEED_OFF);
            }
            break;
		case SWST_CLCKD_TWO_OFF:	//released after two clicks, execute second click
            if (event == SW_RELEASED)
            {
                state = SWST_CLCKD_THREE;
                timeout = MS2ST(settings->trig_on_time);
            }
            if (event == TIMER_EXPIRY)
            {
                state = SWST_ONE_ON;
                timeout = TIME_INFINITE;
				send_to_speed (SPEED_UP);
            }
            break;
        case SWST_CLCKD_THREE: // clicked twice before, process three click max speed
            if (event == SW_PRESSED)
            {
				if ((settings->reverse) == 1 || (settings->jump) == 1 || (settings->cruise) == 1)
				{
					state = SWST_CLCKD_THREE_OFF;
					timeout = MS2ST(settings->trig_on_time);
				}
				else
				{
					state = SWST_CLICKED;
					timeout = MS2ST(settings->trig_on_time);
				}
            }
            if (event == TIMER_EXPIRY)
            {
                state = SWST_OFF;
                timeout = TIME_INFINITE;
                send_to_speed (SPEED_OFF);
            }
            break;
		case SWST_CLCKD_THREE_OFF: // released after three clicks, execute three click
            if (event == SW_RELEASED)
            {
                state = SWST_CLCKD_FOUR;
                timeout = MS2ST(settings->trig_on_time);
            }
            if (event == TIMER_EXPIRY)
            {
                if ((settings->jump) == 1)
				{
					state = SWST_ONE_ON;
					send_to_speed (JUMP_SPEED);
					timeout = TIME_INFINITE;
				}
				else
				{
					state = SWST_CLICKED;
					timeout = MS2ST(settings->trig_on_time);
				}
            }
            break;
        case SWST_CLCKD_FOUR: //clicked three before, four click process
            if (event == SW_PRESSED)
            {
				if ((settings->cruise) == 1 && (settings->reverse) == 0)
				{
					state = SWST_CRUISE;
					timeout = TIME_INFINITE;
				}
				else
				{
					state = SWST_CLCKD_FOUR_OFF;
					timeout = MS2ST(settings->trig_on_time);
				}
            }
            if (event == TIMER_EXPIRY)
            {
                state = SWST_OFF;
                timeout = TIME_INFINITE;
                send_to_speed (SPEED_OFF);
            }
            break;
		case SWST_CLCKD_FOUR_OFF: //released after four clicks, execute four click
            if (event == SW_RELEASED)
            {
				if ((settings->reverse) == 1)
				{
					state = SWST_CLCKD_FIVE;
					timeout = MS2ST(settings->trig_on_time);
				}
				else
				{
					state = SWST_CLICKED;
					timeout = MS2ST(settings->trig_off_time);
				}
            }
            if (event == TIMER_EXPIRY)
            {
				if ((settings->cruise) == 1)
				{
					state = SWST_CRUISE;
					timeout = TIME_INFINITE;
				}
				else
				{
					state = SWST_ONE_ON;
					timeout = TIME_INFINITE;
					send_to_speed (SPEED_UP);
				}
            }
            break;
        case SWST_CRUISE: //cruise
            if (event == SW_PRESSED)
            {
				state = SWST_ONE_ON;
				timeout = TIME_INFINITE;				
            }
			break;
		case SWST_CLCKD_FIVE: //clicked 4 before, process five click reverse
            if (event == SW_PRESSED)
            {
				state = SWST_OFF;
				timeout = TIME_INFINITE;
				send_to_speed (REVERSE_SPEED);
            }
			if (event == TIMER_EXPIRY)
            {
                if ((settings->cruise) == 1)
				{
					state = SWST_CRUISE;
					timeout = TIME_INFINITE;
				}
				else
				{
					state = SWST_OFF;
					timeout = TIME_INFINITE;
					send_to_speed (SPEED_OFF);
				}
            }
            break;
        default:
            break;
        }

        if (old_state != state)
            TRIG_LOG(("NEW State = %s", sw_states[state]));
    }
}
