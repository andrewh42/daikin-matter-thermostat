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
nRF Connect SDK v3.3.0.


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
      -DFILE_SUFFIX=internal \
      -DEXTRA_CONF_FILE=prj_release.conf
    ```

    For the Seeed XIAO nRF54L15 with an external antenna attached:
    ```sh
    west build -p -d build-xiao -b xiao_nrf54l15/nrf54l15/cpuapp -- \
      -DFILE_SUFFIX=internal \
      -DEXTRA_DTC_OVERLAY_FILE=xiao-external-antenna.overlay \
      -DEXTRA_CONF_FILE=prj_release.conf

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

    For the nRF54L15 boards (XIAO + DK), production builds provision the MCUboot
    public key into the on-chip KMU. Flash with `--erase` so the key is provisioned;
    the device will not boot a signed image otherwise:
    ```sh
    west flash -d build-xiao --erase
    ```


#### Production signing key

The nRF54L15 builds sign the firmware (and DFU images) with an Ed25519 key and
store the public verification key in the SoC's hardware Key Management Unit (KMU).
The private key is **not** committed to this repo (it is gitignored), so you must
generate it once before building:

```sh
mkdir -p keys
python3 /opt/nordic/ncs/v3.3.0/bootloader/mcuboot/scripts/imgtool.py \
    keygen -t ed25519 -k keys/mcuboot_ed25519_priv.pem
```

(Run from an nRF Connect SDK terminal so `imgtool`'s Python dependencies are available.)

**Back this key up to secure offline storage immediately.** If it is lost, no
future DFU image can ever be signed for already-deployed devices. The build then
picks the key up automatically via `BOOT_SIGNATURE_KEY_FILE` in `Kconfig.sysbuild`.
Flash production devices with `west flash --erase` to provision the KMU.


#### Debug builds

Remove the `-DEXTRA_CONF_FILE=prj_release.conf` from the end of the `west build` commands above
to create debug builds to play around with.


## Testing

### Unit tests

Unit tests run on the host and do not require any hardware. They use [Catch2](https://github.com/catchorg/Catch2) and are built with CMake.

```sh
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests
```

### Integration tests

Integration tests run on the Seeed XIAO nRF54L15 and require a loopback wire connecting the S21 UART TX pin to the RX pin.

Build:
```sh
west build -p -d build-integration-s21uart \
  -b xiao_nrf54l15/nrf54l15/cpuapp \
  tests/integration/s21_datalink_uart \
  -- -DFILE_SUFFIX=internal
```

Flash and observe test results over the serial console:
```sh
west flash -d build-integration-s21uart
```


### Manual tests

A set of `s21` shell commands allows you to interact with the S21 port.

For example:
* `s21 set_operation on heat 30 midhigh`
* `s21 get_operation`


## Using

Details for adding the device to your Matter controller (e.g. Apple Home) are contained in the build
file `build-<name>/daikin-matter-airconditioner/zephyr/factory_data.txt`.


## Licence

This software is licenced under the [Apache Licence 2.0](https://spdx.org/licenses/Apache-2.0.html),
except for files containing a [SPDX-License-Identifier](https://spdx.dev/learn/handling-license-info/)
in which case the licence referenced by the SPDX licence identifier applies.
