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

#ifndef APPLICATIONS_BATTERIES_H_
#define APPLICATIONS_BATTERIES_H_

#define BATTERY_CHECK_COUNTS 200  // 5 seconds based on 40Hz calling rate

#define K 1000.0
#define V2_Rtop     (141*K) // (147*K)
#define V2_Rbottom  (10*K)


// check_batteries() checks the for the situation where there is an imbalance in the
// battery charge between two batteries. In this case we want to indicate
// to the user which one needs is defective, and stop discharging by
// turning off the motor.
// It should be called at periodically. When called BATTERY_CHECK_COUNTS times,
// it sends a battery status message to the SPEED thread and possibly the DISPLAY thread.
void check_batteries (void);

// this reads the last calculated battery voltage, of whichever battery is lowest.
float get_lowest_battery_voltage(void);


#endif /* APPLICATIONS_BATTERIES_H_ */
