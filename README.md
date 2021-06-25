# VESC firmware for Blacktip DPV

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

An open source motor controller firmware for the Blacktip DPV with additional added features.

This is BASED ON the source code for the VESC DC/BLDC/FOC controller. Read more at  
[https://vesc-project.com/](https://vesc-project.com/)

# DISCLAIMER
----------
Please note: everything in this repo is released for use "AS IS" without any warranties of any kind, including, but not limited to their installation, use, or performance. We disclaim any and all warranties, either express or implied, including but not limited to any warranty of noninfringement, merchantability, and/ or fitness for a particular purpose. 

Any use of this repo is at your own risk. There is no guarantee that anything has been through testing. We are not responsible for any damage, data loss, death, injury, or property damage incurred with its use.

You are responsible for reviewing and testing anything in this repo before using.

## Prerequisites

### On Ubuntu

Install the gcc-arm-embedded toolchain


```bash
sudo add-apt-repository ppa:team-gcc-arm-embedded/ppa
sudo apt update
sudo apt install gcc-arm-embedded
```

Add udev rules to use the stlink v2 programmer without being root


```bash
wget vedder.se/Temp/49-stlinkv2.rules
sudo mv 49-stlinkv2.rules /etc/udev/rules.d/
sudo udevadm trigger
```

### On MacOS

Go to the [GNU ARM embedded toolchain downloads Website](https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain/gnu-rm/downloads) and select the mac version, download it and extract it to your user directory.

Append the bin directory to your **$PATH**. For example:


```bash
export PATH="$PATH:/Users/your-name/gcc-arm-none-eabi-8-2019-q3-update/bin/"
```

Install stlink and openocd


```bash
brew install stlink
brew install openocd
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

Use the [Vesc Tool](https://vesc-project.com/vesc_tool) (included for ubuntu, vesc_tool_1.16) to update the firmware. The firmware is in the build/ directory - BLDC_4_ChibiOS.elf

## License

The software is released under the GNU General Public License version 3.0
