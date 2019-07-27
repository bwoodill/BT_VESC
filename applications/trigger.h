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

#ifndef APPLICATIONS_TRIGGER_H_
#define APPLICATIONS_TRIGGER_H_

#include "msgs.h"

// Call to start the trigger thread. Must be done before sending a message to the thread
void trigger_init (void);

void send_to_trigger (MESSAGE event);

#endif /* APPLICATIONS_TRIGGER_H_ */
