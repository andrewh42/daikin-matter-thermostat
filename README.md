# Daikin Matter thermostat

This is a very early stage hardware/software project to connect certain Daikin air conditioners
to a Thread-based Matter network.

The project is currently in its formative stages so will not be very useful to most - unless you've
been struggling to get a XIAO nRF54L15 working with Matter.


## Requirements

### Hardware requirements

The project is based on the following hardware:

* A Daikin air conditioner indoor unit that contains an [S21](https://github.com/revk/ESP32-Faikout/wiki/Wiring#s21-connector) port.
* A Nordic nRF5340 or nRF54L15 microcontroller-based board,
e.g. [Nordic nRF5340 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF5340-DK),
[Nordic nRF54L15 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF54L15-DK) or
[Seeed XIAO nRF54L15](https://wiki.seeedstudio.com/xiao_nrf54l15_sense_getting_started/).
The Seeed XIAO is the recommended option due to its small size and low cost.
* A 16V-capable DC-DC converter to reduce the DC supply from the S21 port down to 3.3V for use
by the microcontroller. A [Sparkfun AP63203 breakout board](https://www.sparkfun.com/sparkfun-babybuck-regulator-breakout-3-3v-ap63203.html) is a relatively efficient and low cost option.


### Software requirements

The software has been built and tested using the nRF Connect Visual Studio Code plugin with
nRF Connect SDK v3.2.1.


## Building

### Hardware

TODO

### Software

Open this repo in Visual Studio Code and follow these steps:

1. Select the nRF Connect view using the sidebar on the left hand side.

2. Start a terminal configured for the nRF Connect SDK by clicking on `Open terminal` under the view's Welcome section.

3. Create a build configuration and build the thermostat software.

    For the Seeed XIAO nRF54L15:
    ```sh
    west build -p -d build-xiao -b xiao_nrf54l15/nrf54l15/cpuapp -- \
      -DCONFIG_CHIP_DFU_OVER_BT_SMP=y -DFILE_SUFFIX=internal \
      -DEXTRA_CONF_FILE=prj_release.conf
    ```

    For the Nordic nRF54L15 DK:
    ```sh
    west build -p -d build-nrf54l15dk -b nrf54l15dk/nrf54l15/cpuapp -- \
      -DCONFIG_CHIP_DFU_OVER_BT_SMP=y -DFILE_SUFFIX=internal \
      -DEXTRA_CONF_FILE=prj_release.conf
    ```

    For the Nordic nRF5340 DK:
    ```sh
    west build -p -d build-nrf5430dk -b nrf5340dk/nrf5340/cpuapp -- \
      -DEXTRA_CONF_FILE=prj_release.conf
    ```

4. Flash the software.


#### Debug builds

Remove the `-DEXTRA_CONF_FILE=prj_release.conf` from the end of the `west build` commands above
to create debug builds to play around with.


## Licence

This software is licenced under the [Apache Licence 2.0](https://spdx.org/licenses/Apache-2.0.html),
except for files containing a [SPDX-License-Identifier](https://spdx.dev/learn/handling-license-info/)
in which case the licence referenced by the SPDX licence identifier applies.
