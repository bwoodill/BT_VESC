# VESC firmware for Blacktip DPV

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

An open source motor controller firmware for the Blacktip DPV with additional added features.

This is BASED ON the source code for the VESC DC/BLDC/FOC controller and source from Dive Xtras. Read more at  
[https://vesc-project.com/](https://vesc-project.com/) and https://github.com/claroworks-product-development/Sikorski

# DISCLAIMER

Please note: everything in this repo is released for use "AS IS" without any warranties of any kind, including, but not limited to their installation, use, or performance. We disclaim any and all warranties, either express or implied, including but not limited to any warranty of noninfringement, merchantability, and/ or fitness for a particular purpose. 

Any use of this repo is at your own risk. There is no guarantee that anything has been through testing. We are not responsible for any damage, data loss, death, injury, or property damage incurred with its use.

You are responsible for reviewing and testing anything in this repo before using.

# Added Features
* Increased ERPM speed limit to 6000.
* Added an additional speed (Speed 9).
  * This can be set in terminal with $S9 and $L9. Default is speed9 5500 and limit9 29.
* Increased Limit to 30 amp from 23 amp.
* Increase Motor current max to 49 amp from 40 amp.
* Changed defaults (Use Safety 0, Disp Rot 0).
* Added feature where three clicks will go to jump speed.
  * This can be set in terminal with $J to control new jump speed setting. The default is 6.
* Added feature where four clicks enables cruise control.
  * Clicking once (holding or releasing) will cancel cruise. Any additional clicks are processed after disabling cruise control. For example: with cruise on, clicking twice and holding the second one results in cruise turning off and shifting down.
* Added a low speed migrate option that is defaulted to off. If turned on, it will not migrate your speed if your current speed is < default speed. Controlled with $e.

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

Create the firmware 
```bash
make
```

Use the [Vesc Tool](https://vesc-project.com/vesc_tool) (included for ubuntu, vesc_tool_1.16) to update the firmware. The firmware is in the build/ directory - BLDC_4_ChibiOS.bin

## License

The software is released under the GNU General Public License version 3.0
