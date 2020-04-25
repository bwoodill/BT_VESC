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

#include "commands.h"
#include "settings.h"
#include "display.h" // for displaying the battery status
#include "trigger.h" // thread handling the trigger logic
#include "speed.h" 	 // thread handling the motor speed logic
#include "batteries.h"

const char* message_text (MESSAGE msg_type)
{
    const char *const messages[] = MESSAGES_TEXT;
    return messages[(int) msg_type - MESSAGES_BASE];
}

#define DISP_LOG(a) if(settings->logging & DISPLAY_LOG) commands_printf a


static sikorski_data *settings;
void check_batteries (void);

// Switch thread
static THD_FUNCTION(switch_thread, arg);
static THD_WORKING_AREA(switch_thread_wa, 1024); // small stack
static mutex_t batt_mutex;

void app_sikorski_init (void)
{
    chMtxObjectInit(&batt_mutex);

    // Set the SERVO pin as an input with pullup (attached to the trigger switch)
    palSetPadMode(HW_ICU_GPIO, HW_ICU_PIN, PAL_MODE_INPUT_PULLUP);

    settings = get_sikorski_settings_ptr();

    display_start();

    int i = 0;
    while(settings->SettingsValidMagic != VALID_VALUE)
    {
        display_dots(i++);
        chThdSleepMilliseconds(500);   // sleep long enough for other applications to be online
        if(i >= 30)
        {
            settings->SettingsValidMagic = VALID_VALUE;
//             save_all_settings ();
        }
    }

    // Start the motor speed thread
    speed_init ();

    // Start the trigger thread
    trigger_init ();

    // Start the switch thread
    chThdCreateStatic (switch_thread_wa, sizeof(switch_thread_wa), NORMALPRIO, switch_thread, NULL);

    // Start the display thread
    display_init ();
}


static float batteries[2];

float get_lowest_battery_voltage(void)
{
    float batt_volts[2];

    chMtxLock(&batt_mutex);
    { // LOCKED CONTEXT
        batt_volts[0] = batteries[0];
        batt_volts[1] = batteries[1];
    }
    chMtxUnlock(&batt_mutex);

    // return the smallest value
    if (batt_volts[0] < batt_volts[1])
    {
        return batt_volts[0];
    }
    return batt_volts[1];
}

// return the voltage difference between the two batteries
float get_battery_imbalance(void)
{
    float batt_imbalance;

    chMtxLock(&batt_mutex);
    { // LOCKED CONTEXT
        batt_imbalance = batteries[1] - batteries[0];
    }
    chMtxUnlock(&batt_mutex);

    // return the smallest value
    return batt_imbalance;
}

void check_batteries (void)
{
    // (battery1 (top) + battery2) is read by normal VESC firmware.
    // battery 2 (bottom) voltage is read here directly

    static float batt_2,batt_total;
    static int count = 0;

    batt_2 += ((V_REG / 4095.0) * (float) ADC_Value[ADC_IND_EXT] * (settings->b2Rratio + 1));
    batt_total += GET_INPUT_VOLTAGE();

    // calculate an average after so many counts (count == 0)
    count = (count + 1) % BATTERY_CHECK_COUNTS;
    if (count)
        return;

    // count rolled over. Now average the results
    chMtxLock(&batt_mutex); // protect multiple access
    { // LOCKED CONTEXT
        batteries[1] = batt_2 / (float) BATTERY_CHECK_COUNTS;
        batteries[0] = (batt_total - batt_2) / (float) BATTERY_CHECK_COUNTS;

        batt_2 = batt_total = 0.0; // reset the totals

        DISP_LOG(("TOTAL = %2.2f  BATT1 = %2.2f  BATT2 = %2.2f",
            (double) GET_INPUT_VOLTAGE(), (double) batteries[0], (double) batteries[1] ));
    }
    chMtxUnlock(&batt_mutex);
}

static THD_FUNCTION(switch_thread, arg) // @suppress("No return")
{
    (void) arg;

    chRegSetThreadName ("SWITCH");
    static uint8_t sw = 1;
    bool trigger_pressed = false;

    chThdSleepMilliseconds(500);   // sleep long enough for settings to be set by init functions
    settings = get_sikorski_settings_ptr();

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

        check_batteries();

        // Run this loop at 40Hz - timing isn't critical
        chThdSleepMilliseconds(1000 / 40.0);

        // Reset the timeout
        timeout_reset ();
    }
}
