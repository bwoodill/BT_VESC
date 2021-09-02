# VESC firmware for Blacktip DPV

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

An open source motor controller firmware for the Blacktip DPV with additional added features.

Please use the issue tracker here for issues, suggestions, etc. There is also a discussion on scubaboard here https://www.scubaboard.com/community/threads/blacktip-firmware-modifications.609780/

Releases can be found here https://github.com/bwoodill/BT_VESC/releases

This is BASED ON the source code for the VESC DC/BLDC/FOC controller and source from Dive Xtras. Read more at  
[https://vesc-project.com/](https://vesc-project.com/) and https://github.com/claroworks-product-development/Sikorski

# DISCLAIMER

This firmware will allow you to increase speed and current to a level that can damage the motor. Please be very careful if increasing the limits.

Please note: everything in this repo is released for use "AS IS" without any warranties of any kind, including, but not limited to their installation, use, or performance. We disclaim any and all warranties, either express or implied, including but not limited to any warranty of noninfringement, merchantability, and/ or fitness for a particular purpose. 

Any use of this repo is at your own risk. There is no guarantee that anything has been through testing. We are not responsible for any damage, data loss, death, injury, or property damage incurred with its use.

You are responsible for reviewing and testing anything in this repo before using.

# Added Features
* Increased ERPM speed limit to 6000.
* Added an additional speed (Speed 9).
  * This can be set in terminal with $S9 and $L9. Default is speed9 5000 and limit9 23. Be careful if increasing past default. High values can cause permanent damage to the motor.
* Increased Limit to 30 amp from 23 amp.
* Changed defaults (Use Safety 0, Disp Rot 0).
* Added feature where three clicks will go to jump speed.
  * This can be set in terminal with $J to enable the feature and $K to set the speed. The default is disabled and speed 6.
* Added feature where four clicks enables cruise control.
  * Clicking once (holding or releasing) will cancel cruise. Any additional clicks are processed after disabling cruise control. For example: with cruise on, clicking twice and holding the second one results in cruise turning off and shifting down. This can be enabled with $C.
* Added a low speed migrate option that is defaulted to off. If turned on, it will not migrate your speed if your current speed is < default speed. Controlled with $e.
* Added reverse feature.
  * Reverse mode is enabled with 5 clicks. It will stop the motor and reverse the direction of spin. This can be enabled with $E. The display will show R on the screen until you start it again (double click). It will also show R and then the speed you are in. The speeds and other functions are the same, except they are cut in half. When you click the trigger 5 times again, it will display an F until the motor is turned on again. While in reverse, the speed will not migrate up to your default speed (ex. if in speed1, it will restart in speed1 and not migrate to speed3). When entering reverse or going back into forward, it will set your speed to 1.

# Terminal Commands and Defaults
```
$$ BT_VESC Settings
$# (reset all)
$w magic 27F3
$d speed_default 3
$M max_speed 9
$U use_safety 0
$T trig_on_time 400
$t trig_off_time 500
$r ramping 1500
$m migrate_rate 5000
$G guard_high 6.00
$g guard_low 0.50
$h guard_limit 0.30
$j guard_erpm 900
$k guard_max_erpm 1500
$c safe_count 50
$F fail_count 5
$a f_alpha 0.2000
$b brightness 6
$R disp_rotation 0
$p power_off_ms 10000
$f disp_beg_ms 3000
$D disp_dur_ms 6000
$n disp_on_ms 3500
$i batt_imbalance 2.00
$x b2Rratio 14.00
$l logging 00
$C cruise 0
$J jump 0
$K jump_speed 6
$e low_migrate 0
$E reverse 0
$S1 speeds1 1525
$S2 speeds2 2300
$S3 speeds3 3100
$S4 speeds4 3525
$S5 speeds5 3900
$S6 speeds6 4150
$S7 speeds7 4450
$S8 speeds8 4850
$S9 speeds9 5000
$L1 limits1 1.00
$L2 limits2 2.20
$L3 limits3 3.80
$L4 limits4 6.20
$L5 limits5 9.60
$L6 limits6 12.80
$L7 limits7 17.00
$L8 limits8 22.80
$L9 limits9 23.00
$B1 levels1 34.00
$B2 levels2 36.00
$B3 levels3 38.00
```

## Prerequisites

### On Ubuntu

Install the gcc-arm-embedded toolchain
You can download it from here
https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain/gnu-rm/downloads
and unpack it in /usr/local. Make sure to use the gcc-arm-none-eabi-7-2018-q2.

* Unpack the archive in the file manager by right-clicking on it and extract here
* From a terminal, run
```bash
cp -RT gcc-arm-none-eabi-7-2018-q2 /usr/local
```

## Build

Build and flash the [bootloader](https://github.com/vedderb/bldc-bootloader) first

Clone and build the firmware

```bash
git clone https://github.com/bwoodill/BT_VESC.git
cd BT_VESC
make
```

Create the Firmware 
```bash
make
```

Use the [Vesc Tool](https://vesc-project.com/vesc_tool) to update the firmware. The firmware is in the build directory - BLDC_4_BT_VESC.bin

## License

The software is released under the GNU General Public License version 3.0
