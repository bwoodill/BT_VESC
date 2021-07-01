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
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "settings.h"
#include "conf_general.h"
#include "commands.h"
#include "datatypes.h"
#include "app.h"
#include "defaults.h"
#include "app_version.h"

static sikorski_data *settings;

void app_sikorski_configure (sikorski_data *conf)
{
    settings = conf;
}

sikorski_data* get_sikorski_settings_ptr (void)
{
    return settings;
}

#define X(type,name,code,printas,defaultval) bool name(const char * data);
SIKORSKI_VAR_DATA
#undef X
bool set_limits (int index, const char *data);
bool set_speeds (int index, const char *data);
bool set_battlevels (int index, const char *data);

void save_all_settings (void);

// process a command from terminal.c. Only commands that start with '$' are directed here
void settings_command (char *command)
{
    if (strncmp (command, "$$", 2) == 0)
    {
        print_all (&command[2]);
        return;
    }

    if (strncmp (command, "$#", 2) == 0)
    {
        sikorski_set_defaults (settings);
        return;
    }

    bool result = false;
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#define X(type,name,code,printas,defaultval) \
		if (strncmp(command, #code , 2) == 0) \
		{ 	result = name(&command[2]); \
			commands_printf(#code " " #name " " #printas , settings->name ); \
		}
    SIKORSKI_VAR_DATA
#undef X
#pragma GCC diagnostic pop
    if (strncmp (command, "$S", 2) == 0)
    {
        char in[2] = " ";
        in[0] = command[2];
        uint8_t index = atoi (in);
        result = set_speeds (index - 1, &command[3]);
        commands_printf ("$S%i speeds%i %i", index, index, settings->speeds[index - 1]);
    }
    if (strncmp (command, "$L", 2) == 0)
    {
        char in[2] = " ";
        in[0] = command[2];
        uint8_t index = atoi (in);
        result = set_limits (index - 1, &command[3]);
        commands_printf ("$L%i limits%i %0.2f", index, index, (double) settings->limits[index - 1]);
    }
    if (strncmp (command, "$B", 2) == 0)
    {
        char in[2] = " ";
        in[0] = command[2];
        uint8_t index = atoi (in);
        result = set_battlevels (index - 1, &command[3]);
        commands_printf ("$B%i levels%i %0.2f", index, index, (double) settings->battlevels[index - 1]);
    }

    if (result)
        save_all_settings ();
}

void save_all_settings (void)
{
    // get the pointer to the configuration
    const app_configuration *the_conf;
    the_conf = app_get_configuration ();

    // store the configuration to EEPROM
    conf_general_store_app_configuration ((app_configuration*) the_conf);
}

void sikorski_set_defaults (sikorski_data *destination)
{
#define X(type,name,code,printas,defaultval) destination->name = defaultval;
    SIKORSKI_VAR_DATA
#undef X
    destination->speeds[0] = SPEEDS1;
    destination->speeds[1] = SPEEDS2;
    destination->speeds[2] = SPEEDS3;
    destination->speeds[3] = SPEEDS4;
    destination->speeds[4] = SPEEDS5;
    destination->speeds[5] = SPEEDS6;
    destination->speeds[6] = SPEEDS7;
    destination->speeds[7] = SPEEDS8;
    destination->speeds[8] = SPEEDS9; // New
    destination->speeds[9] = SPEEDSA; // New

    destination->limits[0] = LIMITS1;
    destination->limits[1] = LIMITS2;
    destination->limits[2] = LIMITS3;
    destination->limits[3] = LIMITS4;
    destination->limits[4] = LIMITS5;
    destination->limits[5] = LIMITS6;
    destination->limits[6] = LIMITS7;
    destination->limits[7] = LIMITS8;
    destination->limits[8] = LIMITS9; //New
    destination->limits[9] = LIMITSA; //New

    destination->battlevels[0] = DISP_BATT_VOLT1;
    destination->battlevels[1] = DISP_BATT_VOLT2;
    destination->battlevels[2] = DISP_BATT_VOLT3;
}

void print_all (const char *data)
{
    (void) data;

    commands_printf ("$$ BT_VESC Settings:\n  ----------");
    commands_printf ("$# (reset all)");

#pragma GCC diagnostic ignored "-Wdouble-promotion"
#define X(type,name,code,printas,defaultval) \
		commands_printf(#code " " #name " " #printas , settings->name );
    SIKORSKI_VAR_DATA
#undef X
#pragma GCC diagnostic pop

    int i;
    for (i = 1; i <= MAX_SPEED_SETTING; i++)
        commands_printf ("$S%i speeds%i %i", i, i, settings->speeds[i - 1]);
    for (i = 1; i <= MAX_SPEED_SETTING; i++)
        commands_printf ("$L%i limits%i %0.2f", i, i, (double) settings->limits[i - 1]);
    for (i = 1; i <= BATT_LEVELS; i++)
        commands_printf ("$B%i levels%i %0.2f", i, i, (double) settings->battlevels[i - 1]);
    commands_printf ("    ---- %s ----", APP_VERSION);
}

bool use_safety (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    settings->use_safety = i ? 1 : 0;
    return true;
}

bool magic (const char *data)
{
    unsigned int i;
    int num = sscanf (data, "%x", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    return true;
}

bool speed_default (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 1 || i > settings->max_speed)
    {
        commands_printf ("out of range. (See max_speed)\n");
        return false;
    }
    settings->speed_default = i;
    return true;
}

bool max_speed (const char *data) // Increased limit
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 1 || i > 9)
    {
        commands_printf ("out of range. (1-9)\n");
        return false;
    }
    settings->max_speed = i;
    return true;
}

bool trig_on_time (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 200 || i > 1200)
    {
        commands_printf ("out of range. (200-1200)\n");
        return false;
    }
    settings->trig_on_time = i;
    return true;
}
bool trig_off_time (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 200 || i > 1200)
    {
        commands_printf ("out of range. (200-1200)\n");
        return false;
    }
    settings->trig_off_time = i;
    return true;
}
bool ramping (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 200 || i > 10000)
    {
        commands_printf ("out of range. (200-10000)\n");
        return false;
    }
    settings->ramping = i;
    return true;
}
bool migrate_rate (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 500 || i > 1000000)
    {
        commands_printf ("out of range. (500-1000000)\n");
        return false;
    }
    settings->migrate_rate = i;
    return true;
}
bool guard_high (const char *data)
{
    float x;
    int num = sscanf (data, "%f", &x);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (x < 0.5 || x > 6)
    {
        commands_printf ("out of range. (0.5 - 6)\n");
        return false;
    }
    settings->guard_high = x;
    return true;
}
bool guard_low (const char *data)
{
    float x;
    int num = sscanf (data, "%f", &x);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (x < 0.5 || x > 2.5)
    {
        commands_printf ("out of range. (0.5 - 2.5)\n");
        return false;
    }
    settings->guard_low = x;
    return true;
}
bool guard_limit(const char *data)
{
    float x;
    int num = sscanf (data, "%f", &x);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (x < 0.15 || x > 1.0)
    {
        commands_printf ("out of range. (0.15 - 1.0)\n");
        return false;
    }
    settings->guard_limit = x;
    return true;
}
bool guard_erpm (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 300 || i > 1200)
    {
        commands_printf ("out of range. (300-1200)\n");
        return false;
    }
    settings->guard_erpm = i;
    return true;
}
bool guard_max_erpm(const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 600 || i > 2000)
    {
        commands_printf ("out of range. (600-2000)\n");
        return false;
    }
    settings->guard_max_erpm = i;
    return true;
}
bool safe_count (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 3 || i > 100)
    {
        commands_printf ("out of range. (3-100)\n");
        return false;
    }
    settings->safe_count = i;
    return true;
}
bool fail_count (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 3 || i > 50)
    {
        commands_printf ("out of range. (3-50)\n");
        return false;
    }
    settings->fail_count = i;
    return true;
}
bool f_alpha (const char *data)
{
    float x;
    int num = sscanf (data, "%f", &x);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (x < 0.001 || x > 0.5)
    {
        commands_printf ("out of range. (0.001 - 0.5)\n");
        return false;
    }
    settings->f_alpha = x;
    return true;
}
bool brightness (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 0 || i > 15)
    {
        commands_printf ("out of range. (0-15)\n");
        return false;
    }
    settings->brightness = i;
    return true;
}
bool disp_rotation (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 0 || i > 3)
    {
        commands_printf ("out of range. (0-3)\n");
        return false;
    }
    settings->disp_rotation = i;
    return true;
}
bool power_off_ms (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 0 || i > 20000)
    {
        commands_printf ("out of range. (0-20000)\n");
        return false;
    }
    settings->power_off_ms = i;
    return true;
}
bool disp_beg_ms (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 0 || i > 6000)
    {
        commands_printf ("out of range. (0-6000)\n");
        return false;
    }
    settings->disp_beg_ms = i;
    return true;
}
bool disp_dur_ms (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 0 || i > 20000)
    {
        commands_printf ("out of range. (0-20000)\n");
        return false;
    }
    settings->disp_dur_ms = i;
    return true;
}
bool disp_on_ms (const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 0 || i > 20000)
    {
        commands_printf ("out of range. (0-20000)\n");
        return false;
    }
    settings->disp_on_ms = i;
    return true;
}
bool batt_imbalance (const char *data)
{
    float x;
    int num = sscanf (data, "%f", &x);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (x < 0.25 || x > 2.0)
    {
        commands_printf ("out of range. (0.25 - 2.0)\n");
        return false;
    }
    settings->batt_imbalance = x;
    return true;
}
bool b2Rratio(const char *data)
{
    float x;
    int num = sscanf (data, "%f", &x);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if(x == 0)
    {
        settings->b2Rratio = x;
        return true;
    }
    if (x < 10.0 || x > 20.0)
    {
        commands_printf ("out of range. (10.0 - 20.0)\n");
        return false;
    }
    settings->b2Rratio = x;
    return true;
}
bool logging(const char *data)
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 0 || i > 16)
    {
        commands_printf ("out of range. (0-16)\n");
        return false;
    }
    settings->logging = i;
    return true;
}

bool jump_speed(const char *data) // Jump Speed
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 1 || i > settings->max_speed)
    {
        commands_printf ("out of range. (See max_speed)\n");
        return false;
    }
    settings->jump_speed = i;
    return true;
}

bool low_migrate(const char *data) // Low speed migrate
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 0 || i > 1)
    {
        commands_printf ("out of range. (0-1)\n");
        return false;
    }
    settings->low_migrate = i;
    return true;
}

bool set_speeds (int index, const char *data) // Increased limit from 5000 to 6000
{
    int i;
    int num = sscanf (data, "%i", &i);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (i < 1000 || i > 6000)
    {
        commands_printf ("out of range. (1000-6000)\n");
        return false;
    }
    settings->speeds[index] = i;
    return true;
}

bool set_limits (int index, const char *data) // Increased limit from 23 to 30
{
    float x;
    int num = sscanf (data, "%f", &x);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (x < 0.5 || x > 30)
    {
        commands_printf ("out of range. (0.5-30)\n");
        return false;
    }
    settings->limits[index] = x;
    return true;
}

bool set_battlevels (int index, const char *data)
{
    float x;
    int num = sscanf (data, "%f", &x);
    if (num != 1)
    {
        commands_printf ("invalid input.\n");
        return false;
    }
    if (x < 32.0 || x > 42.0)
    {
        commands_printf ("out of range. (32.0 - 42.0)\n");
        return false;
    }
    settings->battlevels[index] = x;
    return true;
}

