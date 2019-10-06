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

#ifndef APPLICATIONS_SETTINGS_H_
#define APPLICATIONS_SETTINGS_H_

#include <stdint.h>

#define SIKORSKI_VAR_DATA \
/* type      name          $code printas default value */ \
X (uint8_t,  speed_default, $d, %i,   SPEED_DEFAULT) \
X (uint8_t,  max_speed,     $M, %i,   MAX_SPEED_SETTING) \
X (uint8_t,  use_safety,    $U, %i,   USE_SAFETY_SPEED) \
X (uint16_t, trig_on_time,  $T, %i,   TRIG_ON_TOUT_MS) \
X (uint16_t, trig_off_time, $t, %i,   TRIG_OFF_TOUT_MS) \
X (uint16_t, ramping,       $r, %i,   SPEED_RAMPING_RATE) \
X (uint32_t, migrate_rate,  $m, %i,   MIGRATE_SPEED_MILLISECONDS) \
X (float, 	 guard_high,    $G, %0.2f,SAFETY_SPEED_GUARD_HIGH) \
X (float, 	 guard_low,     $g, %0.2f,SAFETY_SPEED_GUARD_LOW) \
X (uint8_t,  safe_count,    $c, %i,   RUNNING_SAFE_OK_CT) \
X (float,    f_alpha,       $a, %0.4f,SAFETY_FILTER_ALPHA) \
X (uint8_t,  brightness,    $b, %i,   DISP_BRIGHTNESS) \
X (uint16_t, power_off_ms,  $p, %i,   DISP_POWER_ON_OFFTIME) \
X (uint16_t, disp_beg_ms,   $f, %i,   DISP_OFF_TRIGGER_BEG_MS) \
X (uint16_t, disp_dur_ms,   $D, %i,   DISP_OFF_TRIG_DURATION_MS) \
X (uint16_t, disp_on_ms,    $n, %i,   DISP_ON_TRIGGER_SPEED_MS) \
X (float,    batt_imbalance,$b, %0.2f,BATTERY_MAX_IMBALANCE) \
X (uint8_t,  logging,       $l, %02X, LOGGING_OFF) \


typedef struct
{
#define X(type,name,code,printas,defaultval) type name;
    SIKORSKI_VAR_DATA
#undef X
    float limits[8];
    uint16_t speeds[8];
    float battlevels[4];
} sikorski_data;

void app_sikorski_configure (sikorski_data *conf);

sikorski_data* get_sikorski_settings_ptr (void);

// process a command from terminal.c. Only commands that start with '$' are directed here
void settings_command (char *command);
void print_all (const char *data);

void sikorski_set_defaults (sikorski_data *destination);

// DEBUGGING SETTINGS
#define SPEED_LOG   1<<0
#define SAFETY_LOG  1<<1
#define DISPLAY_LOG 1<<2
#define TRIGGER_LOG 1<<3

#endif /* APPLICATIONS_SETTINGS_H_ */
