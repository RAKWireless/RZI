# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rzi_fuota_agent)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/lorawan)
set(FUOTA_SAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/lorawan/fuota/src)
target_include_directories(app PRIVATE ${FUOTA_SAMPLE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/uplink_log/include)
target_sources(app PRIVATE src/main.c src/callback_log.c ${FUOTA_SAMPLE_DIR}/fuota_event_log.c
  ${FUOTA_SAMPLE_DIR}/fuota_receive_demo.c)
