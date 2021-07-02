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
#include "batteries.h"

#define DISP_LOG(a) if(settings->logging & DISPLAY_LOG) commands_printf a

// Display thread
static THD_FUNCTION(display_thread, arg);
static THD_WORKING_AREA(display_thread_wa, 2048); // medium stack

#define QUEUE_SZ 4
static msg_t msg_queue[QUEUE_SZ];
mailbox_t display_mbox;

static sikorski_data *settings;

#define HOLD_DISPLAY_TIME_mS MS2ST(1500)
static MESSAGE last_speed = 3;

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

void display_battery_graph (bool initial)
{
    float pack_level;
    if (initial)
    {
        pack_level = GET_INPUT_VOLTAGE();
    }
    else
    {
        // Choose the bar graph symbol to show based on battery voltage
        // base on lowest battery, but settings are for 2 batteries
        pack_level = get_lowest_battery_voltage() * 2.0;
    }

    // Display a bar graph, up to 4 bars.

    GFX_setRotation (settings->disp_rotation);
    LED_clear ();   // clear display

    // hard-coded checks was more straight forward and readable than a loop.

    /* always turn on bar 1 */
    GFX_drawBlk (0, 6, 2, 2);

    if (pack_level > settings->battlevels[0])
    {
        GFX_drawBlk (2, 4, 2, 4);
    }
    if (pack_level > settings->battlevels[1])
    {
        GFX_drawBlk (4, 2, 2, 6);
    }
    if (pack_level > settings->battlevels[2])
    {
        GFX_drawBlk (6, 0, 2, 8);
    }

    LED_blinkRate (0);

    // When there is an imbalance in the battery charge between two batteries,
    // indicate to the user which one is low (defective).

    /*  0 1 2 3 4 5 6 7  X        0 1 2 3 4 5 6 7  X
     0  - X - - - - : :        0  X X - - - - - -
     1  X X - - - - : :        1  - X - - - - - -
     2  - X - - : : : :        2  X - - - - - - -
     3  - X - - : : : :        3  X X - - - - - -
     4  - - : : : : : :        4  - - - - - - - -
     5  - - : : : : : :        5  - - - - - - - -
     6  : : : : : : : :        6  - - - - - - - -
     7  : : : : : : : :        7  - - - - - - - -
     Y        1                Y         2
    */

    float imbalance = get_battery_imbalance();

    if (imbalance > settings->batt_imbalance) // display a small '1'
    {
        GFX_drawBlk  (1, 0, 1, 4);
        LED_drawPixel(0, 1, LED_ON);
        DISP_LOG(("Displaying '1'"));
    }

    if (imbalance < ( - settings->batt_imbalance)) // display a small '2'
    {
        GFX_drawBlk  (0, 0, 2, 4);
        LED_drawPixel(0, 1, LED_OFF);
        LED_drawPixel(1, 2, LED_OFF);
        DISP_LOG(("Displaying '2'"));
    }

    LED_writeDisplay ();
}

void display_speed (MESSAGE speed)
{
    int new_speed = speed - DISP_SPEED_1 + 1;
    if(new_speed > 9 || new_speed < 1)
        return;
    GFX_setRotation (settings->disp_rotation);
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

void display_reverse (MESSAGE rev)
{
	char rev = "R";
    GFX_setRotation (settings->disp_rotation);
    GFX_setTextSize (1);
    GFX_setTextColor (LED_ON);
    LED_clear ();
    GFX_setCursor (1, 0);
    char text[2] =
        { '0' + rev, '\0' };
    GFX_print_str (text);
    LED_writeDisplay ();
}

#define DISP_RATE 2
typedef enum _disp_state
{
    DISP_OFF = 0,	// Display is idle
    DISP_BATT,		// display is showing battery
    DISP_TRIG,      // display is "on trigger" - waiting to display speed
    DISP_WAIT,		// display is showing 'wait for it'
    DISP_SPEED,     // displaying the speed number
    DISP_PWR_ON     // Power on display
} DISP_STATE;

const char *const disp_states[] =
    { "DISP_OFF", "DISP_BATT", "DISP_TRIG", "DISP_WAIT", "DISP_SPEED", "DISP_PWR_ON" };

void display_idle(void)
{
    LED_clear ();   // clear display
    LED_writeDisplay ();
}

void display_start(void)
{
    LED_begin();
}

void display_dots(uint16_t pos)
{
    LED_clear ();
    LED_drawPixel (pos & 0x07, 7, LED_ON);
    pos++;
    LED_writeDisplay ();
}

static THD_FUNCTION(display_thread, arg) // @suppress("No return")
{
    (void) arg;

    chRegSetThreadName ("I2C_DISPLAY");

    display_start();

    settings = get_sikorski_settings_ptr ();

    int i = 0;
    while(settings->magic != VALID_VALUE)
    {
        display_dots(i++);
        chThdSleepMilliseconds(50);   // sleep long enough for other applications to be online
    }

    // the message retrieved from the mailbox
    msg_t fetch = MSG_OK;
    int32_t event = TIMER_EXPIRY;

    // clear display & then display voltage meter
    display_battery_graph(true);
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
                display_idle();
                timeout = TIME_INFINITE;
                state = DISP_OFF;
                break;
            case DISP_ON_TRIGGER:   // rcvd when the motor turns on
                timeout = HOLD_DISPLAY_TIME_mS;
                state = DISP_TRIG;
                last_speed = 0;
                break;
            default:
                display_battery_graph(false);
                break;
            }
            break;
        case DISP_TRIG:
            if (event >= DISP_SPEED_1 && event <= DISP_SPEED_9) // don't handle above speed 9, rewrite as needed to support...
                last_speed = event;

            switch (event)
            {
            case TIMER_EXPIRY:
                display_speed (last_speed);
                timeout = MS2ST(settings->disp_on_ms);
                state = DISP_SPEED;
                break;
            default:
                break;
            }
            break;
        case DISP_SPEED:				// enter this state when "on trigger" - motor is running
            if (event >= DISP_SPEED_1 && event <= DISP_SPEED_9) // don't handle above speed 9, rewrite as needed to support...
            {
                last_speed = event;
                display_speed (last_speed);
                timeout = MS2ST(settings->disp_on_ms);
                break;
            }
            switch (event)
            {
            case TIMER_EXPIRY:
                display_idle();
                timeout = TIME_INFINITE;
                break;
            case DISP_OFF_TRIGGER: 	// rcvd when the motor turns off - start the display cycle by showing the 'waiting' display
                display_idle();
                timeout = MS2ST(settings->disp_beg_ms / 24);	// 8 dots in waiting progression
                state = DISP_WAIT;
                dot_pos = 0;
                break;
            default:
                break;
            }
            break;

        case DISP_OFF:                // After displaying speed, go IDLE.
            switch (event)
            {
            case DISP_ON_TRIGGER:   // rcvd when the motor turns on
                last_speed = 0;
                timeout = HOLD_DISPLAY_TIME_mS;
                state = DISP_TRIG;
                break;
            default:
                break;
            }
            break;
        case DISP_WAIT:				// show 8 dots across bottom of display, progressing as timer continues
            switch (event)
            {
            case DISP_ON_TRIGGER: 	// rcvd when the motor turns on
                last_speed = 0;
                timeout = HOLD_DISPLAY_TIME_mS;
                state = DISP_TRIG;
                break;
            case TIMER_EXPIRY:
                if (dot_pos == 24)
                {	// setup and go to DISP_BATT state
                    timeout = MS2ST(settings->disp_dur_ms);
                    state = DISP_BATT;
                    display_battery_graph(false);
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
        case DISP_BATT:
            switch (event)
            {
            case DISP_ON_TRIGGER: 	// rcvd when the motor turns on
                last_speed = 0;
                timeout = HOLD_DISPLAY_TIME_mS;
                state = DISP_TRIG;
                break;
            case TIMER_EXPIRY:
                display_idle();
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

