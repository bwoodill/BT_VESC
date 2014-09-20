/*
	Copyright 2012-2014 Benjamin Vedder	benjamin@vedder.se

	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */

/*
 * servo_dec.h
 *
 *  Created on: 20 jan 2013
 *      Author: benjamin
 */

#ifndef SERVO_DEC_H_
#define SERVO_DEC_H_

#include <stdint.h>
#include <conf_general.h>

// Functions
void servodec_init(void (*d_func)(void));
void servodec_set_pulse_options(float start, float width);
float servodec_get_servo(int servo_num);
uint32_t servodec_get_time_since_update(void);

#endif /* SERVO_DEC_H_ */
