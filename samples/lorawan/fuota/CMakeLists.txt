# SPDX-FileCopyrightText: Copyright (c) 2026 RAKwireless
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rzi_lorawan_fuota)

set(UPLINK_LOG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/uplink_log)

target_sources(app PRIVATE
  src/main.c
  src/uplink_log_rzi.c
  ${UPLINK_LOG_DIR}/src/uplink_log.c
  ${UPLINK_LOG_DIR}/src/uplink_log_frame.c
)
target_sources_ifdef(CONFIG_RZI_LORAWAN_FUOTA_AGENT app PRIVATE src/fuota_event_log.c)
target_sources_ifdef(CONFIG_APP_FUOTA_RECEIVE_DEMO app PRIVATE src/fuota_receive_demo.c)
target_include_directories(app PRIVATE ${UPLINK_LOG_DIR}/include)
