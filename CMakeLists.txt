#
# Copyright (c) 2023 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(thermostat)

# Enable GNU STD support.
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)

# NORDIC SDK APP START

# Include all source files that are located in the Matter common directory.
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/source_common.cmake)

# Include Data Model utils
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/data_model.cmake)

include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/zap_helpers.cmake)
ncs_get_zap_parent_dir(ZAP_PARENT_DIR)

target_include_directories(app PRIVATE
  src
  third_party
  ${ZAP_PARENT_DIR}
)

target_sources(app PRIVATE
  src/airconditioner_manager.cpp
  src/app_task.cpp
  src/main.cpp
  src/s21/s21_datalink_uart.cpp
  src/s21/s21_frame.cpp
  src/s21/s21_manager.cpp
  src/s21/s21_presentation.cpp
  src/s21/s21_presentation_sync_adapter.cpp
  src/s21/s21_shell.cpp
  src/s21/s21_stack.cpp
  src/zcl_callbacks.cpp
)

ncs_configure_data_model()
# NORDIC SDK APP END
