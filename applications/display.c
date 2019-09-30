/*
 Copyright 2019 Claroworks

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

// this supports HT16K33 LED display module
#include "ch.h" // ChibiOS
#include "hal.h" // ChibiOS HAL
#include "mc_interface.h" // Motor control functions
#include "hw.h" // Pin mapping on this hardware
#include "timeout.h" // To reset the timeout

#include "Adafruit_LEDBackpack.h"
#include "Adafruit_GFX.h"

#include "commands.h"
#include "terminal.h"
#include "settings.h"
#include "display.h"

#define DISP_LOG(a) if(settings->logging & DISPLAY_LOG) commands_printf a

// Display thread
static THD_FUNCTION(display_thread, arg);
static THD_WORKING_AREA(display_thread_wa, 1024); // small stack

#define QUEUE_SZ 4
static msg_t msg_queue[QUEUE_SZ];
mailbox_t display_mbox;

static sikorski_data *settings;

// Start the display thread
void display_init ()
{
    // Start the display thread
    chThdCreateStatic (display_thread_wa, sizeof(display_thread_wa), NORMALPRIO, display_thread, NULL);

    // init the incoming message queue
    chMBObjectInit (&display_mbox, msg_queue, QUEUE_SZ);

}

void send_to_display (MESSAGE event)
{
    (void) chMBPost (&display_mbox, (msg_t) event, TIME_IMMEDIATE);
}

void GFX_drawBlk (int16_t x, int16_t y, int16_t w, int16_t h)
{	// set all pixels in the described block x,y,w,h
    for (int16_t j = 0; j < h; j++, y++)
    {
        for (int16_t i = 0; i < w; i++)
        {
            LED_drawPixel (x + i, y, LED_ON);
        }
    }
}

void show_bargraph4 (uint8_t bars /* 0 - 3 */)
{	// Display a bar graph, up to 4 bars.
    GFX_setRotation (1);
    LED_clear ();	// clear display

    /* always */GFX_drawBlk (0, 6, 2, 2);
    if (bars > 0)
        GFX_drawBlk (2, 4, 2, 4);
    if (bars > 1)
        GFX_drawBlk (4, 2, 2, 6);
    if (bars > 2)
        GFX_drawBlk (6, 0, 2, 8);

    LED_writeDisplay ();
}

void display_battery_graph (float volts)
{	// Choose the bar graph symbol to show based on battery voltage
// if the battery is low, flash the display.

    static bool display_flashing = false;

    if (display_flashing)
    {
        show_bargraph4 (0);
        LED_blinkRate (HT16K33_BLINK_1HZ);
        return;
    }
    for (size_t i = 0; i < sizeof(settings->battlevels); i++)
    {
        if (volts > settings->battlevels[i])
            continue;
        if (i == 0)
            display_flashing = true;
        show_bargraph4 (i);
        LED_blinkRate (HT16K33_BLINK_OFF);
        return;
    }
}

void display_speed (MESSAGE speed)
{
    int new_speed = speed - DISP_SPEED_1 + 1;
    GFX_setRotation (1);
    GFX_setTextSize (1);
    GFX_setTextColor (LED_ON);
    LED_clear ();
    GFX_setCursor (1, 0);
    char text[2] =
        { '0' + new_speed, '\0' };
    GFX_print_str (text);
    LED_writeDisplay ();
    DISP_LOG(("Write '%s'", text));
}

#define DISP_RATE 2
typedef enum _disp_state
{
    DISP_OFF = 0,	// Display is idle
    DISP_ON,		// display is showing battery
    DISP_WAIT,		// display is showing 'wait for it'
    DISP_SPEED,     // displaying the speed number
    DISP_PWR_ON
} DISP_STATE;

const char *const disp_states[] =
    { "DISP_OFF", "DISP_ON", "DISP_WAIT", "DISP_SPEED", "DISP_PWR_ON" };

static THD_FUNCTION(display_thread, arg)
{ // @suppress("No return")
    (void) arg;

    chRegSetThreadName ("I2C_DISPLAY");

    LED_begin ();
    LED_setBrightness (settings->brightness);
    LED_clear ();	// clear display
    LED_blinkRate (HT16K33_BLINK_OFF);
    LED_writeDisplay ();

    // the message retrieved from the mailbox
    msg_t fetch = MSG_OK;
    int32_t event = TIMER_EXPIRY;

    // Delay this task starting so that settings are updated
    fetch = chMBFetch (&display_mbox, (msg_t*) &event, MS2ST(500/*mSec*/));
    settings = get_sikorski_settings_ptr ();

    // clear display & then display voltage meter
    LED_clear ();
    float volts = GET_INPUT_VOLTAGE();
    display_battery_graph (volts);
    LED_writeDisplay ();

    // used during DISP_WAIT to track the dot position
    uint8_t dot_pos = 0;

    // timeout value (used as a timer service)
    // set it for our initial state -  power on
    systime_t timeout = MS2ST(settings->disp_on_ms); // timeout in ticks. Use MS2ST(milliseconds) to set the value in milliseconds

    DISP_STATE state = DISP_PWR_ON;

    for (;;)
    {
        fetch = chMBFetch (&display_mbox, (msg_t*) &event, timeout);
        if (fetch == MSG_TIMEOUT)
            event = TIMER_EXPIRY;

        DISP_LOG(("DISPLAY = %s, Event = %s", disp_states[state], message_text (event)));

        DISP_STATE old_state = state;

        switch (state)
        {
        case DISP_PWR_ON:
            switch (event)
            {
            case TIMER_EXPIRY:
                LED_clear ();	// clear display
                LED_writeDisplay ();
                timeout = TIME_INFINITE;
                state = DISP_OFF;
                break;
            case DISP_ON_TRIGGER:   // rcvd when the motor turns on
                state = DISP_SPEED;
                break;
            default:
                volts = GET_INPUT_VOLTAGE();
                display_battery_graph (volts);
                break;
            }
            break;
        case DISP_SPEED:				// enter this state when "on trigger" - motor is running
            if (event >= DISP_SPEED_1 && event <= DISP_SPEED_9) // don't handle above speed 9, rewrite as needed to support...
            {
                display_speed (event);
                timeout = MS2ST(settings->disp_on_ms);
                break;
            }
            switch (event)
            {
            case TIMER_EXPIRY:
                LED_clear ();   // clear display
                LED_writeDisplay ();
                timeout = TIME_INFINITE;
                state = DISP_OFF;
                break;
            case DISP_OFF_TRIGGER: 	// rcvd when the motor turns off - start the display cycle by showing the 'waiting' display
                LED_clear ();	// clear display
                LED_writeDisplay ();
                timeout = MS2ST(settings->disp_beg_ms / 24);	// 8 dots in waiting progression
                state = DISP_WAIT;
                dot_pos = 0;
                break;
            default:
                break;
            }
            break;

        case DISP_OFF:                // After displaying speed, go IDLE.
            if (event >= DISP_SPEED_1 && event <= DISP_SPEED_9) // don't handle above speed 9, rewrite as needed to support...
            {
                state = DISP_SPEED;
                display_speed (event);
                timeout = MS2ST(settings->disp_on_ms);
                break;
            }
            switch (event)
            {
            case DISP_OFF_TRIGGER: 	// rcvd when the motor turns off - start the display cycle by showing the 'waiting' display
                LED_clear ();	// clear display
                LED_writeDisplay ();
                timeout = MS2ST(settings->disp_beg_ms / 24);	// 8 dots in waiting progression
                state = DISP_WAIT;
                dot_pos = 0;
                break;
            case DISP_ON_TRIGGER:   // rcvd when the motor turns on
                state = DISP_SPEED;
                break;
            default:
                break;
            }
            break;
        case DISP_WAIT:				// show 8 dots across bottom of display, progressing as timer continues
            switch (event)
            {
            case DISP_ON_TRIGGER: 	// rcvd when the motor turns on
                LED_clear ();	// clear display
                GFX_setRotation (1);
                LED_writeDisplay ();
                timeout = TIME_INFINITE;
                state = DISP_SPEED;
                break;
            case TIMER_EXPIRY:
                if (dot_pos == 24)
                {	// setup and go to DISP_ON state
                    timeout = MS2ST(settings->disp_dur_ms);
                    state = DISP_ON;
                    volts = GET_INPUT_VOLTAGE();
                    display_battery_graph (volts);
                    break;
                }
                LED_clear ();
                LED_drawPixel (dot_pos & 0x07, 7, LED_ON);
                dot_pos++;
                LED_writeDisplay ();
                break;
            default:
                break;
            }
            break;
        case DISP_ON:
            switch (event)
            {
            case DISP_ON_TRIGGER: 	// rcvd when the motor turns on
                LED_clear ();	// clear display
                LED_writeDisplay ();
                timeout = TIME_INFINITE;
                state = DISP_SPEED;
                break;
            case TIMER_EXPIRY:
                LED_clear ();	// clear display
                LED_writeDisplay ();
                timeout = TIME_INFINITE;
                state = DISP_OFF;
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }

        if (old_state != state)
            DISP_LOG(("NEW State = %s", disp_states[state]));
    }
}

