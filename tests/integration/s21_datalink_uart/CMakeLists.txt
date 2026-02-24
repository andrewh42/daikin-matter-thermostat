#
# SPDX-License-Identifier: LicenseRef-Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(s21_datalink_uart_test)

target_sources(app PRIVATE
  src/main.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/s21/S21DataLinkUart.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/s21/S21Frame.cpp
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../src
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party
)
