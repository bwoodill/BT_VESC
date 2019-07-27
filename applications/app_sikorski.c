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

#include "ch.h" // ChibiOS
#include "hal.h" // ChibiOS HAL
#include "mc_interface.h" // Motor control functions
#include "hw.h" // Pin mapping on this hardware
#include "timeout.h" // To reset the timeout
#include <stddef.h>

#include "display.h" // for displaying the battery status
#include "trigger.h" // thread handling the trigger logic
#include "speed.h" 	 // thread handling the motor speed logic

const char* message_text (MESSAGE msg_type)
{
    const char *const messages[] = MESSAGES_TEXT;
    return messages[(int) msg_type - MESSAGES_BASE];
}

// Switch thread
static THD_FUNCTION(switch_thread, arg);
static THD_WORKING_AREA(switch_thread_wa, 1024); // small stack

void app_sikorski_init (void)
{
    // Set the SERVO pin as an input with pullup
    palSetPadMode(HW_ICU_GPIO, HW_ICU_PIN, PAL_MODE_INPUT_PULLUP);

    // Start the motor speed thread
    speed_init ();

    // Start the trigger thread
    trigger_init ();

    // Start the switch thread
    chThdCreateStatic (switch_thread_wa, sizeof(switch_thread_wa), NORMALPRIO, switch_thread, NULL);

    // Start the display thread
    display_init ();
}

static THD_FUNCTION(switch_thread, arg) // @suppress("No return")
{
    (void) arg;

    chRegSetThreadName ("SWITCH");
    static uint8_t sw = 0;
    bool trigger_pressed = false;

    for (;;)
    {
    	trigger_pressed = palReadPad(HW_ICU_GPIO, HW_ICU_PIN);
        if (trigger_pressed)
        {
            if (sw == 0) // debounce
                send_to_trigger (SW_PRESSED);
            sw = 1;
        }
        else
        {
            if (sw == 1) // debounce
                send_to_trigger (SW_RELEASED);
            sw = 0;
        }

        // Run this loop at 40Hz
        chThdSleepMilliseconds(1000 / 40.0);

        // Reset the timeout
        timeout_reset ();
    }
}
