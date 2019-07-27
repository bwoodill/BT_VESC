# VESC firmware

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

An open source motor controller firmware.

This is BASED ON the source code for the VESC DC/BLDC/FOC controller. Read more at  
[https://vesc-project.com/](https://vesc-project.com/)

## Supported boards

All  of them!

Make sure you select your board in [conf_general.h](conf_general.h)


## Prerequisites

On an Ubuntu machine, install the gcc-arm-embedded toolchain


```bash
sudo add-apt-repository ppa:team-gcc-arm-embedded/ppa
sudo apt update
sudo apt install gcc-arm-embedded
```


## Build

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
