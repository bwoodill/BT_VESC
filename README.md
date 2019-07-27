# VESC firmware

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

An open source motor controller firmware.

This is BASED ON the source code for the VESC DC/BLDC/FOC controller. Read more at  
[https://vesc-project.com/](https://vesc-project.com/)

## Supported boards

All  of them!

Make sure you select your board in [conf_general.h](conf_general.h)


## Prerequisites

### On Ubuntu

Install the gcc-arm-embedded toolchain


```bash
sudo add-apt-repository ppa:team-gcc-arm-embedded/ppa
sudo apt update
sudo apt install gcc-arm-embedded
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
git clone git@github.com:claroworks-product-development/Sikorski.git sikorski
cd sikorski
make
```

Create the firmware 
```bash
make
```

Use the [Vesc Tool](https://vesc-project.com/vesc_tool) (included for ubuntu, vesc_tool_1.16) to update the firmware. The firmware is in the build/ directory - BLDC_4_ChibiOS.elf

## License

The software is released under the GNU General Public License version 3.0
