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

#ifndef APPLICATIONS_DOCS_H_
#define APPLICATIONS_DOCS_H_

/*
@startuml

[*] --> SW_OFF
SW_ON --> SW_OFF : OFF
SW_GOING_OFF --> SW_OFF : TOUT (SPEED_OFF)

SW_GOING_ON --> SW_ONE_OFF : OFF
SW_ONE_OFF --> SW_ONE_ON : ON (SPEED_ON)
SW_ONE_OFF --> SW_OFF : TOUT
SW_ONE_ON : (Turn motor on)

SW_OFF -> SW_GOING_ON : ON
SW_GOING_ON --> SW_ON : TOUT

SW_ONE_ON --> SW_GOING_OFF : OFF
SW_GOING_OFF --> SW_CLICKED : ON
SW_CLICKED --> SW_ONE_ON: TOUT (SPEED_DOWN)
SW_CLICKED --> SW_CLICKED_OFF: OFF
SW_CLICKED_OFF --> SW_OFF : TOUT (SPEED_OFF)
SW_CLICKED_OFF --> SW_ONE_ON:ON (SPEED_UP)
@enduml



@startuml

[*] --> MTR_OFF
MTR_ON --> MTR_OFF : SPEED_OFF
MTR_ON : Speed
MTR_ON -l-> MTR_ON : SPEED_UP
MTR_ON -d-> MTR_ON : SPEED_DN

MTR_START --> MTR_OFF : SPEED_OFF
MTR_START : (test for obstruction)
MTR_START --> MTR_START : OVERCURR
MTR_START --> MTR_ON : TOUT
MTR_OFF --> MTR_START : SPEED_ON
MTR_OFF --> MTR_OFF : TOUT (speed toward 3)
MTR_OFF : Speed

@enduml


*/

#endif /* APPLICATIONS_DOCS_H_ */
