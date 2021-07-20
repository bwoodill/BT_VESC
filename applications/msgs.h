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

#ifndef APPLICATIONS_MSGS_H_
#define APPLICATIONS_MSGS_H_

#define MESSAGES_BASE 0x100

typedef enum _msgs
{
    NO_MSG = 0,
    TIMER_EXPIRY = MESSAGES_BASE,	// used when message fetch times out. Used as a general purpose timer event.

    // Messages sent to the trigger thread
    SW_RELEASED,
    SW_PRESSED,

    // sent to the motor speed controller thread
    SPEED_OFF,
    SPEED_ON,
    SPEED_UP,
    SPEED_DOWN,
    JUMP_SPEED_START,
    REVERSE_SPEED_START,
    JUMP_SPEED,
    REVERSE_SPEED,
    SPEED_READY,	// from the READY thread - notifies that scooter is ready to run
    CHECK_BATTERY,  // tell SPEED thread to check battery voltages.

    // commands the ready task to run or cease
    READY_OFF,
    READY_ON,

    // messages for display so that it will know the current state
    DISP_ON_TRIGGER,
    DISP_OFF_TRIGGER,
    DISP_SPEED_1,       // up to 15 speeds represented
    DISP_SPEED_2,       //  for displaying on a speed change
    DISP_SPEED_3,
    DISP_SPEED_4,
    DISP_SPEED_5,
    DISP_SPEED_6,
    DISP_SPEED_7,
    DISP_SPEED_8,
    DISP_SPEED_9,
    DISP_SPEED_A,
    DISP_SPEED_B,
    DISP_SPEED_C,
    DISP_SPEED_D,
    DISP_SPEED_E,
    DISP_SPEED_F,

    // battery condition messages
    BATT_1_TOOLOW,
    BATT_2_TOOLOW,

    MESSAGE_EOL,
} MESSAGE;

extern const char* message_text (MESSAGE msg_type);
#define MESSAGES_TEXT {\
	"TIMER_EXPIRY", \
	"SW_RELEASED", "SW_PRESSED", \
	"SPEED_OFF", "SPEED_ON", "SPEED_UP", "JUMP_SPEED", "JUMP_SPEED_START", "REVERSE_SPEED", "REVERSE_SPEED_START", "SPEED_DOWN", "SPEED_READY", \
	"CHECK_BATTERY","READY_OFF", "READY_ON", \
	"DISP_ON_TRIGGER", "DISP_OFF_TRIGGER", \
    "DISP_SPEED_1", "DISP_SPEED_2", "DISP_SPEED_3", "DISP_SPEED_4", "DISP_SPEED_5", \
    "DISP_SPEED_6", "DISP_SPEED_7", "DISP_SPEED_8", "DISP_SPEED_9", "DISP_SPEED_A", \
    "DISP_SPEED_B", "DISP_SPEED_C", "DISP_SPEED_D", "DISP_SPEED_E", "DISP_SPEED_F", \
    "BATT_1_TOOLOW", "BATT_2_TOOLOW", \
}

#endif /* APPLICATIONS_MSGS_H_ */
